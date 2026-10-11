"""One bounded linear fairing pass over all native graph supports.

Targets are frozen before the parallel pass. Smooth interfaces use a common
normal and world-space shape operator; source creases keep independent sides.
There is no chart subdivision, nonlinear optimizer, or repeated refit loop.
"""
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import shutil
import time

import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline

from .bounded_support import bending_matrix
from .mesh import dump
from .partition_fit import parameters, positions, spline_graph_error
from .splines import tensor
from .surface_jets import jets


@dataclass
class Config:
    movement: float = .012
    feature_fraction: float = .05
    normal_weight: float = .003
    seam_normal_weight: float = .03
    seam_curvature_weight: float = .00005
    variation_length: float = .05
    boundary_weight: float = 100.
    support_error: float = .003
    smooth_degrees: float = 10.
    solver_seconds: float = .05
    workers: int = 8


_STATE = None


def _initialize(state):
    global _STATE
    from .mesh import Locator
    _STATE = state
    _STATE['locator'] = Locator(state['vertices'], state['triangles'])
    # The next stage uses the same immutable STL locator in these workers.
    from . import boundary_refine
    boundary_refine._SOURCE_LOCATOR = _STATE['locator']
    boundary_refine._DISTANCE_LIMIT = state['distance']


def feasible_fraction(value, change, lower, upper):
    """Largest segment fraction in a convex box containing the initial value."""
    value, change = np.asarray(value), np.asarray(change)
    lower = np.minimum(np.broadcast_to(lower, value.shape), value)
    upper = np.maximum(np.broadcast_to(upper, value.shape), value)
    bound = np.full(value.shape, np.inf)
    np.divide(upper-value, change, out=bound, where=change > 0)
    np.divide(lower-value, change, out=bound, where=change < 0)
    return float(np.clip(bound.min(initial=1.), 0., 1.))


def normal_slopes(normal, axes):
    local = normal@axes.T
    return -local[:, :2]/np.maximum(local[:, 2, None], .1), local[:, 2] > .15


def hessian_target(record, normal, shape):
    """Pull a world shape operator back to this graph's parameter frame."""
    gradient, good = normal_slopes(normal, record['axes'])
    tangent = record['axes'][:2].T[None]+record['axes'][2][None, :, None]*gradient[:, None]
    result = np.sqrt(1+np.sum(gradient**2, axis=1))[:, None, None]*(tangent.transpose(0, 2, 1)@shape@tangent)
    return result, good


def _worker(di):
    tick = time.perf_counter(); state = _STATE; cfg = state['config']
    records, curves = state['records'], state['curves']
    rec = records[di]; old = rec['height']; knot = rec['knots']
    movement = min(cfg.movement, cfg.feature_fraction*state['widths'][di]) if 'widths' in state else cfg.movement
    v, f = state['vertices'], state['triangles']; ids = rec['triangle_ids']; used = rec['vertex_ids']
    triangles = v[f[ids]]; centers = triangles.mean(axis=1)
    area = np.linalg.norm(np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0]), axis=1)
    weights = area/area.sum()
    # Preserve the already fitted shape, while changing its derivatives.
    q = parameters(centers, rec); a = tensor(q, knot, knot)
    h = a.T@a.multiply(weights[:, None]); rhs = np.zeros(len(old))
    normal = state['normals'][ids].mean(axis=1)
    normal /= np.linalg.norm(normal, axis=1)[:, None]
    slopes, good = normal_slopes(normal, rec['axes'])
    def add(op, target, weight):
        nonlocal h, rhs
        residual = np.asarray(target)-op@old
        h = h+op.T@op.multiply(np.asarray(weight)[:, None])
        rhs += np.asarray(op.T@(np.asarray(weight)*residual)).ravel()
    for axis in (0, 1):
        op = tensor(q[good], knot, knot, int(axis == 0), int(axis == 1))/rec['size'][axis]
        add(op, slopes[good, axis], weights[good]*cfg.normal_weight)
    boundary = []; normal_rows = []; curvature_rows = []
    for ci in state['incident'][di]:
        curve = curves[ci]; pair = list(map(int, curve['owners'])); neighbor = pair[1-pair.index(di)]
        cuts = np.unique(curve['knots'])
        u = np.unique(np.r_[curve['parameters'], (cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel()])
        spline = BSpline(curve['knots'], curve['poles'], 3); xyz = spline(u)
        w = np.linalg.norm(spline(u, 1), axis=1)*np.r_[np.diff(u)[0]/2, (u[2:]-u[:-2])/2, np.diff(u)[-1]/2]
        boundary.append((xyz, w, curve))
        angles = state['angles'][ci]
        smooth = angles[np.clip(np.searchsorted(curve['parameters'], u, side='right')-1, 0, len(angles)-1)] <= cfg.smooth_degrees
        if not smooth.any():
            continue
        xyz, w = xyz[smooth], w[smooth]
        n0, s0 = jets(rec, xyz); n1, s1 = jets(records[neighbor], xyz)
        target_n = n0+n1; target_n /= np.linalg.norm(target_n, axis=1)[:, None]
        target_h, valid = hessian_target(rec, target_n, (s0+s1)/2)
        target_g, _ = normal_slopes(target_n, rec['axes'])
        bq = parameters(xyz[valid], rec); bw = w[valid]
        for axis in (0, 1):
            op = tensor(bq, knot, knot, int(axis == 0), int(axis == 1))/rec['size'][axis]
            normal_rows.append((op, target_g[valid, axis], bw))
        for dx, dy, i, j, factor in ((2, 0, 0, 0, 1), (1, 1, 0, 1, 2), (0, 2, 1, 1, 1)):
            op = tensor(bq, knot, knot, dx, dy)/rec['size'][0]**dx/rec['size'][1]**dy
            curvature_rows.append((op, target_h[valid, i, j], bw*factor))
    xyz = np.concatenate([b[0] for b in boundary]); bw = np.concatenate([b[1] for b in boundary]); bw /= bw.sum()
    b = tensor(parameters(xyz, rec), knot, knot); bz = (xyz-rec['origin'])@rec['axes'][2]
    add(b, bz, bw*cfg.boundary_weight)
    for rows, strength, dimensions in ((normal_rows, cfg.seam_normal_weight, 2), (curvature_rows, cfg.seam_curvature_weight, 4)):
        total = sum(float(w.sum()) for _, _, w in rows)/dimensions
        if total:
            for op, target, weight in rows:
                add(op, target, weight*(strength/total))
    variation = bending_matrix(knot, rec['size'], 3)*cfg.variation_length**6
    h = h+variation
    rhs -= variation@old
    h = h.tocsc(); diagonal = max(float(h.diagonal().max()), 1.)
    h += sparse.eye(len(old), format='csc')*(diagonal*1e-12)
    import osqp
    before_boundary = b@old-bz
    bound = max(cfg.support_error, float(abs(before_boundary).max())+1e-9)
    solver = osqp.OSQP()
    constraint = sparse.vstack([b, sparse.eye(len(old))], format='csc')
    lower = np.r_[-bound-before_boundary, np.full(len(old), -movement)]
    upper = np.r_[bound-before_boundary, np.full(len(old), movement)]
    solver.setup(P=sparse.triu(h/diagonal, format='csc'), q=-rhs/diagonal,
                 A=constraint, l=lower, u=upper, verbose=False, polishing=False,
                 max_iter=2000, time_limit=cfg.solver_seconds, eps_abs=1e-7, eps_rel=1e-7)
    solved = solver.solve(raise_error=False)
    displacement = solved.x
    if displacement is None:
        return di, old, dict(accepted=False, reason='no_bounded_proposal')
    if not np.isfinite(displacement).all():
        return di, old, dict(accepted=False, reason='nonfinite_linear_solution')
    fraction = min(1., movement/max(float(abs(displacement).max()), 1e-30))
    fraction = min(fraction, feasible_fraction(before_boundary, b@displacement, -bound, bound))
    # Fixed independent samples: all source vertices and centers, two Gauss
    # points in each support knot span, and every incident trim sample.
    uv = parameters(v[used], rec)
    from .mesh import Locator
    tri = np.searchsorted(used, f[ids])
    spans = np.unique(knot); nodes = np.polynomial.legendre.leggauss(2)[0]
    grid = (spans[:-1, None]+np.diff(spans)[:, None]*(nodes+1)/2).ravel()
    iq = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    projected = Locator(np.c_[uv, np.zeros(len(uv))], tri)
    _, distance, _, _ = projected.closest(np.c_[iq, np.zeros(len(iq))])
    probe_q = np.r_[uv, q, iq[distance < 1e-10], parameters(xyz, rec)]
    base_xyz = positions(probe_q, rec)
    op = tensor(probe_q, knot, knot); delta = op@displacement
    base_distance = state['locator'].closest(base_xyz)[1]
    maximum = max(state['distance'], float(base_distance.max())+1e-9)
    accepted = False; reason = 'bounded_guard'; error = []
    # At most one half-step after the first proposal; never re-solve.
    for attempt in range(2):
        candidate = dict(rec, height=old+fraction*displacement)
        distances = state['locator'].closest(base_xyz+(fraction*delta)[:, None]*rec['axes'][2])[1]
        if float(distances.max()) <= maximum:
            error = [spline_graph_error(c['poles'], c['knots'], candidate) for _, _, c in boundary]
            if max(error) <= bound and fraction > 1e-5:
                accepted = True; break
        fraction *= .5
    result = candidate['height'] if accepted else old
    # Keep all geometry-dependent error evidence separate from old fit metadata.
    return di, result, dict(accepted=accepted, reason='accepted' if accepted else reason,
        fraction=fraction if accepted else 0., attempts=attempt+1,
        maximum_control_movement_mm=float(abs(result-old).max()),
        local_movement_budget_mm=movement,
        source_probe_count=len(probe_q), source_max_before_mm=float(base_distance.max()),
        source_max_after_mm=float(distances.max()) if accepted else float(base_distance.max()),
        boundary_max_mm=max(error) if accepted else None,
        solver_status=solved.info.status, solver_seconds=solved.info.run_time,
        seconds=time.perf_counter()-tick)


def fair(source, output, config=None, runtime=None):
    from .shared_support import source_angles, domain_widths
    from .source_normals import crease_fan_normals
    tick = time.perf_counter(); cfg = config or Config()
    if (cfg.workers < 1 or min(cfg.movement, cfg.variation_length, cfg.support_error) <= 0
            or min(cfg.normal_weight, cfg.seam_normal_weight, cfg.seam_curvature_weight, cfg.boundary_weight) < 0):
        raise ValueError('Invalid transition fairing configuration')
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    records = {int(p.stem): dict(np.load(p)) for p in sorted((root/'charts').glob('*.npz'))}
    curves = {int(p.stem): dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
    if any(len(r.get('shared_support_group', [di])) != 1 for di, r in records.items()):
        raise ValueError('This independent pass requires one record per support identity')
    with np.load(meta['partition']) as data:
        v, f = data['mesh_vertex_original'], data['mesh_triangle']
    lookup = source_angles(v, f); incident = {di: [] for di in records}; angles = {}
    for ci, curve in curves.items():
        for di in curve['owners']:
            incident[int(di)].append(ci)
        angles[ci] = np.array([lookup[tuple(sorted((int(a), int(b))))] for a, b in zip(curve['chain'][:-1], curve['chain'][1:])])
    normals = crease_fan_normals(v, f, meta['config']['feature_angle'], filter_steps=2)
    widths = domain_widths(records, curves, v, f)
    state = dict(config=cfg, records=records, curves=curves, vertices=v, triangles=f, incident=incident, widths=widths,
                 angles=angles, normals=normals, distance=meta['config']['distance'])
    (out/'charts').mkdir(); rows = {}; preparation = time.perf_counter()-tick
    print(dict(stage='transition_targets', seconds=preparation, charts=len(records)), flush=True)
    pool = ProcessPoolExecutor(max_workers=cfg.workers, initializer=_initialize, initargs=(state,))
    if runtime is not None:
        runtime.update(pool=pool, partition=meta['partition'], records=records, curves=curves, vertices=v, triangles=f, widths=widths)
    try:
        for di, height, report in pool.map(_worker, sorted(records), chunksize=4):
            rec = dict(records[di], height=height)
            if report['accepted']:
                rec.update(transition_fair_version=1, transition_source_max_mm=report['source_max_after_mm'],
                           continuous_boundary_error_mm=report['boundary_max_mm'])
            np.savez_compressed(out/'charts'/f'{di:05d}.npz', **rec); rows[di] = report
            if runtime is not None:
                records[di] = rec
    finally:
        if runtime is None:
            pool.shutdown()
    for name in ('curves', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    shutil.copy2(root/'chart_index.npz', out/'chart_index.npz')
    changed = [di for di, row in rows.items() if row['accepted']]
    report = dict(algorithm='bounded-linear-transition-fairing-v1', config=asdict(cfg),
        source_fit=str(root.resolve()), seconds=time.perf_counter()-tick, preparation_seconds=preparation,
        charts_considered=len(records), charts_changed=len(changed),
        regions_changed=sorted({int(records[di]['region']) for di in changed}),
        source_max_after_mm=max(r['source_max_after_mm'] for r in rows.values() if 'source_max_after_mm' in r),
        adaptive_subdivisions=0, bounded_quadratic_solves_per_chart=1, maximum_guard_trials=2, per_chart=rows,
        scope='Frozen common normal and shape-operator targets. Sampled source distance and continuous trim/support checks; no exact G1/G2 or Hausdorff certificate.')
    dump(out/'transition_fairing.json', report)
    meta.update(source_fit=str(out.resolve()), incremental_source=str(root.resolve()), status='fitted',
                requires_support_refit=False, transition_fairing='transition_fairing.json')
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(); p.add_argument('--source', required=True); p.add_argument('--output', required=True)
    p.add_argument('--workers', type=int, default=8)
    a = p.parse_args(); r = fair(a.source, a.output, Config(workers=a.workers))
    print({k: v for k, v in r.items() if k not in ('per_chart', 'regions_changed')}, flush=True)
