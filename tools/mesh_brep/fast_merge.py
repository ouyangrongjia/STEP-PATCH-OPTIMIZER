"""One-pass regional merging with a wall-clock budget and unchanged outer curves.

Each disjoint pair receives one uniform C2 fit. Failed pairs keep their original
surfaces; there is no local knot enrichment or continuity-repair loop. The output
is a fitting checkpoint, not a validated CAD delivery.
"""
import argparse
from dataclasses import asdict, dataclass, replace
import json
import multiprocessing as mp
from pathlib import Path
import time
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump
from .partition_fit import Config, boundary_edges, parameters, positions, projected_boundary_valid
from .skeleton_graph import fit_support, stable_frame
from .source_normals import crease_fan_normals
from .splines import knots


@dataclass(frozen=True)
class Budget:
    seconds: float = 180.
    proposals: int = 200
    workers: int = 4
    controls: int = 33
    solver_seconds: float = 1.
    source_normal_slack: float = .5
    seam_normal_slack: float = 1.

    def validate(self):
        if (not np.isfinite([self.seconds, self.solver_seconds, self.source_normal_slack,
                             self.seam_normal_slack]).all()
                or min(self.seconds, self.solver_seconds) <= 0
                or min(self.proposals, self.workers) < 1 or self.controls < 9
                or min(self.source_normal_slack, self.seam_normal_slack) < 0):
            raise ValueError('Invalid fast merging budget')


def normals_at(record, xyz):
    uv = parameters(xyz, record)
    n = np.cross(positions(uv, record, 1, 0), positions(uv, record, 0, 1))
    return n/np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-30)


def normal_stats(angles, weights):
    order = np.argsort(angles); weights = weights/weights.sum()
    return dict(rms=float(np.sqrt(weights@np.square(angles))),
                p95=float(np.interp(.95, np.cumsum(weights[order]), angles[order])),
                maximum=float(np.max(angles)))


def angle(a, b):
    return np.degrees(np.arccos(np.clip(np.sum(a*b, axis=1), -1, 1)))


def seam_change(curve, before, after):
    from .surface_jets import support_span_parameters
    owners = list(map(int, curve['owners']))
    records = [lookup[i] for lookup in (before, after) for i in owners]
    q = np.unique(np.r_[curve['parameters'], np.linspace(0, 1, 65),
                         support_span_parameters(curve, records, quadrature=True)])
    xyz = BSpline(curve['knots'], curve['poles'], 3)(q)
    result = []
    for lookup in (before, after):
        a = angle(*(normals_at(lookup[i], xyz) for i in owners))
        result.append(dict(p95=float(np.quantile(a, .95)), maximum=float(a.max())))
    return result


def seam_regresses(before, after, slack):
    return after['p95'] > before['p95']+slack or after['maximum'] > before['maximum']+2*slack


def candidates(records, curves, vertices, triangles, feature_angle):
    """Rank savings in internal edge length; preserve labels and old common supports."""
    from .shared_support import source_angles
    lookup = source_angles(vertices, triangles); pairs = {}
    for ci, c in curves.items():
        pair = tuple(sorted(map(int, c['owners'])))
        a, b = pair
        if (int(records[a]['region']) != int(records[b]['region'])
                or any(len(records[i].get('shared_support_group', [])) > 1 for i in pair)):
            continue
        pairs.setdefault(pair, []).append(ci)
    eligible = []; rejected = []
    for pair, internal in pairs.items():
        ids = np.union1d(*(records[i]['triangle_ids'] for i in pair))
        used = np.unique(triangles[ids]); local = np.searchsorted(used, triangles[ids])
        reason = None
        try:
            frame = stable_frame(vertices[used], local)
            if frame[-1].min() <= .15 or not projected_boundary_valid(frame[4], boundary_edges(local)):
                reason = 'nonregular_union'
            elif any(max(lookup[tuple(sorted((a, b)))] for a, b in
                         zip(curves[ci]['chain'][:-1], curves[ci]['chain'][1:])) > feature_angle for ci in internal):
                reason = 'protected_source_crease'
        except ValueError:
            reason = 'invalid_union'
        if reason:
            rejected.append(dict(pair=pair, reason=reason)); continue
        length = sum(np.linalg.norm(np.diff(vertices[curves[ci]['chain']], axis=0), axis=1).sum() for ci in internal)
        eligible.append((float(length), pair))
    # One attempt per original face, covering the whole model without retries.
    selected = []; taken = set()
    for _, pair in sorted(eligible, key=lambda x: (-x[0], x[1])):
        if not taken.intersection(pair):
            selected.append(pair); taken.update(pair)
    return selected, rejected


_WORK = None


def initialize(vertices, triangles, normals, records, curves, config, budget):
    global _WORK
    _WORK = vertices, triangles, normals, records, curves, config, budget


def propose(pair):
    started = time.perf_counter()
    v, f, normal, records, curves, config, budget = _WORK
    info = dict(pair=list(pair), accepted=False)
    try:
        ids = np.union1d(*(records[i]['triangle_ids'] for i in pair)); used = np.unique(f[ids])
        local = np.searchsorted(used, f[ids])
        exterior = {ci: c for ci, c in curves.items()
                    if len(set(pair).intersection(map(int, c['owners']))) == 1}
        from .network_support import samples, validate_wires
        count = min(budget.controls, max(config.controls),
                    int(np.sqrt(sum(len(records[i]['height']) for i in pair))))
        if count < 9:
            info['reason'] = 'insufficient_control_budget'; return None, info
        cfg = replace(config, controls=(count,), distance=config.distance*.85,
                      boundary_normal_weight=0., normal_angle_limit=None)
        _, fitted, fitting = fit_support((0, v[used], local, normal[ids],
            np.concatenate([samples(c) for c in exterior.values()]), list(exterior.values()),
            cfg, 'bounded', None, dict(knots=knots(count))),
            max_iterations=1, solver_seconds=budget.solver_seconds, solver_retry=False)
        info['fitting'] = fitting
        if fitted is None:
            info['reason'] = 'single_fit_rejected'; return None, info
        fitted.update(region=records[pair[0]]['region'], triangle_ids=ids, vertex_ids=used)
        remapped = {ci: dict(c, owners=np.where(c['owners'] == pair[1], pair[0], c['owners']))
                    for ci, c in exterior.items()}
        changed = dict(records); changed[pair[0]] = fitted
        failures = validate_wires(v, f, changed, remapped, {pair[0]}, native_frame=True)
        if failures:
            info.update(reason='trim_rejected', failures=failures); return None, info
        info['source_normals'] = []
        for di in pair:
            ti = records[di]['triangle_ids']; points = v[f[ti]].mean(axis=1)
            target = normal[ti].mean(axis=1)
            target /= np.maximum(np.linalg.norm(target, axis=1)[:, None], 1e-30)
            weights = np.linalg.norm(np.cross(v[f[ti, 1]]-v[f[ti, 0]], v[f[ti, 2]]-v[f[ti, 0]]), axis=1)
            before, after = [normal_stats(angle(normals_at(r, points), target), weights) for r in (records[di], fitted)]
            info['source_normals'].append(dict(chart=di, before=before, after=after))
            if (any(after[k] > before[k]+budget.source_normal_slack for k in ('rms', 'p95'))
                    or after['maximum'] > before['maximum']+4*budget.source_normal_slack):
                info['reason'] = 'source_normal_regression'; return None, info
        joined = dict(records)
        for di in pair: joined[di] = fitted
        info['boundary_normals'] = []
        for ci, c in exterior.items():
            before, after = seam_change(c, records, joined)
            info['boundary_normals'].append(dict(curve=ci, before=before, after=after))
            if seam_regresses(before, after, budget.seam_normal_slack):
                info['reason'] = 'neighbor_normal_regression'; return None, info
        info.update(accepted=True, reason='accepted', controls=count)
        return fitted, info
    except (ValueError, RuntimeError) as error:
        info.update(reason='invalid_proposal', error=str(error)); return None, info
    finally:
        info['seconds'] = time.perf_counter()-started


def save_fit(root, out, records, curves, accepted, meta):
    """Drop internal edges and renumber ancestry, without projecting labels."""
    records = dict(records); redirect = {di: di for di in records}; merged = {}
    for pair, fitted in accepted:
        a, b = pair; records[a] = fitted; del records[b]; redirect[b] = a; merged[a] = list(pair)
    mapping = {di: i for i, di in enumerate(sorted(records))}
    assignment = np.load(root/'chart_index.npz')['face_chart']
    new_assignment = np.array([mapping[redirect[int(i)]] for i in assignment])
    for name in ('charts', 'curves'): (out/name).mkdir()
    for di, rec in sorted(records.items()):
        rec = dict(rec)
        if 'shared_support_group' in rec:
            rec['shared_support_group'] = np.array([mapping[redirect[int(i)]] for i in rec['shared_support_group']])
        np.savez_compressed(out/'charts'/f'{mapping[di]:05d}.npz', **rec)
    retained = []; removed = []
    for ci, c in sorted(curves.items()):
        owners = np.array([mapping[redirect[int(i)]] for i in c['owners']])
        if owners[0] == owners[1]: removed.append(ci); continue
        np.savez_compressed(out/'curves'/f'{len(retained):05d}.npz', **dict(c, owners=owners))
        retained.append(ci)
    np.savez_compressed(out/'chart_index.npz', face_chart=new_assignment)
    prior_rows = {r['chart']: r for r in json.loads((root/'fitting.json').read_text(encoding='utf-8'))}
    rows = []
    for di in sorted(records):
        row = dict(prior_rows[di], chart=mapping[di]); row.pop('shared_support_group', None)
        if di in merged: row.update(accepted=True, merged_source_charts=merged[di], history=[])
        rows.append(row)
    dump(out/'fitting.json', rows)
    report = dict(meta, status='fitted', source_fit=str(out.resolve()), fitted_charts=len(records),
                  requires_support_refit=False, required_refit_charts=[], force_refit_charts=[],
                  algorithm='budgeted-single-pass-region-merge-v1', complete_source_coverage=True,
                  partition_labels_unchanged=True, actual_control_points=sum(len(c['height']) for c in records.values()))
    report['fast_merge_ancestry'] = dict(source_fit=str(root.resolve()), chart_mapping=mapping,
        merged_charts={mapping[a]: pair for a, pair in merged.items()}, removed_curves=removed,
        retained_source_curves=retained)
    dump(out/'run.json', report)
    return report


def merge(source, output, budget=None):
    budget = budget or Budget(); budget.validate()
    start = time.perf_counter(); root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', can_apply=False, budget=asdict(budget), source_fit=str(root.resolve()),
                  source_CAD_read=False, commercial_engine_used=False, refitting_passes=1)
    dump(out/'merge.json', report)
    try:
        z = np.load(meta['partition']); v, f = z['mesh_vertex_original'], z['mesh_triangle']
        records = {int(p.stem): dict(np.load(p)) for p in sorted((root/'charts').glob('*.npz'))}
        curves = {int(p.stem): dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
        from .shared_support import restore_groups
        restore_groups(records); config = Config(**meta['config'])
        pairs, rejected = candidates(records, curves, v, f, config.feature_angle)
        report.update(input_faces=len(records), eligible_disjoint_pairs=len(pairs), screening_rejections=rejected,
                      input_control_points=sum(len(c['height']) for c in records.values()), trials=[])
        pairs = pairs[:budget.proposals]
        normal = crease_fan_normals(v, f, config.feature_angle, filter_steps=meta['normal_filter_steps'])
        report['preparation_seconds'] = time.perf_counter()-start
        accepted = []
        remaining = budget.seconds-(time.perf_counter()-start)
        if pairs and remaining > 0:
            pool = mp.get_context('spawn').Pool(budget.workers, initializer=initialize,
                initargs=(v, f, normal, records, curves, config, budget))
            try:
                results = pool.imap_unordered(propose, pairs, chunksize=1)
                for _ in pairs:
                    remaining = budget.seconds-(time.perf_counter()-start)
                    if remaining <= 0: break
                    try: fitted, info = results.next(timeout=remaining)
                    except mp.TimeoutError: break
                    report['trials'].append(info)
                    if fitted is not None: accepted.append((tuple(info['pair']), fitted))
                    dump(out/'merge.json', report)
                    print(dict(stage='fast_merge', completed=len(report['trials']), accepted=len(accepted),
                               seconds=round(time.perf_counter()-start, 2)), flush=True)
            finally:
                pool.terminate(); pool.join()
        report['fitting_seconds'] = time.perf_counter()-start
        report['unattempted_or_timed_out_pairs'] = [p for p in pairs if list(p) not in [r['pair'] for r in report['trials']]]
        # Independently accepted neighboring pairs may interact. Withdraw both
        # sides of any new regression, never launch another solve.
        combined = dict(records); membership = {}
        for k, (pair, rec) in enumerate(accepted):
            for di in pair: combined[di] = rec; membership[di] = k
        withdraw = set(); interactions = []
        for ci, c in curves.items():
            a, b = map(int, c['owners'])
            if a not in membership or b not in membership or membership[a] == membership[b]: continue
            before, after = seam_change(c, records, combined)
            if seam_regresses(before, after, budget.seam_normal_slack):
                withdraw.update((membership[a], membership[b])); interactions.append(ci)
        report['interaction_rollbacks'] = [list(accepted[i][0]) for i in sorted(withdraw)]
        report['interaction_curves'] = interactions
        accepted = [item for i, item in enumerate(accepted) if i not in withdraw]
        accepted.sort(key=lambda item: item[0])
        fitted = save_fit(root, out, records, curves, accepted, meta)
        from .distance_guard import validate_fit
        validate_fit(out, meta['partition'], meta['stl'])
        report.update(status='fitted_pending_CAD_validation', output_faces=fitted['fitted_charts'],
                      output_control_points=fitted['actual_control_points'], accepted_pairs=[list(p) for p, _ in accepted],
                      removed_curves=len(fitted['fast_merge_ancestry']['removed_curves']),
                      retained_curves_unchanged=True, complete_source_coverage=True)
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-start; dump(out/'merge.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--seconds', type=float, default=180.); p.add_argument('--proposals', type=int, default=200)
    p.add_argument('--workers', type=int, default=4); p.add_argument('--controls', type=int, default=33)
    a = p.parse_args(); merge(a.run, a.output, Budget(seconds=a.seconds, proposals=a.proposals, workers=a.workers, controls=a.controls))
