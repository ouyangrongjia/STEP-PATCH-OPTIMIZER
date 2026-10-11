"""Whole-model master reconstruction with atomic junction/branch admission.

Existing validated masters remain fixed. Every other regional and internal
interface is considered by the same geometric rules, including closed loops.
Candidates are admitted only with all transported branches and valid wires.
"""
import argparse
from collections import Counter
from dataclasses import asdict, replace
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump
from .curve_routes import Config, route_graph, fit_master, restrict
from .route_audit import source_strip
from .boundary_quality import measures
from .network_support import validate_wires
from .shared_support import source_angles
from .regional_rectangle import curve_polyline_error


def plan_before_fit(stage, metadata, domains, curves, pairs, assignment, labels, config):
    """Construct the whole master network from this run's unfitted layout.

    Layout records contain source ownership only. No prior surface, master
    curve, candidate directory or previously fitted coefficients are read.
    """
    stage = Path(stage); seed = stage/'source_network'; out = stage/'smooth_network'
    seed.mkdir(); (seed/'charts').mkdir(); (seed/'curves').mkdir()
    for di, domain in enumerate(domains):
        np.savez_compressed(seed/'charts'/f'{di:05d}.npz', **domain)
    for ci, (curve, pair) in enumerate(zip(curves, pairs)):
        np.savez_compressed(seed/'curves'/f'{ci:05d}.npz', **dict(curve, owners=pair))
    np.savez_compressed(seed/'chart_index.npz', face_chart=assignment, source_labels=labels)
    dump(seed/'fitting.json', [dict(chart=di, region=int(d['region']), accepted=False,
                                  reason='not_fitted_yet', history=[]) for di, d in enumerate(domains)])
    dump(seed/'run.json', dict(metadata, source_fit=str(seed.resolve()), status='planned',
                             requires_support_refit=True, required_refit_charts=list(range(len(domains)))))
    report = reconstruct(seed, out, config=config)
    rows = [dict(np.load(p)) for p in sorted((out/'curves').glob('*.npz'))]
    from .network_support import samples
    pairs = [row['owners'] for row in rows]
    if (out/'routes').exists():
        shutil.copytree(out/'routes', stage/'routes')
    meta = json.loads((out/'run.json').read_text(encoding='utf-8'))
    meta.update(status='planned', requires_support_refit=True,
                required_refit_charts=list(range(len(domains))))
    dump(out/'run.json', meta)
    return rows, pairs, [samples(c) for c in rows], dict(
        source_network=str(seed.resolve()), smooth_network=str(out.resolve()),
        changed_curves=len(report['changed_curves']), changed_regions=report['changed_regions'],
        accepted_routes=report['accepted_routes'], seconds=report['seconds'])


def reconstruct(source, output, config=None, excluded_charts=()):
    cfg = config or Config(movement=.16, maximum_controls=32, minimum_length=.15, cross_region_junctions=True)
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if min(cfg.movement, cfg.tube, cfg.minimum_length, cfg.direction_span) <= 0 or cfg.tube >= meta['config']['distance']:
        raise ValueError('Invalid curve reconstruction budgets')
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    original = dict(curves); regions = {di: int(c['region']) for di, c in records.items()}
    quality = {ci: measures(c) for ci, c in curves.items()}
    lookup = source_angles(v, f); make_locator, halo = source_strip(v, f, cfg.movement, cfg.tube)
    sharp = {ci for ci, c in curves.items() if max(lookup[tuple(sorted((int(a), int(b))))]
             for a, b in zip(c['chain'][:-1], c['chain'][1:])) > meta['config']['feature_angle']}
    from .partition_fit import incidence
    edges, owners = incidence(f); keys = edges[:, 0]*len(v)+edges[:, 1]
    normal = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    normal /= np.linalg.norm(normal, axis=1)[:, None]
    def incident_planes(chain):
        key = np.minimum(chain[:-1], chain[1:])*len(v)+np.maximum(chain[:-1], chain[1:])
        pair = normal[owners[np.searchsorted(keys, key)]].copy()
        smooth = np.einsum('ij,ij->i', pair[:, 0], pair[:, 1]) >= np.cos(np.radians(meta['config']['feature_angle']))
        # A local crease does not impose tangent-plane constraints on the
        # entire curved route. Smooth portions use the finite surface tube.
        pair[smooth] = 0.
        return pair
    excluded = set(map(int, excluded_charts)); decisions = []; masters = []; changed = set()
    rid = max((int(p.stem) for p in (root/'routes').glob('*.npz')), default=-1)+1
    for kind in ('regional', 'internal'):
        local_cfg = replace(cfg, interface_kind=kind, cross_region_junctions=kind == 'regional')
        eligible = {ci for ci, c in curves.items() if 'route_id' not in c and not excluded.intersection(map(int, c['owners']))}
        if kind == 'internal':
            eligible = {ci for ci in eligible if max(lookup[tuple(sorted((int(a), int(b))))]
                         for a, b in zip(curves[ci]['chain'][:-1], curves[ci]['chain'][1:])) <= meta['config']['feature_angle']}
        fixed = {int(vi) for c in curves.values() if 'route_id' in c for vi in c['chain'][[0, -1]]}
        if kind == 'internal':
            fixed.update(int(vi) for ci in sharp for vi in curves[ci]['chain'][[0, -1]])
        fixed.update(int(vi) for c in curves.values() if excluded.intersection(map(int, c['owners'])) for vi in c['chain'][[0, -1]])
        routes = route_graph(curves, regions, v, local_cfg, allowed_curves=eligible, fixed_nodes=fixed)
        routes.sort(key=lambda r: -float(np.linalg.norm(np.diff(v[r['chain']], axis=0), axis=1).sum()))
        for index, route in enumerate(routes):
            chain = route['chain']; points = v[chain]
            length = float(np.linalg.norm(np.diff(points, axis=0), axis=1).sum())
            row = dict(kind=kind, members=route['members'], regions=route['regions'], closed=route['closed'],
                       length_mm=length, accepted=False); decisions.append(row)
            if length < cfg.minimum_length:
                row['reason'] = 'short'; continue
            if index % 20 == 0:
                print(dict(stage='global_network', kind=kind, considered=index, total=len(routes), accepted=len(masters)), flush=True)
            first, forward = route['members'][0]; last, last_forward = route['members'][-1]
            ends = np.array([curves[first]['poles'][0 if forward else -1], curves[last]['poles'][-1 if last_forward else 0]])
            # A loop touching an established master has a real fixed junction;
            # retain that junction rather than introducing a free periodic cut.
            if route['closed'] and set(map(int, chain)).intersection(fixed):
                row['reason'] = 'closed_fixed_junction'; continue
            feature = any(ci in sharp for ci, _ in route['members'])
            row['source_crease'] = feature
            master, trials = fit_master(points, ends, make_locator(chain), local_cfg, closed=route['closed'],
                                        plane_normals=incident_planes(chain) if feature else None)
            row['trials'] = trials
            if master is None:
                row['reason'] = 'source_envelope'; continue
            proposal = {}; anchors = {}; offset = 0
            for ci, direction in route['members']:
                c = curves[ci]; start = master['parameters'][offset]; offset += len(c['chain'])-1; stop = master['parameters'][offset]
                cp, kt = restrict(master['poles'], master['knots'], start, stop, direction)
                u = master['parameters'][offset-len(c['chain'])+1:offset+1]
                proposal[ci] = dict(c, poles=cp, knots=kt, route_id=rid, route_interval=np.array([start, stop]),
                    route_forward=direction, method='global_master_restriction', movement_budget_mm=cfg.movement,
                    source_tube_limit_mm=cfg.tube, source_tube_certificate_version=1,
                    source_tube_certified_max_mm=master['source_tube_certified_max_mm'],
                    parameters=(u-start)/(stop-start) if direction else (stop-u[::-1])/(stop-start))
                if feature:
                    proposal[ci].update(incident_plane_max_mm=master['incident_plane_max_mm'],
                                        incident_plane_limit_mm=cfg.crease_plane_tolerance)
                for vi, pt in zip(c['chain'][[0, -1]], cp[[0, -1]]):
                    if int(vi) in anchors and np.linalg.norm(anchors[int(vi)]-pt) > 1e-8:
                        raise ValueError('Inconsistent periodic/shared master endpoint')
                    anchors[int(vi)] = pt
            branch_trials = []; rejected = None
            for ci, c in curves.items():
                if ci in proposal:
                    continue
                ends = np.array([anchors.get(int(vi), p) for vi, p in zip(c['chain'][[0, -1]], c['poles'][[0, -1]])])
                if np.max(np.linalg.norm(ends-c['poles'][[0, -1]], axis=1)) < 1e-10:
                    continue
                if 'route_id' in c or excluded.intersection(map(int, c['owners'])):
                    rejected = 'protected_master_junction'; break
                branch, attempts = fit_master(v[c['chain']], ends, make_locator(c['chain']), replace(cfg, fairness_length=.1),
                    plane_normals=incident_planes(c['chain']) if ci in sharp else None)
                branch_trials.append(dict(curve=ci, trials=attempts))
                if branch is None:
                    rejected = 'branch_envelope'; break
                old, new = quality[ci], measures(branch)
                if (new['total_tangent_turn_degrees'] > old['total_tangent_turn_degrees']*1.05+2 or
                        new['curvature_squared_integral_per_mm'] > old['curvature_squared_integral_per_mm']*1.05+.01/max(old['length_mm'], 1e-10)):
                    rejected = 'branch_fairness'; break
                proposal[ci] = dict(c, **branch, method='global_branch_refit', movement_budget_mm=cfg.movement,
                                    source_tube_limit_mm=cfg.tube, source_tube_certificate_version=1)
            row['branches'] = branch_trials
            if rejected:
                row['reason'] = rejected; continue
            new_quality = {ci: measures(c) for ci, c in proposal.items()}
            before = sum(quality[ci]['total_tangent_turn_degrees'] for ci in proposal)
            after = sum(r['total_tangent_turn_degrees'] for r in new_quality.values())
            if after > before*1.02+1.:
                row['reason'] = 'transaction_turn_regression'; continue
            old_bending = sum(quality[ci]['curvature_squared_integral_per_mm'] for ci in proposal)
            new_bending = sum(value['curvature_squared_integral_per_mm'] for value in new_quality.values())
            length_before = sum(quality[ci]['length_mm'] for ci in proposal)
            if new_bending > old_bending*1.05+.01/max(length_before, 1e-10):
                row['reason'] = 'transaction_bending_regression'; continue
            affected = {int(di) for c in proposal.values() for di in c['owners']}
            trial = {ci: proposal.get(ci, c) for ci, c in curves.items() if affected.intersection(map(int, c['owners']))}
            failures = validate_wires(v, f, records, trial, affected)
            if failures:
                row.update(reason='trim', failures=failures); continue
            for ci, c in proposal.items():
                c['movement_mm'], _ = curve_polyline_error(c['poles'], c['knots'], v[c['chain']], c['parameters'])
            row.update(accepted=True, route=rid, changed_curves=sorted(proposal), affected_charts=sorted(affected),
                       before_turn_degrees=before, after_turn_degrees=after)
            row.update(before_bending=old_bending, after_bending=new_bending)
            masters.append(dict(master, route=rid, chain=chain, closed=route['closed']))
            rid += 1; curves.update(proposal); quality.update(new_quality); changed.update(proposal)
            # Keep periodic recovery evidence without serializing the entire
            # growing trace for every accepted curve. The final report below
            # still retains every proposal and rejection.
            if len(masters) % 20 == 0:
                dump(out/'progress.json', dict(accepted_routes=len(masters), decisions=decisions))
    for name in ('charts', 'curves', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    (out/'routes').mkdir(exist_ok=True)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for ci in changed:
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **curves[ci])
    for master in masters:
        np.savez_compressed(out/'routes'/f"{master['route']:05d}.npz", **master)
    affected = sorted({int(di) for ci in changed for di in curves[ci]['owners']})
    report = dict(config=asdict(cfg), considered_routes=len(decisions), accepted_routes=len(masters),
                  accepted_closed_routes=sum(bool(m['closed']) for m in masters), decisions=decisions,
                  rejection_counts=dict(Counter(r.get('reason', 'accepted') for r in decisions)),
                  changed_curves=sorted(changed), affected_charts=affected,
                  changed_regions=sorted({regions[i] for i in affected}), excluded_charts=sorted(excluded),
                  source_geodesic_halo_mm=halo, seconds=time.perf_counter()-started,
                  all_regions_considered=True, existing_masters_preserved=True,
                  source_crease_curves=sorted(sharp),
                  reconstructed_source_crease_curves=sorted(sharp.intersection(changed)),
                  preserved_source_crease_curves=sorted(sharp-changed),
                  crease_protection='both_incident_planes_and_source_tube',
                  source_coordinates_and_labels_unchanged=True, source_CAD_read=False, commercial_engine_used=False)
    dump(out/'global_network.json', report)
    dump(out/'progress.json', dict(accepted_routes=len(masters), decisions=decisions))
    meta.update(source_fit=str(out.resolve()), status='boundary_candidate' if affected else 'fitted',
                requires_support_refit=bool(affected), required_refit_charts=affected, force_refit_charts=[],
                global_network=report, can_apply=False)
    dump(out/'run.json', meta)
    print({k: report[k] for k in ('considered_routes', 'accepted_routes', 'accepted_closed_routes', 'rejection_counts', 'seconds')}, flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); parser.add_argument('--output', required=True)
    args = parser.parse_args(); reconstruct(args.run, args.output)
