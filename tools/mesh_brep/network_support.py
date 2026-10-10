"""Refit both sides of a moved shared network without rebuilding its junctions."""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import replace
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy.interpolate import BSpline
from shapely.geometry import Polygon
from .mesh import dump
from .partition_fit import Config, boundary_edges, loops, incidence, spline_graph_error
from .skeleton_graph import fit_support, stable_frame
from .boundary_checks import intersections, preserves_winding
from .source_normals import crease_fan_normals


def guard(source, baseline, output, restore_charts=(), geometric_guard=True):
    """Restore a conflicting curve neighborhood while keeping junctions shared."""
    from .network_fairing import incidence as curve_incidence
    from .mesh import Locator
    from .surface_boundary import certify_tube
    from .regional_rectangle import curve_polyline_error
    from .boundary_quality import measures
    root, prior, out = Path(source), Path(baseline), Path(output)
    out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    assignment = np.load(root/'chart_index.npz')['face_chart']
    if not np.array_equal(assignment, np.load(prior/'chart_index.npz')['face_chart']):
        raise ValueError('Network rollback requires identical source chart ownership')
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    old = {int(p.stem): dict(np.load(p)) for p in (prior/'curves').glob('*.npz')}
    if curves.keys() != old.keys() or any(not np.array_equal(c['chain'], old[ci]['chain']) for ci, c in curves.items()):
        raise ValueError('Curve identities changed across rollback baseline')
    selected = set(meta['required_refit_charts'])
    selected.update(int(di) for ci, c in curves.items() if not np.array_equal(c['poles'], old[ci]['poles']) for di in c['owners'])
    reference_quality = {ci: measures(c) for ci, c in old.items()} if geometric_guard else {}
    fixed = {ci for ci, c in curves.items() if set(restore_charts).intersection(map(int, c['owners']))}
    history = []; locators = {}; passed = False
    for attempt in range(16):
        failures = validate_wires(v, f, records, curves, selected)
        bad = set()
        detailed = {r['chart'] for r in failures if r.get('curves')}
        for r in failures:
            bad.update(r.get('curves', []))
            if r['chart'] not in detailed:
                bad.update(ci for ci, c in curves.items() if r['chart'] in c['owners'])
        certificate_failures = []; geometric_failures = []
        for ci, c in curves.items():
            if np.array_equal(c['poles'], old[ci]['poles']):
                continue
            if geometric_guard:
                before, after = reference_quality[ci], measures(c)
                length = max(before['length_mm'], 1e-10)
                if (after['total_tangent_turn_degrees'] > before['total_tangent_turn_degrees']*1.05+2. or
                        after['curvature_squared_integral_per_mm'] > before['curvature_squared_integral_per_mm']*1.05+.01/length):
                    bad.add(ci); geometric_failures.append(dict(curve=ci, before=before, after=after))
            pair = tuple(map(int, c['owners']))
            if pair not in locators:
                ids = np.flatnonzero(np.isin(assignment, pair)); used = np.unique(f[ids])
                locators[pair] = Locator(v[used], np.searchsorted(used, f[ids]))
            limit = float(c.get('source_tube_limit_mm', meta['source_plane_budget_mm']))
            cert = certify_tube(c['poles'], c['knots'], locators[pair], limit)
            movement, _ = curve_polyline_error(c['poles'], c['knots'], v[c['chain']], c['parameters'])
            if not cert['passed'] or movement > float(c.get('movement_budget_mm', meta['smoothing_mm']))*(1+1e-6):
                bad.add(ci); certificate_failures.append(ci)
            else:
                c.update(source_tube_certified_max_mm=cert['certified_max_mm'], movement_mm=movement)
        newly_fixed = bad-fixed
        event = dict(attempt=attempt, wire_failures=failures, source_certificate_failures=certificate_failures,
                     geometric_failures=geometric_failures, newly_restored_curves=sorted(newly_fixed)); history.append(event)
        if not failures and not certificate_failures and not geometric_failures and not (attempt == 0 and fixed):
            passed = True; break
        if not newly_fixed and not (attempt == 0 and fixed):
            break
        fixed.update(bad)
        anchors = {int(vi) for ci in fixed for vi in old[ci]['chain'][[0, -1]]}
        for ci in fixed:
            curves[ci] = dict(old[ci])
        # A linear displacement field has an exact B-spline representation and
        # spreads the endpoint correction across the curve instead of inserting
        # a high-frequency endpoint bump. Both uses of every vertex agree.
        for ci, c in curves.items():
            if ci in fixed:
                continue
            ends = c['chain'][[0, -1]]
            delta = np.zeros((2, 3))
            for k, vi in enumerate(ends):
                if int(vi) in anchors:
                    delta[k] = old[ci]['poles'][0 if k == 0 else -1]-c['poles'][0 if k == 0 else -1]
            if np.any(delta):
                t = c['knots']; g = np.array([t[i+1:i+4].mean() for i in range(len(c['poles']))])
                c['poles'] = c['poles']+(1-g[:, None])*delta[0]+g[:, None]*delta[1]
                for k, vi in enumerate(ends):
                    if int(vi) in anchors:
                        c['poles'][0 if k == 0 else -1] = old[ci]['poles'][0 if k == 0 else -1]
                selected.update(map(int, c['owners']))
        print(dict(stage='network_topology_guard', attempt=attempt, restored_curves=len(fixed)), flush=True)
    report = dict(passed=passed, restored_curves=sorted(fixed), history=history,
                  source_fit=str(root.resolve()), baseline_fit=str(prior.resolve()), geometric_guard=geometric_guard)
    dump(out/'network_guard.json', report)
    if not passed:
        return report
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for ci, c in curves.items():
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **c)
    meta.update(source_fit=str(out.resolve()), network_guard=report, required_refit_charts=sorted(selected), requires_support_refit=True)
    dump(out/'run.json', meta)
    return report


def samples(curve):
    cuts = np.unique(np.r_[curve['parameters'], curve['knots']])
    q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
    return BSpline(curve['knots'], curve['poles'], 3)(q)


def validate_wires(vertices, triangles, records, curves, selected):
    lookup = {}
    for ci, c in curves.items():
        for a, b in zip(c['chain'][:-1], c['chain'][1:]):
            lookup[tuple(sorted((int(a), int(b))))] = (ci, int(a))
    sampled = {ci: samples(c) for ci, c in curves.items()}
    failures = []
    for di in sorted(selected):
        rec = records[di]; used = rec['vertex_ids']
        tri = np.searchsorted(used, triangles[rec['triangle_ids']])
        o, ax, lo, size, _, _, _ = stable_frame(vertices[used], tri)
        polygons = []; involved = set()
        for loop in loops(boundary_edges(tri)):
            refs = []
            for a, b in zip(used[loop], np.roll(used[loop], -1)):
                ci, av = lookup[tuple(sorted((int(a), int(b))))]
                item = (ci, int(a) == av)
                if not refs or refs[-1] != item:
                    refs.append(item)
            if len(refs) > 1 and refs[0] == refs[-1]:
                refs.pop()
            xyz = np.concatenate([(sampled[ci] if forward else sampled[ci][::-1])[:-1] for ci, forward in refs])
            xy = (xyz-o)@ax[:2].T
            if not preserves_winding((vertices[used[loop]]-o)@ax[:2].T, xy):
                failures.append(dict(chart=di, kind='reversed_loop'))
            polygons.append(xy); involved.update(ci for ci, _ in refs)
        polygons.sort(key=lambda p: -abs(np.sum(p[:, 0]*np.roll(p[:, 1], -1)-p[:, 1]*np.roll(p[:, 0], -1))))
        if not Polygon(polygons[0], polygons[1:]).is_valid:
            failures.append(dict(chart=di, kind='invalid_polygon'))
        details = []
        bad = intersections(curves, involved, dict(origin=o, axes=ax, lo=lo, size=size), details=details)
        if bad:
            failures.append(dict(chart=di, kind='continuous_intersection', curves=sorted(bad), details=details))
    return failures


def refit(source, output, workers=4, normal_weight=.01, boundary_normal_weight=.02, fairness=1e-5):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if not meta.get('requires_support_refit') or not meta.get('required_refit_charts'):
        raise ValueError('A network candidate must declare all affected supports')
    cfg = replace(Config(**meta['config']), normal_weight=normal_weight,
                  boundary_normal_weight=boundary_normal_weight, fairness=fairness)
    if min(normal_weight, boundary_normal_weight, fairness) < 0:
        raise ValueError('Negative objective weight')
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    assignment = np.load(root/'chart_index.npz')['face_chart']
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
    selected = set(meta['required_refit_charts'])
    # All copies of an old common support must be treated consistently.
    while True:
        expanded = selected.union(*(set(map(int, records[di].get('shared_support_group', []))) for di in selected))
        if expanded == selected:
            break
        selected = expanded
    failures = validate_wires(v, f, records, curves, selected)
    dump(out/'wire_validation.json', dict(passed=not failures, checked_charts=len(selected), failures=failures))
    if failures:
        dump(out/'run.json', dict(status='rejected', reason='invalid_network_wires', can_apply=False))
        return None
    compatible = []
    required = set()
    for di in sorted(selected):
        try:
            error = max(spline_graph_error(c['poles'], c['knots'], records[di])
                        for c in curves.values() if di in c['owners'])
        except ValueError:
            # A tangentially moved trim may leave the old rectangular extent.
            # fit_support builds a new extent including the canonical curves.
            error = float('inf')
        if error < cfg.boundary*.4 and di not in meta.get('force_refit_charts', []):
            compatible.append(di)
        else:
            required.add(di)
    # Preserve already validated supports when the new canonical curves still
    # lie in their continuous fitting tube. Coordinate changes alone do not
    # require changing an entire neighboring surface.
    selected = required
    edges, owners = incidence(f)
    normal = crease_fan_normals(v, f, cfg.feature_angle, (edges, owners), meta['normal_filter_steps'])
    relevant = [ci for ci, c in curves.items() if selected.intersection(map(int, c['owners']))]
    boundary_normals = {}
    if boundary_normal_weight:
        from .boundary_normals import targets
        active = [curves[ci] for ci in relevant]
        target = targets(v, f, normal, assignment, edges, owners, active, [c['owners'] for c in active])
        boundary_normals = dict(zip(relevant, target))
    tasks = []
    for di in sorted(selected):
        rec = records[di]; used = rec['vertex_ids']; ids = rec['triangle_ids']
        incident = [ci for ci in relevant if di in curves[ci]['owners']]
        bn = None if not boundary_normal_weight else np.concatenate([
            boundary_normals[ci][list(curves[ci]['owners']).index(di)] for ci in incident])
        tasks.append((di, v[used], np.searchsorted(used, f[ids]), normal[ids],
                      np.concatenate([samples(curves[ci]) for ci in incident]), [curves[ci] for ci in incident],
                      cfg, 'bounded', bn))
    print(dict(stage='network_support_refit', charts=len(tasks)), flush=True)
    fitting = {r['chart']: r for r in json.loads((root/'fitting.json').read_text(encoding='utf-8'))}
    fitted = {}; failed = []
    proposals = out/'support_proposals'; proposals.mkdir()
    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(fit_support, task) for task in tasks]
        for index, future in enumerate(as_completed(futures)):
            di, rec, info = future.result()
            info.update(chart=di, region=int(records[di]['region'])); fitting[di] = info
            if rec is None:
                failed.append(di)
            else:
                rec.update({k: records[di][k] for k in ('region', 'triangle_ids', 'vertex_ids')})
                fitted[di] = rec
                np.savez_compressed(proposals/f'{di:05d}.npz', **rec)
            if (index+1)%10 == 0 or rec is None or index+1 == len(tasks):
                print(dict(stage='network_support_refit', done=index+1, total=len(tasks), chart=di,
                           accepted=rec is not None, failed=len(failed)), flush=True)
            dump(out/'fitting.json', list(fitting.values()))
    dump(out/'fitting.json', list(fitting.values()))
    report = dict(accepted=not failed, refitted_charts=sorted(fitted), reused_compatible_charts=compatible, failed_charts=failed,
                  normal_weight=normal_weight, boundary_normal_weight=boundary_normal_weight, fairness=fairness,
                  seconds=time.perf_counter()-start, source_fit=str(root.resolve()))
    dump(out/'network_support.json', report)
    if failed:
        dump(out/'run.json', dict(status='rejected', reason='support_refit_failed', can_apply=False, **report))
        return report
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    shutil.copy2(root/'chart_index.npz', out/'chart_index.npz')
    for di, rec in fitted.items():
        np.savez_compressed(out/'charts'/f'{di:05d}.npz', **rec)
    if (root/'routes').exists():
        shutil.copytree(root/'routes', out/'routes')
    meta.update(status='fitted', source_fit=str(out.resolve()), requires_support_refit=False,
                force_refit_charts=[], network_support=report, can_apply=False)
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--workers', type=int, default=4); p.add_argument('--fairness', type=float, default=1e-5)
    p.add_argument('--boundary-normal-weight', type=float, default=.02)
    a = p.parse_args(); refit(a.run, a.output, a.workers, boundary_normal_weight=a.boundary_normal_weight, fairness=a.fairness)
