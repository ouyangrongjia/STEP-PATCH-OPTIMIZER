"""Attach a supplied CAD boundary to already frozen mesh-derived supports.

No support/control point or interior normal is changed. CAD supplies only 3D
assembly edges and their correspondence to the new C2 boundary isocurves.
"""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy.interpolate import splrep
from .compact import side_uv, array1
from .trimmed import pcurve
from .mesh import dump, stats
from . import cad
from .adapt_interface import original_loop
from OCP.BRep import BRep_Builder, BRep_Tool
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeVertex
from OCP.BRepLib import BRepLib
from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve
from OCP.TopExp import TopExp
from OCP.TopoDS import TopoDS_Shell, TopoDS
from OCP.TopLoc import TopLoc_Location
from OCP.gp import gp_Pnt


def compact_pcurve(mapping, parameters, values):
    """Compress only scalar parameter correspondence, below 1e-8 UV RMS."""
    from OCP.Geom2d import Geom2d_BSplineCurve
    from OCP.TColgp import TColgp_Array1OfPnt2d
    from OCP.gp import gp_Pnt2d
    t, cp, degree = splrep(parameters, values, s=len(values)*1e-16, k=3)
    cp = cp[:len(t)-4]; cp[[0, -1]] = values[[0, -1]]
    uv = side_uv(mapping, cp); points = TColgp_Array1OfPnt2d(1, len(cp))
    for i, point in enumerate(uv):
        points.SetValue(i+1, gp_Pnt2d(*point))
    k, m = np.unique(t, return_counts=True)
    return Geom2d_BSplineCurve(points, array1(k), array1(m, True), degree)


def attach(mesh_result, region_path, out, tolerance=.001):
    start = time.perf_counter(); mesh_result, out = Path(mesh_result), Path(out)
    out.mkdir(parents=True, exist_ok=False)
    data = [np.load(mesh_result/f'patch_{i}.npz') for i in range(5)]
    surfaces = [cad.surface(d['poles'], d['tu'], d['tv']) for d in data]
    report = json.loads((mesh_result/'reconstruction.json').read_text(encoding='utf-8'))
    # Coons orientation is checked in the producer; old pilot files used this
    # same ordering but did not yet serialize the mappings.
    if 'sides' not in report:
        raise ValueError('Re-run the producer to record explicit side correspondence')
    sides = report['sides']
    region = cad.read_generated(region_path); edges, old_curves, owners = original_loop(region)
    starts = np.array([c.StartPoint().Coord() for c in old_curves])
    outer = []
    for fi in range(4):
        m = sides[fi][0]['mapping']; c = surfaces[fi].UIso(m['fixed']) if m['axis'] == 0 else surfaces[fi].VIso(m['fixed'])
        if m['reverse']:
            c.Reverse()
        outer.append(c)
    corners = np.linalg.norm(np.array([c.StartPoint().Coord() for c in outer])[:, None]-starts, axis=2).argmin(axis=1)
    corner_error = np.linalg.norm(np.array([c.StartPoint().Coord() for c in outer])-starts[corners], axis=1)
    if len(set(corners)) != 4 or corner_error.max() > 1e-4:
        raise ValueError('Supplied CAD loop does not match the mesh region corners')
    builder = BRep_Builder(); shell = TopoDS_Shell(); builder.MakeShell(shell)
    outer_vertices = [TopExp.FirstVertex_s(edges[int(i)], True) for i in corners]
    for v, error in zip(outer_vertices, corner_error):
        builder.UpdateVertex(v, max(BRep_Tool.Tolerance_s(v), float(error)*1.05+1e-7))
    seam_curves = []; newedges = []; inner_vertices = []
    for sid in range(8):
        options = [(fi, s['mapping']) for fi, ss in enumerate(sides) for s in ss if not s['outer'] and s['seam'] == sid]
        fi, m = min(options, key=lambda x: -x[0] if sid >= 4 else x[0])
        c = surfaces[fi].UIso(m['fixed']) if m['axis'] == 0 else surfaces[fi].VIso(m['fixed'])
        if m['reverse']:
            c.Reverse()
        seam_curves.append(c)
        if sid < 4:
            inner_vertices.append(BRepBuilderAPI_MakeVertex(c.EndPoint()).Vertex())
    for sid, c in enumerate(seam_curves):
        va, vb = (outer_vertices[sid], inner_vertices[sid]) if sid < 4 else (inner_vertices[sid-4], inner_vertices[(sid-3) % 4])
        edge = BRepBuilderAPI_MakeEdge(c, va, vb, 0., 1.).Edge()
        for fi, ss in enumerate(sides):
            for s in ss:
                if not s['outer'] and s['seam'] == sid:
                    builder.UpdateEdge(edge, pcurve(side_uv(s['mapping'], np.array([0., 1.])), [0., 1.]),
                                       surfaces[fi], TopLoc_Location(), 1e-7)
        newedges.append(edge)
    boundary = []; components = []; all_errors = []
    for fi in range(4):
        indices = [(int(corners[fi])+k) % len(edges) for k in range(int((corners[(fi+1) % 4]-corners[fi]) % len(edges)))]
        components.append(indices); m = sides[fi][0]['mapping']
        for k in indices:
            edge = edges[k]; a, b = BRep_Tool.Range_s(edge); old = BRep_Tool.Curve_s(edge, a, b)
            ts = np.linspace(a, b, 385); params = []
            for t in ts:
                point = old.Value(float(t)); query = GeomAPI_ProjectPointOnCurve(point, outer[fi])
                options = [(point.Distance(outer[fi].StartPoint()), 0.), (point.Distance(outer[fi].EndPoint()), 1.)]
                if query.NbPoints():
                    options.append((query.LowerDistance(), query.LowerDistanceParameter()))
                params.append(min(options)[1])
            params = np.asarray(params)
            if np.any(np.diff(params)*(params[-1]-params[0]) < -1e-12):
                raise ValueError('Nonmonotone CAD-to-mesh boundary correspondence')
            pc = compact_pcurve(m, ts, params)
            error = np.array([surfaces[fi].Value(*pc.Value(float(t)).Coord()).Distance(old.Value(float(t)))
                              for t in np.linspace(a, b, 1537)])
            maximum = float(error.max()); all_errors.extend(error)
            boundary.append(dict(original_edge=k, patch=fi, range=[a, b], mapping=m,
                                 projected_endpoints=params[[0, -1]].tolist(), error_mm=stats(error)))
            if maximum > tolerance:
                dump(out/'rejected_boundary.json', dict(boundary=boundary, tolerance_mm=tolerance))
                raise ValueError(f'CAD interface exceeds fixed tolerance: edge {k}, {maximum} mm')
            builder.UpdateEdge(edge, pc, surfaces[fi], TopLoc_Location(), max(BRep_Tool.Tolerance_s(edge), maximum*1.01+1e-7))
            builder.SameParameter(edge, False)
    for fi, surface in enumerate(surfaces):
        wire = BRepBuilderAPI_MakeWire()
        if fi < 4:
            for index in components[fi]:
                wire.Add(edges[index])
            wire.Add(newedges[(fi+1) % 4]); wire.Add(TopoDS.Edge_s(newedges[fi+4].Reversed())); wire.Add(TopoDS.Edge_s(newedges[fi].Reversed()))
        else:
            for edge in newedges[4:]:
                wire.Add(edge)
        if not wire.IsDone():
            raise ValueError('Assembly boundary wire is disconnected')
        face = BRepBuilderAPI_MakeFace(surface, wire.Wire(), True).Face()
        if report['uv'][fi]['orientation'] < 0:
            face = TopoDS.Face_s(face.Reversed())
        builder.Add(shell, face)
    BRepLib.SameParameter_s(shell, tolerance, True)
    cad.write_step(out/'network.step', shell); reread = cad.read_generated(out/'network.step')
    checks = cad.edge_checks(reread, 1537); topology = cad.topology(reread)
    if checks['max_curve_on_surface_mm'] > tolerance or not topology['brep_valid']:
        raise ValueError('Attached CAD boundary failed the unchanged geometric tolerance')
    result = dict(stage='CAD-assisted boundary attachment; mesh supports unchanged', status='DiagnosticOnly', can_apply=False,
                  inputs=dict(mesh_result=str(mesh_result.resolve()), interface=str(Path(region_path).resolve())),
                  tolerance_mm=tolerance, corner_error_mm=corner_error.tolist(), boundary=boundary,
                  boundary_error_mm=stats(all_errors), topology=topology, cad_checks=checks,
                  wall_seconds=time.perf_counter()-start)
    dump(out/'attachment.json', result)
    print(json.dumps(dict(boundary=result['boundary_error_mm'], topology=topology, wall_seconds=result['wall_seconds'])), flush=True)
    return result


def main():
    p = argparse.ArgumentParser()
    for name in ['mesh-result', 'region', 'output']:
        p.add_argument('--'+name, required=True)
    a = p.parse_args(); attach(a.mesh_result, a.region, a.output)


if __name__ == '__main__':
    main()
