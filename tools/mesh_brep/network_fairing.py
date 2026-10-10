"""Fair a shared spline network, including its non-feature junctions.

The source mesh supplies only a positional tube and crease evidence. Junction
coordinates are shared unknowns; a curve is never moved independently from an
incident curve. This module does not turn a curve proposal into fitted surfaces.
"""
import argparse
from collections import defaultdict
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import shutil
import time

import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline

from .mesh import Locator, dump
from .splines import sparse_basis
from .surface_boundary import certify_tube
from .boundary_quality import measures
from .regional_rectangle import curve_polyline_error


@dataclass
class Config:
    movement: float = .12
    tube: float = .012
    fairness_length: float = .2
    variation_weight: float = 0.
    continuation_weight: float = .1
    feature_angle: float = 30.
    iterations: int = 12
    samples: int = 65
    solver_seconds: float = 45.


def incidence(curves):
    nodes = defaultdict(list)
    for ci, c in curves.items():
        for end in (0, 1):
            nodes[int(c['chain'][0 if end == 0 else -1])].append((ci, end))
    return dict(nodes)


def junctions(curves, selected, sharp, feature_angle):
    """Freeze genuine crease corners and the boundary of the edited network."""
    result = {}
    for vi, uses in incidence(curves).items():
        if not any(ci in selected for ci, _ in uses):
            continue
        xyz = np.array([curves[ci]['poles'][0 if end == 0 else -1] for ci, end in uses])
        if np.max(np.linalg.norm(xyz-xyz[0], axis=1)) > 1e-8:
            raise ValueError('Input curves disagree on a shared junction')
        feature = [(ci, end) for ci, end in uses if sharp.get(ci, False)]
        directions = []
        for ci, end in feature:
            c = curves[ci]; d = BSpline(c['knots'], c['poles'], 3)(float(end), 1)
            d *= 1 if end == 0 else -1
            directions.append(d/max(np.linalg.norm(d), 1e-30))
        corner = len(feature) != 0 and len(feature) != 2
        if len(feature) == 2:
            corner = np.dot(*directions) > -np.cos(np.radians(feature_angle))
        fixed = corner or any(ci not in selected for ci, _ in uses)
        result[vi] = dict(uses=uses, position=xyz[0], fixed=bool(fixed), feature_corner=bool(corner))
    return result


def normal_operator(basis, normal):
    """Dot a vector-valued spline with one direction at each sample."""
    b = basis.tocoo()
    rows = np.repeat(b.row, 3)
    cols = (b.col[:, None]*3+np.arange(3)).ravel()
    values = (b.data[:, None]*normal[b.row]).ravel()
    return sparse.coo_matrix((values, (rows, cols)), shape=(basis.shape[0], basis.shape[1]*3)).tocsr()


def pad(matrix, offset, total):
    a = matrix.tocoo()
    return sparse.coo_matrix((a.data, (a.row, a.col+offset)), shape=(a.shape[0], total)).tocsr()


def line_candidate(record, points, locator, movement, tube, endpoint_angle=15.):
    """Accept an exact line only when the finite source surface supports it."""
    end = record['poles'][[0, -1]]; direction = end[1]-end[0]
    length = np.linalg.norm(direction)
    if length < 1e-10:
        return None, dict(accepted=False, reason='closed_or_collapsed_curve')
    direction /= length
    tangent = BSpline(record['knots'], record['poles'], 3)([0., 1.], 1)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1)[:, None], 1e-30)
    angle = np.degrees(np.arccos(np.clip(tangent@direction, -1, 1)))
    if angle.max() > endpoint_angle:
        return None, dict(accepted=False, reason='endpoint_direction', max_angle_degrees=float(angle.max()))
    cp = np.linspace(*end, 4); knot = np.r_[np.zeros(4), np.ones(4)]
    certificate = certify_tube(cp, knot, locator, tube)
    displacement, _ = curve_polyline_error(cp, knot, points, record['parameters'])
    certificate.pop('violating_parameters', None)
    passed = bool(certificate['passed'] and displacement <= movement)
    info = dict(accepted=passed, source_certificate=certificate, movement_mm=displacement,
                max_endpoint_direction_change_degrees=float(angle.max()))
    if not passed:
        return None, info
    result = dict(record, poles=cp, knots=knot, movement_mm=displacement,
                  method='certified_straight_boundary', source_tube_certificate_version=1,
                  source_tube_limit_mm=tube, source_tube_certified_max_mm=certificate['certified_max_mm'])
    return result, info


def straighten(source, output, regions, *, minimum_length=.5, tube=.012, endpoint_angle=15.):
    from .partition_fit import spline_graph_error
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if not 0 < tube < meta['config']['distance'] or minimum_length <= 0 or not 0 < endpoint_angle < 90:
        raise ValueError('Invalid primitive boundary configuration')
    data = np.load(meta['partition']); v, f, labels = (data[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    assignment = np.load(root/'chart_index.npz')['face_chart']
    selected = set(np.unique(assignment[np.isin(labels, regions)]))
    records = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    changed = {}; attempts = []; locators = {}; supports = {}
    for ci, c in records.items():
        pair = tuple(map(int, c['owners']))
        if not selected.intersection(pair):
            continue
        before = measures(c)
        if before['length_mm'] < minimum_length or before['total_tangent_turn_degrees'] < 1.:
            continue
        if pair not in locators:
            ids = np.flatnonzero(np.isin(assignment, pair)); used = np.unique(f[ids])
            locators[pair] = Locator(v[used], np.searchsorted(used, f[ids]))
        result, info = line_candidate(c, v[c['chain']], locators[pair],
                                      min(meta['smoothing_mm'], float(c.get('movement_budget_mm', meta['smoothing_mm']))),
                                      tube, endpoint_angle)
        if result is not None:
            errors = []
            for di in pair:
                if di not in supports:
                    supports[di] = dict(np.load(root/'charts'/f'{di:05d}.npz'))
                try:
                    errors.append(spline_graph_error(result['poles'], result['knots'], supports[di]))
                except ValueError:
                    errors.append(None)
            info['incident_support_errors_mm'] = errors
            if any(e is None or e >= meta['config']['boundary']*.4 for e in errors):
                result = None; info.update(accepted=False, reason='incident_support_incompatible')
        info.update(curve=ci, before=before); attempts.append(info)
        if result is not None:
            changed[ci] = result
    report = dict(changed_curves=sorted(changed), attempts=attempts, minimum_length_mm=minimum_length,
                  tube_mm=tube, endpoint_angle_degrees=endpoint_angle, regions=list(regions),
                  source_fit=str(root.resolve()), source_CAD_read=False, commercial_engine_used=False)
    dump(out/'straight_boundaries.json', report)
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for ci, c in changed.items():
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **c)
    affected = {int(di) for c in changed.values() for di in c['owners']}
    if meta.get('requires_support_refit'):
        affected.update(meta['required_refit_charts'])
    meta.update(source_fit=str(out.resolve()), status='boundary_candidate', straight_boundaries=report,
                required_refit_charts=sorted(affected), requires_support_refit=bool(affected))
    dump(out/'run.json', meta)
    return report


def solve_network(curves, selected, source_points, locators, sharp, config):
    import osqp
    if not selected or not set(selected).issubset(curves):
        raise ValueError('A nonempty subset of existing curves is required')
    if (min(config.movement, config.tube, config.fairness_length, config.solver_seconds) <= 0
            or config.variation_weight < 0 or config.continuation_weight < 0
            or not 0 < config.feature_angle < 90 or config.iterations < 1 or config.samples < 9):
        raise ValueError('Invalid network fairing configuration')
    selected = sorted(selected)
    nodes = junctions(curves, set(selected), sharp, config.feature_angle)
    origin = np.mean([r['position'] for r in nodes.values()], axis=0)
    offsets = {}; total = 0; prepared = {}; objectives = []; linear = []
    for ci in selected:
        c = curves[ci]; offsets[ci] = total; total += 3*len(c['poles'])
        knot = c['knots']; spline = BSpline(knot, c['poles'], 3)
        q = np.unique(np.r_[c['parameters'], knot, np.linspace(0, 1, config.samples)])
        b = sparse_basis(knot, q); reference = source_points[ci]
        target = np.column_stack([np.interp(q, c['parameters'], reference[:, k]) for k in range(3)])
        length = max(float(np.linalg.norm(np.diff(reference, axis=0), axis=1).sum()), 1e-6)
        fair_length = min(config.fairness_length, length*.3)
        h = b.T@b*(length/len(q))
        cut = np.unique(knot); g, w = np.polynomial.legendre.leggauss(4)
        tq = (cut[:-1, None]+np.diff(cut)[:, None]*(g+1)/2).ravel()
        qw = (np.diff(cut)[:, None]*w/2).ravel()
        for order, strength in ((2, 1.), (3, config.variation_weight)):
            d = sparse_basis(knot, tq, order)
            h += d.T@d.multiply(qw[:, None])*(strength*fair_length**(2*order)/length**(2*order-1))
        objectives.append(sparse.kron(h, sparse.eye(3), format='csc'))
        linear.append(-(b.T@(target-origin)*length/len(q)).ravel())
        prepared[ci] = dict(q=q, basis=b, target=target, current=spline(q), spline=spline, length=length)
    h = sparse.block_diag(objectives, format='csc'); linear = np.concatenate(linear)
    fixed_a = []; fixed_lo = []; fixed_hi = []; continuation = []
    for node in nodes.values():
        uses = [(ci, end) for ci, end in node['uses'] if ci in offsets]
        columns = [offsets[ci]+(0 if end == 0 else 3*(len(curves[ci]['poles'])-1)) for ci, end in uses]
        anchor = columns[0]
        for other in columns[1:]:
            rows = np.repeat(np.arange(3), 2)
            cols = np.column_stack([anchor+np.arange(3), other+np.arange(3)]).ravel()
            fixed_a.append(sparse.coo_matrix((np.tile([1., -1.], 3), (rows, cols)), shape=(3, total)))
            fixed_lo.append(np.zeros(3)); fixed_hi.append(np.zeros(3))
        if node['fixed']:
            fixed_a.append(sparse.coo_matrix((np.ones(3), (np.arange(3), anchor+np.arange(3))), shape=(3, total)))
            fixed_lo.append(node['position']-origin); fixed_hi.append(node['position']-origin)
        # Continue the best aligned pair through an artificial graph junction.
        if not node['feature_corner'] and len(uses) >= 2 and config.continuation_weight:
            directions = []
            for ci, end in uses:
                d = prepared[ci]['spline'](float(end), 1)*(1 if end == 0 else -1)
                directions.append(d/max(np.linalg.norm(d), 1e-30))
            dot = np.asarray(directions)@np.asarray(directions).T
            dot[np.tril_indices(len(uses))] = 2.
            a, b = np.unravel_index(np.argmin(dot), dot.shape)
            if dot[a, b] < -np.cos(np.radians(config.feature_angle)):
                op = sparse.csr_matrix((3, total))
                for index in (a, b):
                    ci, end = uses[index]
                    d = sparse_basis(curves[ci]['knots'], np.array([float(end)]), 1)
                    op += pad(sparse.kron(d, sparse.eye(3)), offsets[ci], total)*(1 if end == 0 else -1)/prepared[ci]['length']
                continuation.append(op)
    if continuation:
        op = sparse.vstack(continuation, format='csr')
        h += op.T@op*(config.continuation_weight*config.fairness_length**2)
    for ci in selected:
        r = prepared[ci]; offset = offsets[ci]; c = curves[ci]
        op = pad(sparse.kron(r['basis'], sparse.eye(3)), offset, total)
        budget = min(config.movement, float(c.get('movement_budget_mm', config.movement)))
        fixed_a.append(op); fixed_lo.append((r['target']-origin).ravel()-budget/np.sqrt(3))
        fixed_hi.append((r['target']-origin).ravel()+budget/np.sqrt(3))
        d = sparse_basis(c['knots'], r['q'], 1)
        velocity = r['spline'](r['q'], 1); speed = np.linalg.norm(velocity, axis=1)
        good = speed > 1e-9
        op = pad(normal_operator(d[good], velocity[good]/speed[good, None]), offset, total)
        fixed_a.append(op); fixed_lo.append(speed[good]*.05); fixed_hi.append(np.full(good.sum(), np.inf))
    fixed_a = sparse.vstack(fixed_a, format='csc'); fixed_lo = np.concatenate(fixed_lo); fixed_hi = np.concatenate(fixed_hi)
    # A single maximum-diagonal normalization can erase the objective of long
    # curves next to a tiny, densely knotted curve. Equilibrate variables instead.
    scale = np.sqrt(np.maximum(h.diagonal(), np.median(h.diagonal())*1e-12))
    inverse_scale = sparse.diags(1/scale, format='csc')
    h = inverse_scale@h@inverse_scale+sparse.eye(total, format='csc')*1e-14
    linear /= scale
    warm = np.concatenate([(curves[ci]['poles']-origin).ravel() for ci in selected])
    history = []; best = None
    for iteration in range(config.iterations):
        blocks = [fixed_a]; lower = [fixed_lo]; upper = [fixed_hi]
        for ci in selected:
            r = prepared[ci]; closest, distance, tri, bary = locators[ci].closest(r['current'])
            # Distance to a finite triangle union is 1-Lipschitz. A local
            # spatial trust region prevents jumping across a narrow groove to
            # another plane during the sequential linearization.
            radius = np.maximum(config.tube*.15, (config.tube-distance)*.65)
            trust = pad(sparse.kron(r['basis'], sparse.eye(3)), offsets[ci], total)
            center_xyz = (r['current']-origin).ravel()
            blocks.append(trust)
            lower.append(center_xyz-np.repeat(radius/np.sqrt(3), 3))
            upper.append(center_xyz+np.repeat(radius/np.sqrt(3), 3))
            nv = locators[ci].source_normals[tri]
            op = pad(normal_operator(r['basis'], nv), offsets[ci], total)
            center = np.sum(nv*(closest-origin), axis=1)
            blocks.append(op); lower.append(center-config.tube*.75); upper.append(center+config.tube*.75)
            active = distance > config.tube*.1
            if active.any():
                direction = (r['current'][active]-closest[active])/distance[active, None]
                blocks.append(pad(normal_operator(r['basis'][active], direction), offsets[ci], total))
                center = np.sum(direction*(closest[active]-origin), axis=1)
                lower.append(np.full(active.sum(), -np.inf)); upper.append(center+config.tube*.75)
        constraint = sparse.vstack(blocks, format='csc')
        lower = np.concatenate(lower); upper = np.concatenate(upper)
        model = osqp.OSQP()
        model.setup(P=sparse.triu(h, format='csc'), q=linear, A=constraint@inverse_scale,
                    l=lower, u=upper, eps_abs=2e-7, eps_rel=2e-7,
                    max_iter=20000, time_limit=config.solver_seconds, verbose=False, polishing=True)
        model.warm_start(x=warm*scale); solved = model.solve(raise_error=False)
        value = constraint@(solved.x/scale)
        violation = float(max(np.max(lower-value), np.max(value-upper), 0.))
        event = dict(iteration=iteration, status=solved.info.status, seconds=solved.info.run_time,
                     constraint_violation_mm=violation, solver_iterations=solved.info.iter)
        history.append(event)
        # A time-limited iterate is only a proposal, never an optimum. Shared
        # endpoints are projected exactly; continuous geometric certificates,
        # rather than these conservative linearizations, decide acceptance.
        event['linear_constraints_satisfied'] = violation <= 2e-6
        if solved.info.status_val not in (1, 2, 7, 8) or not np.isfinite(value).all() or violation > config.tube*.1:
            break
        warm = solved.x/scale; candidates = {}; maximum = 0.
        for ci in selected:
            n = len(curves[ci]['poles']); cp = warm[offsets[ci]:offsets[ci]+3*n].reshape(n, 3)+origin
            candidates[ci] = dict(curves[ci], poles=cp)
        # Store exactly identical endpoints, independent of QP residuals.
        for node in nodes.values():
            uses = [(ci, end) for ci, end in node['uses'] if ci in candidates]
            point = node['position'] if node['fixed'] else np.mean([candidates[ci]['poles'][0 if end == 0 else -1] for ci, end in uses], axis=0)
            for ci, end in uses:
                candidates[ci]['poles'][0 if end == 0 else -1] = point
        for ci, c in candidates.items():
            r = prepared[ci]; r['current'] = BSpline(c['knots'], c['poles'], 3)(r['q'])
            maximum = max(maximum, float(locators[ci].closest(r['current'])[1].max()))
        event['sampled_source_max_mm'] = maximum
        print(dict(stage='network_fairing', **event), flush=True)
        if maximum <= config.tube*.95:
            failures = []; certificates = {}
            for ci, c in candidates.items():
                certificate = certify_tube(c['poles'], c['knots'], locators[ci], config.tube)
                movement, _ = curve_polyline_error(c['poles'], c['knots'], source_points[ci], c['parameters'])
                if not certificate['passed'] or movement > min(config.movement, float(c.get('movement_budget_mm', config.movement)))*(1+1e-6):
                    failures.append(ci)
                certificate.pop('violating_parameters', None); certificates[ci] = certificate
                c.update(movement_mm=movement, method='coupled_junction_fairing',
                         source_tube_certificate_version=1, source_tube_limit_mm=config.tube,
                         source_tube_certified_max_mm=certificate.get('certified_max_mm', maximum),
                         source_tube_domains=1, junction_network_version=1)
            event['certificate_failures'] = failures
            if not failures:
                best = candidates; break
    if best is None:
        return None, dict(accepted=False, history=history, reason='network_constraints_not_satisfied')
    before = [measures(curves[i]) for i in selected]; after = [measures(best[i]) for i in selected]
    sums = {side: {key: float(sum(r[key] for r in records)) for key in before[0]}
            for side, records in [('before', before), ('after', after)]}
    line_search = []
    if sums['after']['curvature_squared_integral_per_mm'] > sums['before']['curvature_squared_integral_per_mm']*.999:
        for alpha in (.5, .25, .125, .0625):
            trial = {ci: dict(best[ci], poles=curves[ci]['poles']+alpha*(best[ci]['poles']-curves[ci]['poles'])) for ci in selected}
            trial_measures = [measures(trial[ci]) for ci in selected]
            totals = {key: float(sum(r[key] for r in trial_measures)) for key in before[0]}
            item = dict(alpha=alpha, totals=totals); line_search.append(item)
            if (totals['curvature_squared_integral_per_mm'] >= sums['before']['curvature_squared_integral_per_mm']*.999 or
                    totals['total_tangent_turn_degrees'] > sums['before']['total_tangent_turn_degrees']):
                continue
            valid = True
            for ci, c in trial.items():
                certificate = certify_tube(c['poles'], c['knots'], locators[ci], config.tube)
                movement, _ = curve_polyline_error(c['poles'], c['knots'], source_points[ci], c['parameters'])
                if not certificate['passed'] or movement > min(config.movement, float(c.get('movement_budget_mm', config.movement))):
                    valid = False; break
                c.update(movement_mm=movement, source_tube_certified_max_mm=certificate['certified_max_mm'])
                certificate.pop('violating_parameters', None); certificates[ci] = certificate
            item['certificates_passed'] = valid
            if valid:
                best = trial; sums['after'] = totals; break
    if sums['after']['curvature_squared_integral_per_mm'] > sums['before']['curvature_squared_integral_per_mm']*.999:
        return None, dict(accepted=False, history=history, reason='geometric_bending_regression', totals=sums, line_search=line_search)
    movement = []
    for node in nodes.values():
        ci, end = next((ci, end) for ci, end in node['uses'] if ci in best)
        movement.append(float(np.linalg.norm(best[ci]['poles'][0 if end == 0 else -1]-node['position'])))
    return best, dict(accepted=True, history=history, totals=sums, curves=len(selected), line_search=line_search,
                     junctions=len(nodes), movable_junctions=sum(not n['fixed'] for n in nodes.values()),
                     moved_junctions=sum(d > 1e-7 for d in movement), junction_max_movement_mm=max(movement),
                     exact_shared_endpoints=True, positional_certificates=certificates)


def optimize(source, output, regions, config=None, feasible_seed_only=False):
    from .partition_fit import incidence as mesh_incidence
    config = config or Config(); root, out = Path(source), Path(output)
    out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if config.tube >= meta['config']['distance']:
        raise ValueError('Curve tube must leave room inside the unchanged shape budget')
    data = np.load(meta['partition']); v, f, labels = (data[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    assignment = np.load(root/'chart_index.npz')['face_chart']
    curves = {int(p.stem): dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
    charts = set(np.unique(assignment[np.isin(labels, regions)]).tolist())
    selected = {ci for ci, c in curves.items() if charts.intersection(map(int, c['owners']))}
    nodes = incidence(curves)
    active_nodes = {int(vi) for ci in selected for vi in curves[ci]['chain'][[0, -1]]}
    selected.update(ci for vi in active_nodes for ci, end in nodes[vi])
    edges, owners = mesh_incidence(f); keys = edges[:, 0]*len(v)+edges[:, 1]
    normal = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    normal /= np.linalg.norm(normal, axis=1)[:, None]
    sharp = {}; points = {}; locators = {}; locator_cache = {}
    for ci in selected:
        c = curves[ci]; chain = c['chain']; points[ci] = v[chain]
        edge_ids = np.searchsorted(keys, np.minimum(chain[:-1], chain[1:])*len(v)+np.maximum(chain[:-1], chain[1:]))
        angle = np.degrees(np.arccos(np.clip(np.sum(normal[owners[edge_ids, 0]]*normal[owners[edge_ids, 1]], axis=1), -1, 1)))
        length = np.linalg.norm(np.diff(points[ci], axis=0), axis=1)
        sharp[ci] = float(length@(angle > config.feature_angle)) > .25*length.sum()
        pair = tuple(sorted(map(int, c['owners'])))
        if pair not in locator_cache:
            ids = np.flatnonzero(np.isin(assignment, pair))
            used = np.unique(f[ids])
            loc = Locator(v[used], np.searchsorted(used, f[ids]))
            loc.source_normals = normal[ids]; locator_cache[pair] = loc
        locators[ci] = locator_cache[pair]
    screened = []
    if feasible_seed_only:
        eligible = set()
        for ci in selected:
            c = curves[ci]
            q = np.unique(np.r_[c['parameters'], c['knots'], np.linspace(0, 1, 257)])
            xyz = BSpline(c['knots'], c['poles'], 3)(q)
            target = np.column_stack([np.interp(q, c['parameters'], points[ci][:, k]) for k in range(3)])
            budget = min(config.movement, float(c.get('movement_budget_mm', config.movement)))
            distance = float(locators[ci].closest(xyz)[1].max())
            box = float(abs(xyz-target).max())
            feasible = bool(distance < config.tube*.7 and box <= budget/np.sqrt(3)+1e-8)
            screened.append(dict(curve=ci, eligible=feasible, source_max_mm=distance,
                                 coordinate_movement_mm=box, movement_budget_mm=budget))
            if feasible:
                eligible.add(ci)
        selected = eligible
        dump(out/'network_screening.json', screened)
    print(dict(stage='network_prepared', curves=len(selected), charts=len(charts)), flush=True)
    if not selected:
        raise ValueError('No feasible curve neighborhood remains')
    updated, report = solve_network(curves, selected, points, locators, sharp, config)
    report.update(source_fit=str(root.resolve()), config=asdict(config), regions=sorted(map(int, regions)),
                  seconds=time.perf_counter()-started, source_CAD_read=False, commercial_engine_used=False,
                  feasible_seed_only=feasible_seed_only, excluded_curves=[r['curve'] for r in screened if not r['eligible']])
    dump(out/'network_fairing.json', report)
    if updated is None:
        return report
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for ci, c in updated.items():
        # A rollback must restore a whole junction neighborhood, not one curve.
        c = {k: val for k, val in c.items() if not k.startswith('rollback_')}
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **c)
    affected = {int(di) for c in updated.values() for di in c['owners']}
    if meta.get('requires_support_refit'):
        affected.update(meta.get('required_refit_charts', []))
    affected = sorted(affected)
    meta.update(status='boundary_candidate', source_fit=str(out.resolve()), requires_support_refit=True,
                required_refit_charts=affected, required_refit_regions=np.unique(labels[np.isin(assignment, affected)]).tolist(),
                network_fairing=report)
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', required=True, type=int, nargs='+')
    p.add_argument('--fairness-length', type=float, default=.2); p.add_argument('--tube', type=float, default=.012)
    p.add_argument('--feasible-seed-only', action='store_true')
    a = p.parse_args(); optimize(a.run, a.output, a.regions, Config(fairness_length=a.fairness_length, tube=a.tube), a.feasible_seed_only)
