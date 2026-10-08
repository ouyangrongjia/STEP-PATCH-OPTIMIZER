"""Exact common-edge assembly for mixed polynomial quad/triangle patches."""
from pathlib import Path
import time

import numpy as np

from . import cad
from .compact import curve
from .mesh import dump, stats
from .partition_cad import topology
from .regional_reconstruct import edge_uv, evaluate
from .trimmed import pcurve


def affine_triangle(poles):
    """Exact triangular polynomial on an affine UV triangle, without a pole.

The fit uses Duffy coordinates for sampling. STEP instead trims the same
polynomial by (0,0),(1,0),(0,1), so no collapsed surface side or degenerate
topological edge is needed by the receiving CAD kernel.
"""
    from math import factorial
    from .subdivision_reconstruct import triangle_transform
    from .regional_reconstruct import bernstein
    n = len(poles)-1; keys, transform = triangle_transform(n)
    origin = poles[0, 0].copy()
    cp = np.linalg.lstsq(transform, (poles-origin).reshape(-1, 3), rcond=1e-13)[0]
    t = np.linspace(0, 1, n+1)
    u, v = np.meshgrid(t, t, indexing='ij'); u, v = u.ravel(), v.ravel()
    tri_basis = np.stack([factorial(n)/(factorial(a)*factorial(b)*factorial(c))
                          *(1-u-v)**a*u**b*v**c for a, b, c in keys], axis=1)
    samples = (tri_basis@cp).reshape(n+1, n+1, 3)
    inverse = np.linalg.inv(bernstein(n, t))
    return np.einsum('ij,jkc,lk->ilc', inverse, samples, inverse)+origin


def cad_edge_uv(count, side, t, affine):
    if affine and count == 3:
        return [lambda: np.c_[t, 0*t], lambda: np.c_[1-t, t], lambda: np.c_[0*t, 1-t]][side]()
    return edge_uv(count, side, t)


def surface(poles):
    from OCP.Geom import Geom_BezierSurface
    from OCP.GeomConvert import GeomConvert
    from OCP.TColgp import TColgp_Array2OfPnt
    from OCP.gp import gp_Pnt
    p = TColgp_Array2OfPnt(1, len(poles), 1, poles.shape[1])
    for i in range(len(poles)):
        for j in range(poles.shape[1]):
            p.SetValue(i+1, j+1, gp_Pnt(*poles[i, j]))
    return GeomConvert.SurfaceToBSplineSurface_s(Geom_BezierSurface(p))


def assemble(v, patches, network, output, tolerance, affine_triangles=True):
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeEdge,
                                   BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeSolid)
    from OCP.BRepLib import BRepLib
    from OCP.TopAbs import TopAbs_FORWARD, TopAbs_REVERSED
    from OCP.TopLoc import TopLoc_Location
    from OCP.TopoDS import TopoDS, TopoDS_Shell, TopoDS_Edge
    from OCP.gp import gp_Pnt
    out = Path(output); started = time.perf_counter()
    builder = BRep_Builder(); shell = TopoDS_Shell(); builder.MakeShell(shell)
    cad_poles = [affine_triangle(p['poles']) if affine_triangles and len(p['vertices']) == 3 else p['poles'] for p in patches]
    surfaces = [surface(p) for p in cad_poles]
    vertices = {int(k): BRepBuilderAPI_MakeVertex(gp_Pnt(*v[k])).Vertex() for k in {i for key in network for i in key}}
    trim_uses = []
    for p in patches:
        if 'trim_uses' in p:
            trim_uses.append(p['trim_uses'])
        else:
            uses = []
            for side, (key, a) in enumerate(zip(p['edges'], p['vertices'])):
                forward = a == key[0]; t = np.array([0., 1.]) if forward else np.array([1., 0.])
                uses.append(dict(edge=key, forward=forward, uv=cad_edge_uv(len(p['vertices']), side, t, affine_triangles)))
            trim_uses.append(uses)
    edges = {}; max_gap = 0.; smooth = []; sharp = []; within = []
    for key, record in network.items():
        cp = record['line']+record['scale']*(record['cp']-record['line'])
        from OCP.Geom import Geom_BezierCurve
        from OCP.GeomConvert import GeomConvert
        from OCP.TColgp import TColgp_Array1OfPnt
        controls = TColgp_Array1OfPnt(1, len(cp))
        for j, point in enumerate(cp):
            controls.SetValue(j+1, gp_Pnt(*point))
        c = GeomConvert.CurveToBSplineCurve_s(Geom_BezierCurve(controls))
        edge = BRepBuilderAPI_MakeEdge(c, vertices[key[0]], vertices[key[1]], 0., 1.).Edge()
        normals = []
        for fi in record['owners']:
            use = next(x for x in trim_uses[fi] if tuple(x['edge']) == key)
            pc = pcurve(use['uv'], [0., 1.])
            builder.UpdateEdge(edge, pc, surfaces[fi], TopLoc_Location(), tolerance)
            t = np.linspace(.001, .999, 25)
            uv = (1-t[:,None])*use['uv'][0]+t[:,None]*use['uv'][1]
            xyz, n, _ = evaluate(cad_poles[fi], uv)
            target = np.array([c.Value(float(x)).Coord() for x in t])
            gap = float(np.linalg.norm(xyz-target, axis=1).max()); max_gap = max(max_gap, gap)
            if gap > tolerance:
                raise ValueError(f'Common curve is not embedded: {key}, {fi}, {gap}')
            normals.append(n)
        angles = np.degrees(np.arccos(np.clip(np.sum(normals[0]*normals[1], axis=1), -1, 1)))
        (sharp if record['sharp'] else smooth).extend(angles)
        if patches[record['owners'][0]]['region'] == patches[record['owners'][1]]['region']:
            within.extend(angles)
        builder.SameRange(edge, True); builder.SameParameter(edge, True)
        edges[key] = edge
    faces = []
    for fi, p in enumerate(patches):
        wire = BRepBuilderAPI_MakeWire()
        for side, use in enumerate(trim_uses[fi]):
            key = tuple(use['edge'])
            edge = edges[key]
            wire.Add(edge if use['forward'] else TopoDS.Edge_s(edge.Reversed()))
            if not affine_triangles and len(p['vertices']) == 3 and side == 1:
                # The rectangular polynomial support collapses its top side
                # exactly to triangle vertex C; a genuine degenerate edge is
                # required in its parameter-space wire.
                deg = TopoDS_Edge(); builder.MakeEdge(deg)
                vertex = vertices[p['vertices'][2]]
                builder.Add(deg, vertex.Oriented(TopAbs_FORWARD))
                builder.Add(deg, vertex.Oriented(TopAbs_REVERSED))
                builder.UpdateEdge(deg, pcurve(np.array([[1., 1.], [0., 1.]]), [0., 1.]),
                                   surfaces[fi], TopLoc_Location(), tolerance)
                builder.Range(deg, surfaces[fi], TopLoc_Location(), 0., 1.)
                builder.Degenerated(deg, True); wire.Add(deg)
        if not wire.IsDone():
            raise ValueError(f'Disconnected fitted face {fi}')
        face = BRepBuilderAPI_MakeFace(surfaces[fi], wire.Wire(), True).Face()
        builder.Add(shell, face); faces.append(face)
    shell.Closed(True); solid = BRepBuilderAPI_MakeSolid(shell).Solid()
    if not BRepLib.OrientClosedSolid_s(solid):
        raise ValueError('Cannot orient closed fitted solid')
    before = topology(solid)
    continuity = dict(smooth_seam_degrees=stats(smooth), sharp_feature_degrees=stats(sharp),
                      within_region_degrees=stats(within), samples_per_edge=25)
    dump(out/'assembly.json', dict(topology=before, max_curve_on_surface_mm=max_gap, continuity=continuity))
    cad.write_step(out/'model.step', solid)
    after = topology(cad.read_generated(out/'model.step'))
    return dict(topology_before=before, topology=after, continuity=continuity,
                max_curve_on_surface_mm=max_gap, fitted_faces=len(patches),
                triangle_representation='affine triangular trim' if affine_triangles else 'collapsed Duffy side',
                actual_support_control_points=sum(len(p['poles'])**2 for p in patches),
                step_bytes=(out/'model.step').stat().st_size,
                assembly_seconds=time.perf_counter()-started,
                watertight=bool(before['brep_valid'] and after['brep_valid']
                               and after['solids'] == 1 and after['free_edges'] == 0
                               and after['multiple_edges'] == 0
                               and after['maximum_edge_tolerance_mm'] <= tolerance*1.01))
