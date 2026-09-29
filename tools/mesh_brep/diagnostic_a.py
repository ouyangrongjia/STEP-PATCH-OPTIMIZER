"""CAD-assisted matched-space A comparison; never imported by B."""
import argparse
import json
import sys
import time
from pathlib import Path
import numpy as np
from .mesh import audit, boundary_loop, mean_value, distortion, Locator, vertex_normals, stats, dump
from .generate import partition
from .splines import joint_fit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, shapes, TopAbs_FACE, BRep_Tool, gp_Pnt, xyz, evaluate
from continuity_fit import fixed_start
from designer_layout import variable_maps, uv_on_side, export_layout
from spline import from_occt, to_occt, basis
from hard_boundary import parameterize
from integrate import edge_checks
from diagnose import seam_quality
from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve


def mesh_mapping(data, layout, out):
    started = time.perf_counter()
    raw = data['mesh_points']; oldtri = data['mesh_triangles']
    _, first, remap = np.unique(np.round(raw, 8), axis=0, return_index=True, return_inverse=True)
    vertices, triangles = raw[first], remap[oldtri]
    xy = ((vertices-data['center'])@data['axes'].T)[:, :2]
    outer = ((layout['outer']-data['center'])@data['axes'].T)[:, :2]
    inner = ((layout['inner']-data['center'])@data['axes'].T)[:, :2]
    if np.linalg.det(np.array([inner[1]-inner[0], inner[2]-inner[0]])) < 0:
        raise ValueError('Unexpected baseline loop orientation')
    extended = inner+10*(outer-inner)
    polygons = [np.array([extended[i], extended[(i+1) % 4], inner[(i+1) % 4], inner[i]]) for i in range(4)]+[inner]
    p, tri, labels, _ = partition(vertices, triangles, xy, polygons)
    mesh_seconds = time.perf_counter()-started
    uv = np.zeros((len(layout['surfaces']), len(p), 2)); reports = []
    started = time.perf_counter()
    for fi, surface in enumerate(layout['surfaces']):
        localtri = tri[labels == fi]; boundary = boundary_loop(localtri)
        ctrl, tu, tv, degree = from_occt(surface)
        flat = (ctrl-data['center'])@data['axes'].T; flat[:, :, 2] = 0
        surf = to_occt(flat, tu, tv, degree)
        curves = [(axis, fixed, surf.UIso(fixed) if axis == 0 else surf.VIso(fixed))
                  for axis in range(2) for fixed in [0., 1.]]
        q = (p[boundary]-data['center'])@data['axes'].T; q[:, 2] = 0
        bparam = []; residuals = []
        for point in q:
            options = []
            for axis, fixed, curve in curves:
                proj = GeomAPI_ProjectPointOnCurve(gp_Pnt(*point), curve, 0., 1.)
                if proj.NbPoints():
                    options.append((proj.LowerDistance(), axis, fixed, proj.LowerDistanceParameter()))
            d, axis, fixed, t = min(options)
            u = np.zeros(2); u[axis] = fixed; u[1-axis] = np.clip(t, 0, 1)
            bparam.append(u); residuals.append(d)
        if max(residuals) > .0031:
            raise ValueError('Mesh boundary incompatible with original curve parameters')
        uv[fi], residual = mean_value(p, localtri, boundary, np.asarray(bparam))
        quality = distortion(p, localtri, uv[fi])
        if quality['flipped'] or quality['zero']:
            raise ValueError(f'A chart failed: {quality}')
        reports.append(dict(patch=fi, topology=audit(p, localtri), distortion=quality,
                            maximum_boundary_planar_projection_mm=max(residuals), linear_residual=residual))
    uv_seconds = time.perf_counter()-started
    started = time.perf_counter(); points = np.vstack([data['train_points'], data['test_points']])
    locator = Locator(p, tri); _, d, ti, bc = locator.closest(points)
    chosen = labels[ti]; params = np.einsum('ij,ijk->ik', bc, uv[chosen[:, None], tri[ti]])
    report = dict(mesh_preparation_seconds=mesh_seconds, uv_solve_seconds=uv_seconds,
                  correspondence_seconds=time.perf_counter()-started, patches=reports,
                  exact_point_to_discrete_mesh_mm=stats(d), vertices=len(p), triangles=len(tri),
                  truth='Exact original CAD samples; their nearest discrete triangle barycentrics supply UV only. XYZ/normal truth is not silently replaced.',
                  boundary='Mesh boundary vertices projected onto flattened seed boundary curves; full original curve parameter, not arclength. Original affine curve-to-pcurve mapping retained.')
    np.savez_compressed(out/'mesh_correspondence.npz', vertices=p, triangles=tri, labels=labels, vertex_uv=uv,
                        patch=chosen, uv=params, exact_to_mesh_distance=d)
    dump(out/'mesh_correspondence.json', report)
    return chosen, params, report


def run(reference, out):
    reference, out = Path(reference), Path(out); out.mkdir(parents=True, exist_ok=False)
    run_started = time.perf_counter()
    started = time.perf_counter(); data = np.load(reference/'reference.npz')
    protocol = json.loads((reference/'protocol.json').read_text(encoding='utf-8'))
    layout, _ = fixed_start(reference, data, protocol, 'coarse')
    initial = shapes(read_step(reference/'aligned_inner_cubic/initial_layout.step'), TopAbs_FACE)
    layout['surfaces'] = [BRep_Tool.Surface_s(f).Copy() for f in initial]
    maps, seam_maps, free = variable_maps(layout)
    offsets, tuples = [], []
    for s in layout['surfaces']:
        p, tu, tv, degree = from_occt(s); offsets.append(p.reshape(-1, 3)); tuples.append((tu, tv))
    preparation_seconds = time.perf_counter()-started
    chosen, uv, mesh_report = mesh_mapping(data, layout, out)
    inverse_out = out/'inverse_mapping'; inverse_out.mkdir()
    started = time.perf_counter(); old_chosen, old_uv = parameterize(data, initial, inverse_out)
    inverse_seconds = time.perf_counter()-started
    shared_seam_samples = []
    # Mesh normals near each seed seam are used identically in both A controls.
    mn = vertex_normals(data['mesh_points'], data['mesh_triangles'])
    locator = Locator(data['mesh_points'], data['mesh_triangles'])
    for fi, sides in enumerate(layout['sides']):
        for side in sides:
            if side['outer']:
                continue
            q = uv_on_side(side['mapping'], np.linspace(.002, .998, 193))
            ps, _ = evaluate(layout['surfaces'][fi], q)
            _, _, ti, bc = locator.closest(ps)
            nn = np.einsum('ij,ijk->ik', bc, mn[data['mesh_triangles'][ti]])
            nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
            shared_seam_samples.append((fi, q, nn))
    reports = []
    for name, assignments, params, mapping_seconds in [
            ('inverse_xyz', old_chosen, old_uv, inverse_seconds),
            ('mesh_uv_xyz', chosen, uv, sum(mesh_report[k] for k in ['mesh_preparation_seconds', 'uv_solve_seconds', 'correspondence_seconds']))]:
        target = out/name; target.mkdir(); specs = []; ntrain = len(data['train_points'])
        for fi, (initial_p, (tu, tv)) in enumerate(zip(offsets, tuples)):
            mask = assignments[:ntrain] == fi
            p = initial_p.reshape(len(tu)-4, len(tv)-4, 3)
            lu = np.linalg.norm(p[-1, 0]-p[0, 0]); lv = np.linalg.norm(p[0, -1]-p[0, 0])
            specs.append(dict(uv=params[:ntrain][mask], points=data['train_points'][mask],
                              normals=data['train_normals'][mask], tu=tu, tv=tv, lengths=[lu, lv]))
        newp, solve = joint_fit(specs, maps, offsets, shared_seam_samples)
        started = time.perf_counter()
        for fi, (p, (tu, tv)) in enumerate(zip(newp, tuples)):
            layout['surfaces'][fi] = to_occt(p, tu, tv)
            np.savez_compressed(target/f'patch_{fi}.npz', poles=p, tu=tu, tv=tv)
        # Recover each exact shared master curve from its embedding, independent
        # of which incident surface was used. No separate per-side fitting.
        for sid, curve in enumerate(layout['seam_curves']):
            fi, side = next((fi, s) for fi, ss in enumerate(layout['sides']) for s in ss if not s['outer'] and s['seam'] == sid)
            t = layout['seam_knots'][sid]; greville = np.array([t[i+1:i+4].mean() for i in range(len(t)-4)])
            values, _ = evaluate(layout['surfaces'][fi], uv_on_side(side['mapping'], greville))
            master = np.linalg.solve(basis(t, 3, greville), values)
            for i, point in enumerate(master):
                curve.SetPole(i+1, gp_Pnt(*point))
        layout['inner_final'] = np.asarray([xyz(c.EndPoint()) for c in layout['seam_curves'][:4]])
        shape = export_layout(layout, target/'network.step')
        reread = read_step(target/'network.step')
        from .cad import topology
        report = dict(name=name, status='DiagnosticOnly', can_apply=False, mapping_seconds=mapping_seconds,
                      preparation_shared_seconds=preparation_seconds, solver=solve,
                      actual_support_control_points=sum(len(p)*p.shape[1] for p in newp),
                      cad_checks=edge_checks(reread, 385), topology=topology(reread), seams=seam_quality(reread),
                      brep_io_check_seconds=time.perf_counter()-started,
                      train_assignment_difference=int(np.sum(chosen[:ntrain] != old_chosen[:ntrain])),
                      limitations='Both XYZ controls change the old height-only objective, but share knots, constraints, target points, normals, fairness and shared variables with each other.')
        dump(target/'reconstruction.json', report); reports.append(report)
    dump(out/'run.json', dict(configurations=reports, mesh_mapping=mesh_report, inverse_mapping_seconds=inverse_seconds,
                             total_wall_seconds=time.perf_counter()-run_started))


def main():
    p = argparse.ArgumentParser(); p.add_argument('--reference', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); run(a.reference, a.output)


if __name__ == '__main__':
    main()
