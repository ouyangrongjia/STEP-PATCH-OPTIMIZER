"""Exact shared-boundary CAD construction for the regional fan experiment."""
from pathlib import Path
import time
import numpy as np
from . import cad
from .compact import side_uv
from .partition_cad import topology
from .trimmed import pcurve
from .mesh import dump, stats


def assemble(patches, curves, endpoints, tolerance, output):
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeEdge,
                                   BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeSolid)
    from OCP.BRepLib import BRepLib
    from OCP.TopLoc import TopLoc_Location
    from OCP.TopoDS import TopoDS, TopoDS_Shell
    from OCP.gp import gp_Pnt, gp_Vec
    started = time.perf_counter(); output = Path(output)
    surfaces = [cad.surface(p['poles'], p['tu'], p['tv']) for p in patches]
    uses = {}
    for fi, patch in enumerate(patches):
        for side, key in enumerate(patch['curves']):
            uses.setdefault(key, []).append((fi, side))
    if any(len(use) != 2 for use in uses.values()):
        raise ValueError('Every generated curve must have exactly two owning faces')
    vertices = {}; edges = {}; builder = BRep_Builder(); maximum = 0.
    internal_angles = []; interface_angles = []; singular_seam_samples = 0
    for key, entries in uses.items():
        c = curves[key]; va, vb = endpoints[key]
        for vid, point in [(va, c.StartPoint()), (vb, c.EndPoint())]:
            if vid not in vertices:
                vertices[vid] = BRepBuilderAPI_MakeVertex(point).Vertex()
        edge = BRepBuilderAPI_MakeEdge(c, vertices[va], vertices[vb], 0., 1.).Edge()
        edge_normals = []
        for fi, side in entries:
            rec = patches[fi]; mapping = rec['mappings'][side]
            forward = rec['directions'][side]
            uv = side_uv(mapping, np.array([0., 1.]) if forward else np.array([1., 0.]))
            builder.UpdateEdge(edge, pcurve(uv, [0., 1.]), surfaces[fi], TopLoc_Location(), tolerance)
            params = np.linspace(0., 1., 65)
            values = side_uv(mapping, params if forward else 1-params)
            error = max(surfaces[fi].Value(*q).Distance(c.Value(float(t))) for q, t in zip(values, params))
            maximum = max(maximum, error)
            if error > tolerance:
                raise ValueError(f'Shared curve embedding is not exact: patch {fi}, {error}')
            normal = []
            for u, v in values[1:-1:4]:
                point, du, dv = gp_Pnt(), gp_Vec(), gp_Vec()
                surfaces[fi].D1(float(u), float(v), point, du, dv)
                cross = np.cross(du.Coord(), dv.Coord()); length = np.linalg.norm(cross)
                singular_seam_samples += int(length < 1e-12)
                normal.append(cross/max(length, 1e-30)*rec['orientation'])
            edge_normals.append(np.array(normal))
        degrees = np.degrees(np.arccos(np.clip(np.sum(edge_normals[0]*edge_normals[1], axis=1), -1., 1.)))
        internal = patches[entries[0][0]]['region'] == patches[entries[1][0]]['region']
        (internal_angles if internal else interface_angles).extend(degrees)
        builder.SameRange(edge, True); builder.SameParameter(edge, True); edges[key] = edge
    shell = TopoDS_Shell(); builder.MakeShell(shell)
    for fi, rec in enumerate(patches):
        wire = BRepBuilderAPI_MakeWire()
        for key, forward in zip(rec['curves'], rec['directions']):
            wire.Add(edges[key] if forward else TopoDS.Edge_s(edges[key].Reversed()))
        if not wire.IsDone():
            raise ValueError(f'Quad wire {fi} is disconnected')
        face = BRepBuilderAPI_MakeFace(surfaces[fi], wire.Wire(), True).Face()
        if rec['orientation'] < 0:
            face = TopoDS.Face_s(face.Reversed())
        builder.Add(shell, face)
    shell.Closed(True); solid = BRepBuilderAPI_MakeSolid(shell).Solid()
    if not BRepLib.OrientClosedSolid_s(solid):
        raise ValueError('Cannot orient the assembled shell')
    before = topology(solid)
    continuity = dict(within_region_seam_degrees=stats(internal_angles), region_interface_degrees=stats(interface_angles),
                      singular_seam_samples=singular_seam_samples, sampling='16 interior samples per shared curve, exact surface D1')
    dump(output/'continuity.json', continuity)
    dump(output/'assembly.json', dict(topology=before, max_curve_on_surface_mm=maximum,
                                      construction_tolerance_mm=tolerance, seconds=time.perf_counter()-started))
    tick = time.perf_counter(); cad.write_step(output/'model.step', solid)
    write_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); reread = cad.read_generated(output/'model.step'); after = topology(reread)
    read_seconds = time.perf_counter()-tick
    return dict(topology_before=before, topology=after, max_curve_on_surface_mm=maximum,
                actual_support_control_points=sum(p['poles'].shape[0]*p['poles'].shape[1] for p in patches),
                fitted_patches=len(patches), step_bytes=(output/'model.step').stat().st_size,
                step_write_seconds=write_seconds, step_read_check_seconds=read_seconds,
                assembly_check_write_seconds=time.perf_counter()-started, continuity=continuity,
                watertight=bool(before['brep_valid'] and after['brep_valid'] and after['solids'] == 1
                               and after['free_edges'] == 0 and after['multiple_edges'] == 0
                               and after['maximum_edge_tolerance_mm'] <= tolerance*1.01))
