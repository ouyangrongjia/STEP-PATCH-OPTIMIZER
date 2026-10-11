"""Fair the complete trim network on existing native graph supports.

This incremental route changes shared curves, not fitted surface coefficients.
Each proposal has a fixed iteration count and is accepted only after continuous
curve/support and winding checks. Failed proposals retain their input geometry.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, asdict
import json
from pathlib import Path
import shutil
import time

import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline

from .splines import sparse_basis, tensor
from .partition_fit import parameters, positions, spline_graph_error
from .boundary_quality import measures
from .mesh import dump


_SOURCE_LOCATOR = None
_DISTANCE_LIMIT = None


def _initialize(partition, distance):
    global _SOURCE_LOCATOR, _DISTANCE_LIMIT
    from .mesh import Locator
    with np.load(partition) as data:
        _SOURCE_LOCATOR = Locator(data['mesh_vertex_original'], data['mesh_triangle'])
    _DISTANCE_LIMIT = distance


@dataclass
class Config:
    movement: float = .12
    fairness_length: float = .3
    support_error: float = .003
    iterations: int = 3
    workers: int = 8
    feature_weight: float = 0.
    solver_seconds: float = .15
    feature_fraction: float = 0.


def graph_jet(xyz, rec):
    uv = parameters(xyz, rec); t = rec['knots']
    residual = tensor(uv, t, t)@rec['height']-(xyz-rec['origin'])@rec['axes'][2]
    gradient = sum((tensor(uv, t, t, int(k == 0), int(k == 1))@rec['height'])[:, None]
                   *rec['axes'][k]/rec['size'][k] for k in range(2))-rec['axes'][2]
    return residual, gradient


def scalar_rows(basis, vectors):
    """Map point-major XYZ poles to one directional value per sample."""
    b = basis.tocoo()
    return sparse.csr_matrix((
        (b.data[:, None]*vectors[b.row]).ravel(),
        (np.repeat(b.row, 3), (b.col[:, None]*3+np.arange(3)).ravel())),
        shape=(basis.shape[0], basis.shape[1]*3))


def endpoint_cones(curves, regions, vertices):
    """Each incident tangent can move toward a common bisector, never past it.

    Independent acceptance/rollback therefore cannot enlarge the paired kink.
    Routing uses macroscopic source secants, so true corners remain unpaired.
    """
    from .curve_routes import route_graph, Config as RouteConfig
    targets = {ci: BSpline(c['knots'], c['poles'], 3)([0., 1.], 1) for ci, c in curves.items()}
    original = {ci: value.copy() for ci, value in targets.items()}
    for kind in ('regional', 'internal'):
        config = RouteConfig(interface_kind=kind, cross_region_junctions=True)
        for route in route_graph(curves, regions, vertices, config):
            pairs = list(zip(route['members'][:-1], route['members'][1:]))
            if route['closed'] and len(route['members']) > 1:
                pairs.append((route['members'][-1], route['members'][0]))
            for (a, forward_a), (b, forward_b) in pairs:
                end_a, end_b = int(forward_a), int(not forward_b)
                u = original[a][end_a]*(1 if forward_a else -1)
                w = original[b][end_b]*(1 if forward_b else -1)
                u /= max(np.linalg.norm(u), 1e-30); w /= max(np.linalg.norm(w), 1e-30)
                if np.dot(u, w) <= -.5:
                    continue
                mid = u+w; mid /= max(np.linalg.norm(mid), 1e-30)
                targets[a][end_a] = mid*(1 if forward_a else -1)
                targets[b][end_b] = mid*(1 if forward_b else -1)
    return targets


def tangent_constraints(knot, poles, targets):
    operators, low, high = [], [], []
    for end in (0, 1):
        row = sparse_basis(knot, [float(end)], 1)
        old = np.asarray(row@poles).ravel(); speed = np.linalg.norm(old)
        if speed < 1e-12:
            continue
        u = old/speed; target = targets[end]/np.linalg.norm(targets[end])
        normal = np.cross(u, target); size = np.linalg.norm(normal)
        if size > 1e-6:
            normal /= size
            vectors = [normal, np.cross(normal, u), np.cross(target, normal)]
            lows, highs = [0., 0., 0.], [0., np.inf, np.inf]
        else:
            axis = np.eye(3)[np.argmin(abs(u))]
            normal = np.cross(u, axis); normal /= np.linalg.norm(normal)
            vectors = [normal, np.cross(normal, u)]
            lows, highs = [0., 0.], [0., 0.]
        vectors.append((u+target)/np.linalg.norm(u+target))
        lows.append(speed*.05); highs.append(speed*5.)
        for direction, lo, hi in zip(vectors, lows, highs):
            operators.append(scalar_rows(row, direction[None]))
            offset = float(old@direction)
            low.append(lo-offset); high.append(hi-offset)
    return sparse.vstack(operators, format='csr'), np.array(low), np.array(high)


def propose(curve, supports, config, targets=None, feature_guide=None):
    import osqp
    started = time.perf_counter()
    old = curve['poles']; knot = curve['knots']; n = len(old)
    original = BSpline(knot, old, 3)
    before = measures(curve)
    length = before['length_mm']
    result = dict(accepted=False, before=before)
    if length < .03 or before['total_tangent_turn_degrees'] < 1.:
        return curve, dict(result, reason='already_smooth_or_short', seconds=time.perf_counter()-started)
    cuts = np.unique(np.r_[knot, curve['parameters']])
    q = np.unique(np.r_[np.linspace(0, 1, 33),
                       (cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 4)).ravel()])
    b = sparse_basis(knot, q)
    # Integrate derivatives per span; use physical length to keep the same
    # smoothing scale on long and short edges.
    nodes, weights = np.polynomial.legendre.leggauss(4)
    u = (cuts[:-1, None]+np.diff(cuts)[:, None]*(nodes+1)/2).ravel()
    w = (np.diff(cuts)[:, None]*weights/2).ravel()
    bq, d2 = sparse_basis(knot, u), sparse_basis(knot, u, 2)
    fair = min(config.fairness_length/length, .3)**4
    h = bq.T@bq.multiply(w[:, None])+fair*(d2.T@d2.multiply(w[:, None]))
    # Solve for displacements to avoid accuracy depending on model coordinates.
    h3 = sparse.kron(h, sparse.eye(3), format='csc')
    linear = (fair*(d2.T@(w[:, None]*(d2@old)))).ravel()
    if feature_guide is not None and config.feature_weight:
        guide_q, guide_xyz, guide_weight = feature_guide
        guide = sparse_basis(knot, guide_q)
        weight = guide_weight/guide_weight.sum()*config.feature_weight
        h3 += sparse.kron(guide.T@guide.multiply(weight[:, None]), sparse.eye(3), format='csc')
        linear += (guide.T@(weight[:, None]*(guide@old-guide_xyz))).ravel()
    scale = max(float(abs(h3.diagonal()).max()), 1.)
    h3 /= scale; linear /= scale
    endpoint = sparse.kron(sparse_basis(knot, [0., 1.]), sparse.eye(3), format='csr')
    targets = original([0., 1.], 1) if targets is None else targets
    tangent, tangent_lo, tangent_hi = tangent_constraints(knot, old, targets)
    cp_identity = sparse.eye(3*n, format='csr')
    static = [endpoint, tangent, cp_identity]
    lower = [np.zeros(6), tangent_lo, np.full(3*n, -config.movement/np.sqrt(3))]
    upper = [np.zeros(6), tangent_hi, np.full(3*n, config.movement/np.sqrt(3))]
    xyz0 = b@old
    for rec in supports:
        uv = parameters(xyz0, rec)
        for k in range(2):
            op = scalar_rows(b, np.broadcast_to(rec['axes'][k]/rec['size'][k], xyz0.shape))
            static.append(op); lower.append(-uv[:, k]); upper.append(1-uv[:, k])
    displacement = np.zeros_like(old)
    statuses = []
    for iteration in range(config.iterations):
        blocks, lo, hi = list(static), list(lower), list(upper)
        xyz = b@(old+displacement)
        for rec in supports:
            r, gradient = graph_jet(xyz, rec)
            op = scalar_rows(b, gradient)
            target = op@displacement.ravel()-r
            blocks.append(op)
            lo.append(target-config.support_error*.9); hi.append(target+config.support_error*.9)
        solver = osqp.OSQP()
        solver.setup(P=sparse.triu(h3, format='csc'), q=linear,
                     A=sparse.vstack(blocks, format='csc'), l=np.concatenate(lo), u=np.concatenate(hi),
                     verbose=False, eps_abs=1e-7, eps_rel=1e-7, max_iter=1200,
                     polishing=True, time_limit=config.solver_seconds)
        solver.warm_start(x=displacement.ravel())
        solved = solver.solve(raise_error=False)
        statuses.append(solved.info.status)
        if solved.x is None or not np.isfinite(solved.x).all() or solved.info.status_val not in (1, 2, 7, 8):
            return curve, dict(result, reason='bounded_solver_'+solved.info.status,
                               seconds=time.perf_counter()-started)
        displacement = solved.x.reshape(-1, 3)
    poles = old+displacement
    poles[[0, -1]] = old[[0, -1]]
    values = tangent@(poles-old).ravel()
    if max(np.max(tangent_lo-values), np.max(values-tangent_hi)) > 1e-6:
        return curve, dict(result, reason='tangent_cone', seconds=time.perf_counter()-started)
    if np.max(np.linalg.norm(displacement, axis=1)) > config.movement*(1+1e-5):
        return curve, dict(result, reason='movement', seconds=time.perf_counter()-started)
    candidate = dict(curve, poles=poles)
    if feature_guide is not None and config.feature_weight:
        before_guide = float(np.average(np.sum((guide@old-guide_xyz)**2, axis=1), weights=guide_weight))
        after_guide = float(np.average(np.sum((guide@poles-guide_xyz)**2, axis=1), weights=guide_weight))
        result['source_crease_rms_mm'] = [np.sqrt(before_guide), np.sqrt(after_guide)]
        if after_guide > before_guide+1e-12:
            return curve, dict(result, reason='source_crease_fidelity', seconds=time.perf_counter()-started)
    after = measures(candidate)
    if (after['curvature_squared_integral_per_mm'] > before['curvature_squared_integral_per_mm']*.98
            or after['total_tangent_turn_degrees'] > before['total_tangent_turn_degrees']*1.01+.1):
        return curve, dict(result, reason='no_fairness_gain', after=after, seconds=time.perf_counter()-started)
    try:
        errors = [spline_graph_error(poles, knot, rec) for rec in supports]
    except ValueError:
        return curve, dict(result, reason='support_domain', seconds=time.perf_counter()-started)
    if max(errors) > config.support_error:
        return curve, dict(result, reason='continuous_support_error', errors=errors, seconds=time.perf_counter()-started)
    result.update(accepted=True, after=after, support_error_mm=errors,
                  solver_statuses=statuses,
                  max_control_displacement_mm=float(np.linalg.norm(displacement, axis=1).max()),
                  seconds=time.perf_counter()-started)
    candidate.update(method='fixed_support_boundary_fairing', prior_method=curve.get('method', ''),
                     incremental_movement_mm=result['max_control_displacement_mm'])
    for key in list(candidate):
        if key.startswith(('route_', 'source_tube_')) or key in ('source_plane_error_mm', 'endpoint_tangent_locked'):
            candidate['prior_'+key] = candidate.pop(key)
    candidate['movement_mm'] = float(curve.get('movement_mm', 0.))+result['max_control_displacement_mm']
    candidate['movement_measure'] = 'Conservative original source-chain bound plus incremental control-hull bound'
    candidate['movement_budget_mm'] = float(curve.get('movement_budget_mm', 0.))+config.movement
    return candidate, result


def _worker(args):
    ci, c, supports, config, targets, guide = args
    try:
        candidate, report = propose(c, supports, config, targets, guide)
        if report['accepted'] and _SOURCE_LOCATOR is not None:
            tick = time.perf_counter()
            check = swept_band_check(c, candidate, supports, _SOURCE_LOCATOR, _DISTANCE_LIMIT)
            report['swept_band'] = check
            report['swept_band_worker_seconds'] = time.perf_counter()-tick
            if not check['passed']:
                candidate = c; report.update(accepted=False, reason='swept_band_guard')
        if not report['accepted']:
            report['support_error_mm'] = [spline_graph_error(candidate['poles'], candidate['knots'], r) for r in supports]
    except Exception as error:
        candidate, report = c, dict(accepted=False, reason=f'{type(error).__name__}: {error}')
    return ci, candidate, report


def swept_band_check(curve, candidate, supports, locator, distance_limit):
    """Probe the newly exposed support strips, not only old triangle centers."""
    from .surface_jets import jets
    cuts = np.unique(np.r_[candidate['knots'], curve['knots']])
    q = np.unique(np.r_[np.linspace(0, 1, 41),
                       (cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel()])
    a = BSpline(curve['knots'], curve['poles'], 3)(q)
    b = BSpline(candidate['knots'], candidate['poles'], 3)(q)
    xyz = (a[:, None]+(b-a)[:, None]*np.linspace(0, 1, 5)[None, :, None]).reshape(-1, 3)
    distance = max(float(locator.closest(positions(parameters(xyz, rec), rec))[1].max()) for rec in supports)
    angles, curvatures = [], []
    for points in (a, b):
        (n0, s0), (n1, s1) = [jets(rec, points) for rec in supports]
        angles.append(float(np.sqrt(np.mean(np.degrees(np.arccos(np.clip(np.sum(n0*n1, axis=1), -1., 1.)))**2))))
        curvatures.append(float(np.sqrt(np.mean(np.sum((s0-s1)**2, axis=(1, 2))))))
    return dict(passed=bool(distance <= distance_limit and angles[1] <= angles[0]+.5
                           and curvatures[1] <= curvatures[0]+max(.5, curvatures[0]*.1)),
                samples_per_support=len(xyz), source_distance_max_mm=distance,
                seam_normal_rms_degrees=angles, seam_shape_jump_rms_per_mm=curvatures,
                scope='Sampled swept strips on unchanged native supports; finite source triangles. Not an exact G1/G2 certificate.')


def feature_guides(vertices, triangles, curves, crease_degrees):
    """Only original STL dihedral creases attract curves; labels do not."""
    from .partition_fit import incidence
    edges, owners = incidence(triangles)
    normal = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                      vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
    normal /= np.linalg.norm(normal, axis=1)[:, None]
    crease = np.sum(normal[owners[:, 0]]*normal[owners[:, 1]], axis=1) < np.cos(np.radians(crease_degrees))
    key = edges[:, 0]*len(vertices)+edges[:, 1]; result = {}
    for ci, curve in curves.items():
        chain = curve['chain']; a, b = chain[:-1], chain[1:]
        ids = np.searchsorted(key, np.minimum(a, b)*len(vertices)+np.maximum(a, b))
        selected = crease[ids]
        if selected.any():
            q = (curve['parameters'][:-1]+curve['parameters'][1:])/2
            p = (vertices[a]+vertices[b])/2
            length = np.linalg.norm(vertices[b]-vertices[a], axis=1)
            result[ci] = (q[selected], p[selected], length[selected])
    return result


def refine(source, output, config=None, certificates=None, runtime=None):
    from .network_support import validate_wires
    started = time.perf_counter(); config = config or Config()
    root, out = Path(source), Path(output)
    out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if config.support_error > meta['config']['boundary']*.85:
        raise ValueError('Continuous support tolerance must fit the existing CAD boundary budget')
    if runtime is None:
        records = {int(p.stem): dict(np.load(p)) for p in sorted((root/'charts').glob('*.npz'))}
        original = {int(p.stem): dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
        data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    else:
        if runtime['partition'] != meta['partition']:
            raise ValueError('Workers belong to another source partition')
        records, original, v, f = (runtime[k] for k in ('records', 'curves', 'vertices', 'triangles'))
    targets = endpoint_cones(original, {di: int(r['region']) for di, r in records.items()}, v)
    guides = feature_guides(v, f, original, meta['config']['feature_angle']) if config.feature_weight else {}
    widths = None
    if config.feature_fraction:
        from .shared_support import domain_widths
        widths = runtime['widths'] if runtime is not None else domain_widths(records, original, v, f)
    curves, reports = {}, {}
    from dataclasses import replace
    jobs = [(ci, c, [records[int(di)] for di in c['owners']],
             config if widths is None else replace(config, movement=min(config.movement,
                 config.feature_fraction*min(widths[int(di)] for di in c['owners']))),
             targets[ci], guides.get(ci)) for ci, c in original.items()]
    from contextlib import nullcontext
    manager = (ProcessPoolExecutor(max_workers=config.workers, initializer=_initialize,
                                  initargs=(meta['partition'], meta['config']['distance']))
               if runtime is None else nullcontext(runtime['pool']))
    with manager as pool:
        for ci, candidate, report in pool.map(_worker, jobs, chunksize=8):
            curves[ci] = candidate; reports[ci] = report
    fit_seconds = time.perf_counter()-started
    changed = {ci for ci, r in reports.items() if r['accepted']}
    selected = {int(di) for ci in changed for di in curves[ci]['owners']}
    failures = validate_wires(v, f, records, curves, selected, native_frame=True)
    # Fixed junctions make one conservative chart-neighborhood rollback enough;
    # there is no retriangulation or adaptive support retry loop.
    restored = {ci for fail in failures for ci, c in curves.items() if fail['chart'] in c['owners']}
    for ci in restored & changed:
        curves[ci] = original[ci]; reports[ci].update(accepted=False, reason='wire_rollback')
        reports[ci]['support_error_mm'] = [spline_graph_error(curves[ci]['poles'], curves[ci]['knots'], records[int(di)])
                                           for di in curves[ci]['owners']]
    changed -= restored
    if failures:
        unresolved = validate_wires(v, f, records, curves, selected, native_frame=True)
        if unresolved:
            dump(out/'failure.json', unresolved)
            raise ValueError('Boundary winding remains invalid after bounded rollback')
    for name in ('charts', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    (out/'curves').mkdir()
    for ci, curve in curves.items():
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **curve)
    shutil.copy2(root/'chart_index.npz', out/'chart_index.npz')
    summary = dict(algorithm='fixed-support-boundary-fairing-v1', source_fit=str(root.resolve()),
        config=asdict(config), curves_considered=len(original), curves_changed=len(changed),
        regions_changed=sorted({int(records[int(di)]['region']) for ci in changed for di in curves[ci]['owners']}),
        charts_changed=0, surface_coefficients_changed=False,
        source_crease_guided_curves=len(guides),
        minimum_local_movement_mm=min(job[3].movement for job in jobs),
        native_piece_curvature_before=sum(r['before']['curvature_squared_integral_per_mm'] for r in reports.values() if 'before' in r),
        native_piece_curvature_after=sum(r['after' if r['accepted'] else 'before']['curvature_squared_integral_per_mm'] for r in reports.values() if 'before' in r),
        optimization_and_parallel_guard_seconds=fit_seconds, seconds=time.perf_counter()-started,
        wire_rollback_curves=sorted(restored & set(reports)), per_curve=reports)
    dump(out/'boundary_refinement.json', summary)
    meta.update(source_fit=str(out.resolve()), incremental_source=str(root.resolve()),
                requires_support_refit=False, boundary_refinement='boundary_refinement.json', status='fitted')
    dump(out/'run.json', meta)
    if certificates is not None:
        certificates.update(records=records, curves=curves,
                            errors={ci: r['support_error_mm'] for ci, r in reports.items()})
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--fairness-length', type=float, default=.3)
    parser.add_argument('--movement', type=float, default=.12)
    args = parser.parse_args()
    report = refine(args.source, args.output, Config(workers=args.workers, fairness_length=args.fairness_length, movement=args.movement))
    print({k: v for k, v in report.items() if k not in ('per_curve', 'regions_changed', 'wire_rollback_curves')}, flush=True)
