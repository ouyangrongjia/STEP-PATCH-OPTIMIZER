"""Adapt a five-chart mesh reconstruction to a supplied CAD interface.

This is explicitly CAD-assisted assembly preparation, not the pure mesh backend.
The mesh reconstruction supplies the interior geometry and all eight seams.
Only the four outer boundary chains are replaced by the exact assembly interface.
"""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, evaluate, xyz, gp_Pnt, BRep_Tool
from spline import from_occt, to_occt, basis, array1
from designer_layout import original_loop, iso_mapping, uv_on_side, export_layout
from OCP.GeomConvert import GeomConvert_CompCurveToBSplineCurve
from OCP.GeomFill import GeomFill_BSplineCurves, GeomFill_CoonsStyle
from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve
from .mesh import dump, stats
from .cad import surface, read_generated, topology, edge_checks


def curve_points(curve, t):
    return np.asarray([xyz(curve.Value(float(x))) for x in t])


def make_layout(mesh_result, region):
    data = [np.load(Path(mesh_result)/f'patch_{i}.npz') for i in range(5)]
    pure = [surface(d['poles'], d['tu'], d['tv']) for d in data]
    edges, source, owners = original_loop(region)
    source_corners = np.asarray([xyz(c.StartPoint()) for c in source])
    mesh_corners = np.asarray([xyz(s.Value(0, 0)) for s in pure[:4]])
    corner = np.linalg.norm(mesh_corners[:, None]-source_corners, axis=2).argmin(axis=1).tolist()
    corner_change = np.linalg.norm(mesh_corners-source_corners[corner], axis=1)
    if len(set(corner)) != 4 or max(corner_change) > 1e-4:
        raise ValueError('Mesh chart corners do not identify the supplied CAD interface')
    seam_curves = [s.UIso(0).Copy() for s in pure[:4]]+[s.VIso(1).Copy() for s in pure[:4]]
    for i in range(4):
        seam_curves[i].SetPole(1, gp_Pnt(*source_corners[corner[i]]))
    surfaces, sides, outer_components, boundary, outer_curves = [], [], [], [], []
    for j in range(4):
        stop = corner[(j+1) % 4]
        indices = [(corner[j]+k) % len(edges) for k in range((stop-corner[j]) % len(edges))]
        if not indices:
            raise ValueError('Empty CAD boundary chain')
        join = GeomConvert_CompCurveToBSplineCurve(source[indices[0]].Copy())
        for k in indices[1:]:
            if not join.Add(source[k].Copy(), 1e-7, True, True, 0):
                raise ValueError('CAD interface chain is not connected')
        outer = join.BSplineCurve(); a, b = outer.FirstParameter(), outer.LastParameter()
        outer.SetKnots(array1([(outer.Knot(k)-a)/(b-a) for k in range(1, outer.NbKnots()+1)]))
        curves = [outer, seam_curves[(j+1) % 4].Copy(), seam_curves[4+j].Copy(), seam_curves[j].Copy()]
        curves[2].Reverse(); curves[3].Reverse()
        initial = GeomFill_BSplineCurves(*[c.Copy() for c in curves], GeomFill_CoonsStyle).Surface()
        om = iso_mapping(initial, outer)
        rec = [dict(outer=True, mapping=om)]
        for sid in [(j+1) % 4, 4+j, j]:
            rec.append(dict(outer=False, seam=sid, mapping=iso_mapping(initial, seam_curves[sid])))
        for k in indices:
            a, b = BRep_Tool.Range_s(edges[k]); curve = BRep_Tool.Curve_s(edges[k], a, b)
            ts = [GeomAPI_ProjectPointOnCurve(curve.Value(t), outer).LowerDistanceParameter() for t in [a, b]]
            boundary.append(dict(edge=k, patch=j, range=[a, b], normalized_endpoints=ts, mapping=om))
        surfaces.append(initial); sides.append(rec); outer_components.append(indices); outer_curves.append(outer)
    surfaces.append(pure[4].Copy())
    sides.append([dict(outer=False, seam=i, mapping=iso_mapping(surfaces[4], seam_curves[i])) for i in range(4, 8)])
    inner = np.asarray([xyz(c.EndPoint()) for c in seam_curves[:4]])
    _, normal = evaluate(pure[4], np.asarray([[.5, .5]]))
    return dict(surfaces=surfaces, sides=sides, edges=edges, owners=owners,
                outer_components=outer_components, boundary=boundary, seam_curves=seam_curves,
                corner_indices=corner, inner_final=inner, normal_axis=normal[0]), pure, outer_curves, corner_change


def fit_adapter(layout, pure, outer_curves):
    records = []
    for fi, outer in enumerate(outer_curves):
        initial = layout['surfaces'][fi]
        p0, tu, tv, degree = from_occt(initial)
        if tuple(degree) != (3, 3):
            raise ValueError('Current interface adapter requires cubic boundary curves')
        om = layout['sides'][fi][0]['mapping']
        im = next(s['mapping'] for s in layout['sides'][fi] if not s['outer'] and s['seam'] == fi+4)
        origin, end = uv_on_side(om, [0, 1]); top = uv_on_side(im, [0])[0]
        ex, ey = end-origin, top-origin
        # Arc length relates original CAD curve parameters to the mesh-only
        # boundary's declared arc-length coordinate; the CAD truth is never
        # substituted for interior mesh geometry.
        knots = np.array([outer.Knot(k) for k in range(1, outer.NbKnots()+1)])
        ts = np.unique(np.r_[np.linspace(0, 1, 4097), np.concatenate([np.linspace(a, b, 17) for a, b in zip(knots[:-1], knots[1:])])])
        cp = curve_points(outer, ts); arc = np.r_[0., np.cumsum(np.linalg.norm(np.diff(cp, axis=0), axis=1))]; arc /= arc[-1]
        def target(uv):
            q = uv-origin; t, r = q@ex, q@ey
            t, r = np.clip(t, 0, 1), np.clip(r, 0, 1)
            weight = (1-r)**3
            mapped = np.interp(t, ts, arc)
            old_uv = np.c_[(1-weight)*t+weight*mapped, r]
            old, _ = evaluate(pure[fi], old_uv)
            outer_old, _ = evaluate(pure[fi], np.c_[mapped, t*0])
            do = curve_points(outer, t)-outer_old
            left_old, _ = evaluate(pure[fi], np.c_[r*0, r])
            right_old, _ = evaluate(pure[fi], np.c_[r*0+1, r])
            dl = curve_points(layout['seam_curves'][fi], r)-left_old
            dr = curve_points(layout['seam_curves'][(fi+1) % 4], r)-right_old
            d0 = xyz(outer.StartPoint())-xyz(pure[fi].Value(0, 0))
            d1 = xyz(outer.EndPoint())-xyz(pure[fi].Value(1, 0))
            return old+weight[:, None]*do+(1-t[:, None])*dl+t[:, None]*dr-weight[:, None]*((1-t[:, None])*d0+t[:, None]*d1)
        gu = np.array([tu[k+1:k+4].mean() for k in range(len(tu)-4)])
        gv = np.array([tv[k+1:k+4].mean() for k in range(len(tv)-4)])
        uv = np.stack(np.meshgrid(gu, gv, indexing='ij'), axis=-1).reshape(-1, 2)
        values = target(uv).reshape(len(gu), len(gv), 3)
        rhs = np.linalg.solve(basis(tu, 3, gu), values.reshape(len(gu), -1)).reshape(values.shape)
        poles = np.linalg.solve(basis(tv, 3, gv), rhs.transpose(1, 0, 2).reshape(len(gv), -1)).reshape(len(gv), len(gu), 3).transpose(1, 0, 2)
        boundary_delta = max(np.max(abs(poles[[0, -1]]-p0[[0, -1]])), np.max(abs(poles[:, [0, -1]]-p0[:, [0, -1]])))
        poles[[0, -1]] = p0[[0, -1]]; poles[:, [0, -1]] = p0[:, [0, -1]]
        layout['surfaces'][fi] = to_occt(poles, tu, tv)
        q = np.stack(np.meshgrid(np.linspace(0, 1, 101), np.linspace(0, 1, 101), indexing='ij'), axis=-1).reshape(-1, 2)
        actual, _ = evaluate(layout['surfaces'][fi], q)
        records.append(dict(face=fi, controls=list(poles.shape[:2]), interpolation_boundary_roundoff_mm=float(boundary_delta),
                            target_approximation_mm=stats(np.linalg.norm(actual-target(q), axis=1))))
    return records


def boundary_coverage(layout, count=385):
    records = []
    for item in layout['boundary']:
        edge = layout['edges'][item['edge']]
        a, b = item['range']; curve = BRep_Tool.Curve_s(edge, a, b)
        t = np.linspace(a, b, count)
        fraction = (t-a)/(b-a)
        x, y = item['normalized_endpoints']
        uv = uv_on_side(item['mapping'], x+(y-x)*fraction)
        actual, _ = evaluate(layout['surfaces'][item['patch']], uv)
        error = np.linalg.norm(actual-curve_points(curve, t), axis=1)
        records.append(dict(original_edge=item['edge'], face=item['patch'], samples=count,
                            maximum_distance_mm=float(error.max())))
    maximum = max(x['maximum_distance_mm'] for x in records)
    if maximum > 1e-7:
        raise ValueError(f'Hard CAD boundary coverage failed: {maximum} mm')
    return dict(original_edges=len(records), samples=sum(x['samples'] for x in records),
                maximum_distance_mm=maximum, hard_constraint_passed=True, edges=records,
                constraint='Exact shared CAD boundary curves; samples verify their embedding, not a soft fitting weight')


def main():
    p = argparse.ArgumentParser(); p.add_argument('--mesh-result', required=True); p.add_argument('--region', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); out = Path(a.output); out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    layout, pure, outer, corner_change = make_layout(a.mesh_result, read_step(a.region))
    records = fit_adapter(layout, pure, outer)
    coverage = boundary_coverage(layout)
    export_layout(layout, out/'network.step')
    shape = read_generated(out/'network.step')
    for i, s in enumerate(layout['surfaces']):
        poles, tu, tv, degree = from_occt(s)
        np.savez_compressed(out/f'patch_{i}.npz', poles=poles, tu=tu, tv=tv, degrees=degree)
    result = dict(stage='CAD-assisted interface adaptation after pure mesh reconstruction',
                  source_mesh_reconstruction=str(Path(a.mesh_result).resolve()), interface_CAD=str(Path(a.region).resolve()),
                  changed='Outer CAD curves and tiny shared radial-corner adjustment; center surface unchanged',
                  corner_change_mm=corner_change.tolist(), boundary_coverage=coverage,
                  surfaces=records, topology=topology(shape), cad_checks=edge_checks(shape),
                  actual_support_control_points=sum(s.NbUPoles()*s.NbVPoles() for s in layout['surfaces']),
                  normal_and_smoothness='Optimization diagnostics, not a claim of exact G1', wall_seconds=time.perf_counter()-started)
    dump(out/'adaptation.json', result)
    print(json.dumps({k: result[k] for k in ['stage', 'topology', 'actual_support_control_points', 'wall_seconds']}), flush=True)


if __name__ == '__main__':
    main()
