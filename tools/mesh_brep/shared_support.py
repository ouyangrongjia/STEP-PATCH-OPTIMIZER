"""Fit a common local support while retaining each supplied region's trims.

This is a seam repair between a small set of neighboring fitting domains. The
STL observations are fitted jointly; no CAD surface is used as a target and no
source triangle or supplied region changes ownership.
"""
import argparse
import json
from pathlib import Path
import shutil
import time
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump
from .partition_fit import Config, incidence, boundary_edges, projected_boundary_valid, parameters, positions
from .skeleton_graph import fit_support, stable_frame
from .source_normals import crease_fan_normals
from .boundary_checks import intersections


def seam_samples(curve, records):
    """Native support normals at the same canonical curve parameters."""
    cuts = np.unique(np.r_[curve['parameters'], curve['knots']])
    q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
    xyz = BSpline(curve['knots'], curve['poles'], 3)(q)
    normals = []
    for di in curve['owners']:
        rec = records[int(di)]; uv = parameters(xyz, rec)
        n = np.cross(positions(uv, rec, 1, 0), positions(uv, rec, 0, 1))
        normals.append(n/np.linalg.norm(n, axis=1)[:, None])
    angle = np.degrees(np.arccos(np.clip(np.sum(normals[0]*normals[1], axis=1), -1, 1)))
    return q, angle


def source_angles(v, f):
    edges, owners = incidence(f)
    fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    fn /= np.linalg.norm(fn, axis=1)[:, None]
    source_angles = np.degrees(np.arccos(np.clip(np.sum(fn[owners[:, 0]]*fn[owners[:, 1]], axis=1), -1, 1)))
    return {tuple(e): a for e, a in zip(edges, source_angles)}


def domain_widths(records, curves, vertices, triangles):
    """Intrinsic strip scale 2A/P from original triangles and boundary chains."""
    area2 = np.linalg.norm(np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                                   vertices[triangles[:, 2]]-vertices[triangles[:, 0]]), axis=1)
    perimeter = {di: 0. for di in records}
    for curve in curves.values():
        length = float(np.linalg.norm(np.diff(vertices[curve['chain']], axis=0), axis=1).sum())
        for di in curve['owners']:
            perimeter[int(di)] += length
    return {di: float(area2[rec['triangle_ids']].sum()/perimeter[di]) for di, rec in records.items()}


def restore_groups(records):
    """Recover common geometric supports, including caches without group tags."""
    groups = {}; touched = set()
    for di, rec in records.items():
        if 'shared_support_group' in rec:
            touched.add(di)
        key = tuple(np.ascontiguousarray(rec[k]).tobytes() for k in
                    ['origin', 'axes', 'lo', 'size', 'knots', 'height'])
        groups.setdefault(key, []).append(di)
    for ids in groups.values():
        if len(ids) > 1:
            touched.update(ids)
        for di in ids:
            if len(ids) > 1: records[di]['shared_support_group'] = np.array(sorted(ids))
            else: records[di].pop('shared_support_group', None)
    return touched


def seam_rows(curves, records, lookup):
    rows = []
    for ci, c in curves.items():
        q, angle = seam_samples(c, records)
        sa = np.array([lookup[tuple(sorted((a, b)))] for a, b in zip(c['chain'][:-1], c['chain'][1:])])
        nearest = np.clip(np.searchsorted(c['parameters'], q, side='right')-1, 0, len(sa)-1)
        smooth = sa[nearest] < 30
        excess = np.maximum(0, angle-sa[nearest]-5)
        rows.append(dict(curve=ci, owners=c['owners'].tolist(),
            regions=[int(records[int(di)]['region']) for di in c['owners']],
            source_p95=float(np.quantile(sa, .95)), source_max=float(sa.max()),
            normal_p95=float(np.quantile(angle, .95)), normal_max=float(angle.max()),
            smooth_excess_p95=float(np.quantile(excess[smooth], .95)) if smooth.any() else 0.,
            smooth_fraction=float(np.mean(smooth))))
    rows.sort(key=lambda r: r['smooth_excess_p95'], reverse=True)
    return rows


def scan(root, output=None):
    """Rank smooth-source joins independently of STEP exchange splitting."""
    root = Path(root); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    rows = seam_rows(curves, records, source_angles(v, f))
    if output: dump(output, rows)
    return rows


def _proposal(task, fit_options=None):
    selected, v, tri, normal, curves, records, config, lookup = task
    _, _, _, _, uv, _, dot = stable_frame(v, tri)
    if dot.min() <= .08 or not projected_boundary_valid(uv, boundary_edges(tri)):
        return None, dict(accepted=False, reason='nonregular_union', selected_charts=selected)
    internal = {ci: c for ci, c in curves.items() if set(map(int, c['owners'])).issubset(selected)}
    if not internal:
        return None, dict(accepted=False, reason='disconnected_union', selected_charts=selected)
    for c in internal.values():
        sa = [lookup[tuple(sorted((a, b)))] for a, b in zip(c['chain'][:-1], c['chain'][1:])]
        if np.quantile(sa, .95) > config.feature_angle:
            return None, dict(accepted=False, reason='source_crease', selected_charts=selected)
    samples = []
    for c in curves.values():
        cuts = np.unique(np.r_[c['parameters'], c['knots']])
        q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
        samples.append(BSpline(c['knots'], c['poles'], 3)(q))
    _, fitted, info = fit_support((0, v, tri, normal, np.concatenate(samples), list(curves.values()), config, 'bounded'),
                                 **(fit_options or {}))
    info['selected_charts'] = selected
    if fitted is None: return None, info
    clist = list(curves.values())
    for di in selected:
        if intersections(clist, [i for i, c in enumerate(clist) if di in c['owners']], fitted):
            return None, dict(info, accepted=False, reason='trim_projection')
    candidate = dict(records)
    for di in selected: candidate[di] = dict(fitted, region=records[di]['region'])
    before = seam_rows(curves, records, lookup); after = seam_rows(curves, candidate, lookup)
    old = {r['curve']: r for r in before}; new = {r['curve']: r for r in after}
    cost_before = sum(r['smooth_excess_p95']**2 for r in before)
    cost_after = sum(r['smooth_excess_p95']**2 for r in after)
    info['join_score_before'] = cost_before; info['join_score_after'] = cost_after
    info['new_worst_smooth_excess_degrees'] = max(r['smooth_excess_p95'] for r in after)
    worsened = [ci for ci in old if new[ci]['smooth_excess_p95'] > max(45, old[ci]['smooth_excess_p95']+10)]
    if cost_after >= cost_before*.9 or worsened:
        return None, dict(info, accepted=False, reason='neighbor_join_regression', worsened_curves=worsened)
    # Source centroid normals protect the interiors from a successful seam
    # repair that merely moves its wrinkle into the neighboring surface.
    before_angles = []; after_angles = []
    world = v[tri].mean(axis=1); target = normal.mean(axis=1)
    target /= np.linalg.norm(target, axis=1)[:, None]
    for di in selected:
        rec = records[di]
        # Task records contain local union triangle indices for this check.
        ids = rec['union_triangle_ids']
        for r, values in [(rec, before_angles), (fitted, after_angles)]:
            uv = parameters(world[ids], r)
            nn = np.cross(positions(uv, r, 1, 0), positions(uv, r, 0, 1))
            nn /= np.linalg.norm(nn, axis=1)[:, None]
            values.extend(np.degrees(np.arccos(np.clip(np.sum(nn*target[ids], axis=1), -1, 1))))
    p95_before, p95_after = [float(np.quantile(a, .95)) for a in [before_angles, after_angles]]
    info['source_normal_p95_before'] = p95_before; info['source_normal_p95_after'] = p95_after
    if p95_after > p95_before+2:
        return None, dict(info, accepted=False, reason='source_normal_regression')
    return fitted, info


def optimize(source, output, *, workers=4, threshold=25., max_proposals=40, max_group=4,
             seconds=None, fit_options=None):
    """Repair only abnormal local joins; preserve every supplied region trim."""
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if seconds is not None and (not np.isfinite(seconds) or seconds <= 0):
        raise ValueError('Invalid common-support time budget')
    config = Config(**meta['config'])
    if config.boundary_normal_weight:
        raise ValueError('Common-support repair requires the uncoupled fitting configuration')
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    restored = restore_groups(records)
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    lookup = source_angles(v, f); normal = crease_fan_normals(v, f, config.feature_angle,
                                                            filter_steps=meta['normal_filter_steps'])
    initial = seam_rows(curves, records, lookup); dump(out/'joins_before.json', initial)
    attempts = []; attempted = set(); accepted = []; pending = []; revision = {}
    while len(attempts) < max_proposals:
        if seconds is not None and time.perf_counter()-start >= seconds:
            break
        rows = initial if not attempts else seam_rows(curves, records, lookup)
        tasks = []; locked = set()
        candidates = pending+[r['owners'] for r in rows if r['smooth_excess_p95'] >= threshold
                              and r['source_p95'] <= config.feature_angle]
        for seed in candidates:
            selected = sorted(set(seed).union(*(set(map(int, records[i].get('shared_support_group', []))) for i in seed)))
            if len(selected) > max_group: continue
            incident = {ci: c for ci, c in curves.items() if set(map(int, c['owners'])).intersection(selected)}
            neighborhood = set(i for c in incident.values() for i in map(int, c['owners']))
            key = (tuple(selected), tuple((i, revision.get(i, 0)) for i in sorted(neighborhood)))
            if key in attempted: continue
            if neighborhood.intersection(locked): continue
            if len({tuple(map(int, records[i].get('shared_support_group', [i]))) for i in selected}) == 1: continue
            ids = np.unique(np.concatenate([records[i]['triangle_ids'] for i in selected])); used = np.unique(f[ids])
            subset = {i: dict(records[i]) for i in neighborhood}
            for di in selected: subset[di]['union_triangle_ids'] = np.searchsorted(ids, records[di]['triangle_ids'])
            local_lookup = {tuple(sorted((a, b))): lookup[tuple(sorted((a, b)))]
                            for c in incident.values() for a, b in zip(c['chain'][:-1], c['chain'][1:])}
            tasks.append((selected, v[used], np.searchsorted(used, f[ids]), normal[ids], incident, subset, config, local_lookup))
            attempted.add(key); locked.update(neighborhood)
            if len(tasks) >= min(workers, max_proposals-len(attempts)): break
        if not tasks: break
        import multiprocessing as mp
        from functools import partial
        pool = mp.Pool(workers)
        try:
            results = pool.imap_unordered(partial(_proposal, fit_options=fit_options), tasks, chunksize=1)
            for _ in tasks:
                remaining = None if seconds is None else seconds-(time.perf_counter()-start)
                if remaining is not None and remaining <= 0:
                    break
                try:
                    fitted, info = results.next(timeout=remaining)
                except mp.TimeoutError:
                    break
                attempts.append(info)
                if fitted is not None:
                    selected = info['selected_charts']; accepted.append(selected)
                    for di in selected:
                        rec = dict(fitted)
                        for key in ['region', 'triangle_ids', 'vertex_ids']: rec[key] = records[di][key]
                        rec['shared_support_group'] = np.array(selected); records[di] = rec
                        revision[di] = revision.get(di, 0)+1
                elif info.get('reason') == 'neighbor_join_regression':
                    # If a two-domain solution displaces a wrinkle to a third
                    # neighbor, try that bounded local union, not a global fit.
                    for ci in info.get('worsened_curves', []):
                        group = sorted(set(info['selected_charts']).union(map(int, curves[ci]['owners'])))
                        if len(group) <= max_group and group not in pending: pending.append(group)
                print(dict(proposal=len(attempts), charts=info['selected_charts'], accepted=fitted is not None,
                           reason=info.get('reason')), flush=True)
                dump(out/'proposals.json', attempts)
        finally:
            pool.terminate(); pool.join()
    for name in ['charts', 'curves', 'routes']:
        if (root/name).exists(): shutil.copytree(root/name, out/name)
    shutil.copy2(root/'chart_index.npz', out/'chart_index.npz')
    for di in set(i for group in accepted for i in group).union(restored):
        np.savez_compressed(out/'charts'/f'{di:05d}.npz', **records[di])
    fitting = json.loads((root/'fitting.json').read_text(encoding='utf-8'))
    for row in fitting:
        row.pop('shared_support_group', None)
        if 'shared_support_group' in records[row['chart']]:
            row['shared_support_group'] = records[row['chart']]['shared_support_group'].tolist()
    dump(out/'fitting.json', fitting)
    final = seam_rows(curves, records, lookup); dump(out/'joins_after.json', final)
    report = dict(accepted_groups=accepted, attempted_groups=len(attempts), threshold_degrees=threshold,
                  max_proposals=max_proposals, max_group=max_group, workers=workers, seconds=time.perf_counter()-start,
                  search_seconds_limit=seconds, fit_options=fit_options,
                  source_fit=str(root.resolve()), source_CAD_read=False, supplied_labels_preserved=True)
    dump(out/'repair.json', report)
    meta.update(source_fit=str(out.resolve()), shared_support_repair=report, status='fitted')
    dump(out/'run.json', meta)
    return report


def join(source, output, selected):
    root, out = Path(source), Path(output)
    if out.exists(): raise FileExistsError(out)
    started = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    config = Config(**meta['config'])
    if config.boundary_normal_weight:
        raise ValueError('Common-support repair requires the uncoupled fitting configuration')
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    restore_groups(records)
    selected = sorted(set(map(int, selected)))
    # A previous accepted common support is indivisible during further local
    # repair; replacing only one member would reopen its smooth seam.
    if all(i in records for i in selected):
        selected = sorted(set(selected).union(*(set(map(int, records[i].get('shared_support_group', []))) for i in selected)))
    if len(selected) < 2 or any(i not in records for i in selected):
        raise ValueError('Common-support repair requires at least two existing chart IDs')
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    ids = np.unique(np.concatenate([records[i]['triangle_ids'] for i in selected])); used = np.unique(f[ids])
    tri = np.searchsorted(used, f[ids]); origin, axes, lo, size, uv, _, dot = stable_frame(v[used], tri)
    if dot.min() <= .08 or not projected_boundary_valid(uv, boundary_edges(tri)):
        raise ValueError('Neighbor union is not a regular local height domain')
    edges, owners = incidence(f); assignment = np.load(root/'chart_index.npz')['face_chart']
    inside = np.isin(assignment[owners], selected)
    shared = inside.all(axis=1) & (assignment[owners[:, 0]] != assignment[owners[:, 1]])
    if not np.any(shared): raise ValueError('Selected charts have no shared source boundary')
    fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    fn /= np.linalg.norm(fn, axis=1)[:, None]
    angle = np.degrees(np.arccos(np.clip(np.sum(fn[owners[shared, 0]]*fn[owners[shared, 1]], axis=1), -1, 1)))
    if np.quantile(angle, .95) > config.feature_angle:
        raise ValueError('Source boundary is predominantly a crease')
    curves = []; samples = []
    for path in sorted((root/'curves').glob('*.npz')):
        c = dict(np.load(path))
        if not set(map(int, c['owners'])).intersection(selected): continue
        cuts = np.unique(np.r_[c['parameters'], c['knots']])
        q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
        samples.append(BSpline(c['knots'], c['poles'], 3)(q)); curves.append(c)
    normal = crease_fan_normals(v, f, config.feature_angle, (edges, owners), meta['normal_filter_steps'])
    _, fitted, info = fit_support((0, v[used], tri, normal[ids], np.concatenate(samples), curves, config, 'bounded'))
    out.mkdir(parents=True)
    report = dict(source_fit=str(root.resolve()), selected_charts=selected, source_triangles=len(ids),
                  supplied_labels_preserved=True, source_CAD_read=False, accepted=fitted is not None,
                  source_join_angle_p95=float(np.quantile(angle, .95)), source_join_angle_max=float(angle.max()),
                  fitting=info, seconds=time.perf_counter()-started)
    dump(out/'join.json', report)
    if fitted is None: return report
    for di in selected:
        incident = [i for i, c in enumerate(curves) if di in c['owners']]
        if intersections(curves, incident, fitted):
            report.update(accepted=False, error='Changed projection invalidates a trim')
            dump(out/'join.json', report); return report
    for name in ['charts', 'curves']:
        shutil.copytree(root/name, out/name)
    shutil.copy2(root/'chart_index.npz', out/'chart_index.npz')
    for di in selected:
        rec = dict(fitted)
        for key in ['region', 'triangle_ids', 'vertex_ids']: rec[key] = records[di][key]
        rec['shared_support_group'] = np.array(selected)
        np.savez_compressed(out/'charts'/f'{di:05d}.npz', **rec)
    rows = json.loads((root/'fitting.json').read_text(encoding='utf-8'))
    for row in rows:
        if row['chart'] in selected:
            row.update(accepted=True, reused=False, shared_support_group=selected, history=info['history'])
    dump(out/'fitting.json', rows)
    meta.update(source_fit=str(out.resolve()), shared_support_repair=report,
                partition_labels_unchanged=True, status='fitted', seconds=time.perf_counter()-started)
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--charts', type=int, nargs='+')
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--threshold', type=float, default=25.)
    p.add_argument('--max-proposals', type=int, default=40)
    a = p.parse_args()
    result = join(a.run, a.output, a.charts) if a.charts else optimize(a.run, a.output, workers=a.workers,
        threshold=a.threshold, max_proposals=a.max_proposals)
    print(result, flush=True)
