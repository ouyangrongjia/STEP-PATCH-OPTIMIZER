"""Repair distance by a small change of the existing support and its jets.

The basis and frame stay fixed. Dense probes inside the original source
projection receive height bounds; the independent actual-distance evaluator
still decides whether the proposal is usable outside that projection.
"""
import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline
from .bounded_support import bending_matrix
from .mesh import Locator
from .partition_fit import parameters, positions
from .splines import tensor
from .support_probe import probes
from .surface_jets import jets, support_span_parameters


def triangle_distances(points, triangles):
    """Distance to one specified finite triangle per point."""
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    ab, ac, ap = b-a, c-a, points-a
    dot = lambda x, y: np.einsum('ij,ij->i', x, y)
    aa, bb, cc = dot(ab, ab), dot(ab, ac), dot(ac, ac)
    normal = np.cross(ab, ac); determinant = dot(normal, normal)
    u = (cc*dot(ap, ab)-bb*dot(ap, ac))/determinant
    v = (aa*dot(ap, ac)-bb*dot(ap, ab))/determinant
    plane = abs(dot(ap, normal))/np.sqrt(determinant)
    edge = []
    for start, end in ((a, b), (b, c), (c, a)):
        d = end-start; q = np.clip(dot(points-start, d)/dot(d, d), 0., 1.)
        edge.append(np.linalg.norm(points-start-q[:, None]*d, axis=1))
    return np.where((u > 1e-12)&(v > 1e-12)&(u+v < 1-1e-12), plane, np.min(edge, axis=0))


def damp(record, proposal, points, vertices, triangles, distance, before_boundary, after_boundary,
         boundary_limit, locator=None):
    """Choose one admissible coefficient step analytically, without a new QP.

    Distance to a fixed finite triangle is convex. Its endpoint distances
    bound every intermediate point on this fixed-basis coefficient segment.
    The continuous boundary residual has the same convex bound. Only the
    supplied probes are certified; the caller still rechecks the geometry.
    """
    if any(not np.array_equal(record[k], proposal[k]) for k in ('origin', 'axes', 'lo', 'size', 'knots')):
        raise ValueError('A coefficient step requires the same frame and basis')
    locator = locator or Locator(vertices, triangles)
    _, old_distance, ids, _ = locator.closest(positions(points, record))
    new_distance = triangle_distances(positions(points, proposal), vertices[triangles[ids]])
    if not np.isfinite(old_distance).all() or not np.isfinite(new_distance).all():
        return None, dict(proposed=False, reason='nonfinite_step_bound', solver_calls=0)
    old = np.r_[old_distance, before_boundary]; new = np.r_[new_distance, after_boundary]
    limits = np.r_[np.full(len(points), distance), boundary_limit]; slope = new-old
    increasing, decreasing = slope > 1e-14, slope < -1e-14
    low = max(0., float(np.max((limits[decreasing]-old[decreasing])/slope[decreasing], initial=0.)))
    high = min(1., float(np.min((limits[increasing]-old[increasing])/slope[increasing], initial=1.)))
    info = dict(method='analytic_coefficient_step', solver_calls=0, fraction_lower=low, fraction_upper=high,
                finite_triangle_probe_bound=True, unchanged_basis=True)
    if np.any(old[~(increasing|decreasing)] >= limits[~(increasing|decreasing)]) or high <= low+1e-10:
        return None, dict(info, proposed=False)
    fraction = (low+high)/2
    bound = (1-fraction)*old+fraction*new
    return dict(record, height=(1-fraction)*record['height']+fraction*proposal['height']), dict(info,
        proposed=True, fraction=fraction, source_probe_bound_mm=float(bound[:-1].max(initial=0.)),
        continuous_boundary_bound_mm=float(bound[-1]))


def refine(record, maximum_controls):
    """Insert simple knots without changing the represented support geometry."""
    t = record['knots']; count = len(t)-4
    if maximum_controls <= count:
        return None
    inserted = []; working = np.unique(t)
    while count+len(inserted) < maximum_controls:
        number = min(maximum_controls-count-len(inserted), len(working)-1)
        spans = np.argsort(np.diff(working), kind='stable')[-number:]
        additions = (working[spans]+working[spans+1])/2
        inserted.extend(additions); working = np.sort(np.r_[working, additions])
    u = BSpline(t, record['height'].reshape(count, count), 3, axis=0)
    for knot in sorted(inserted):
        u = u.insert_knot(float(knot))
    v = BSpline(t, u.c.T, 3, axis=0)
    for knot in sorted(inserted):
        v = v.insert_knot(float(knot))
    return dict(record, knots=v.t.copy(), height=v.c.T.copy().ravel())


def project(record, vertices, triangles, curves, dense_xyz, config, fraction=.9, *, seconds=60., short=False,
            source_locator=None, source_normals=None):
    import osqp
    t = record['knots']; current = record['height']; size = record['size']
    uv = parameters(vertices, record)
    source_z = (vertices-record['origin'])@record['axes'][2]
    interior_q, interior_z = probes(record, vertices, triangles)
    dense_q = parameters(dense_xyz, record)
    locator = Locator(np.c_[uv, np.zeros(len(uv))], triangles)
    _, distance, ids, bary = locator.closest(np.c_[dense_q, np.zeros(len(dense_q))])
    inside = distance < 1e-10
    dense_z = np.einsum('ij,ij->i', bary[inside], source_z[triangles[ids[inside]]])
    source_q = np.r_[uv, uv[triangles].mean(axis=1), interior_q]
    source_target = np.r_[source_z, source_z[triangles].mean(axis=1), interior_z]
    a = tensor(source_q, t, t); dense = tensor(dense_q[inside], t, t)
    source_limit = np.full(len(source_q), config.distance)
    dense_limit = np.full(int(inside.sum()), config.distance)
    if short:
        if source_locator is None:
            source_locator = Locator(vertices, triangles)
            source_normals = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                                      vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
            source_normals /= np.linalg.norm(source_normals, axis=1)[:, None]
        all_q = np.r_[source_q, dense_q]; xyz = positions(all_q, record)
        closest, _, ids, _ = source_locator.closest(xyz)
        normal = source_normals[ids]; cosine = normal@record['axes'][2]
        cosine = np.where(cosine >= 0, 1., -1.)*np.maximum(abs(cosine), 1e-6)
        signed = np.einsum('ij,ij->i', xyz-closest, normal)
        target = (xyz-record['origin'])@record['axes'][2]-signed/cosine
        limits = config.distance/abs(cosine)
        source_target, dense_z = target[:len(source_q)], target[len(source_q):]
        source_limit, dense_limit = limits[:len(source_q)], limits[len(source_q):]
        dense = tensor(dense_q, t, t)
    constraints = [a, dense]
    lower = [source_target-source_limit*.98-a@current, dense_z-dense_limit*fraction-dense@current]
    upper = [source_target+source_limit*.98-a@current, dense_z+dense_limit*fraction-dense@current]
    slack = [source_limit*.02, dense_limit*(1-fraction)]
    # Minimize the change, including normals and curvature on every incident
    # boundary. Each interface has its own normalized weight so short seams
    # cannot disappear inside the whole-model arc-length objective.
    h = (a.T@a/max(1, len(source_q))
         +config.fairness*bending_matrix(t, size)).tocsc()
    for curve in curves:
        q = support_span_parameters(curve, [record])
        xyz = BSpline(curve['knots'], curve['poles'], 3)(q)
        bq = parameters(xyz, record); b = tensor(bq, t, t)
        bz = (xyz-record['origin'])@record['axes'][2]
        constraints.append(b)
        lower.append(bz-config.boundary*.4*.65-b@current)
        upper.append(bz+config.boundary*.4*.65-b@current)
        slack.append(np.full(len(bz), config.boundary*.4*.35))
        if short:
            continue
        _, _, nj, sj = jets(record, xyz, derivatives=True)
        for j in nj:
            h += 2.*(j.T@j)/len(q)
        for j, weight in zip(sj, (1., 1., 1., 2., 2., 2.)):
            h += .02*weight*(j.T@j)/len(q)
    h = h.tocsc()+sparse.eye(len(current), format='csc')*1e-12
    constraint = sparse.vstack(constraints, format='csr')
    lo, hi = np.concatenate(lower), np.concatenate(upper)
    scale = 1/np.sqrt(np.maximum(h.diagonal(), 1e-30))
    ph = h.multiply(scale[:, None]).multiply(scale[None, :]).tocsc()
    ac = constraint.multiply(scale[None, :]).tocsr()
    row_scale = 1/np.maximum(np.asarray(abs(ac).max(axis=1).toarray()).ravel(), 1e-30)
    ac = ac.multiply(row_scale[:, None]).tocsc()
    solver = osqp.OSQP()
    solver.setup(P=sparse.triu(ph, format='csc'), q=np.zeros(len(current)), A=ac,
                 l=lo*row_scale, u=hi*row_scale, verbose=False,
                 eps_abs=1e-8, eps_rel=1e-8, eps_prim_inf=1e-10, eps_dual_inf=1e-10,
                 max_iter=3000 if short else 30000, time_limit=seconds, polishing=True, adaptive_rho_interval=50)
    result = solver.solve(raise_error=False)
    info = dict(status=result.info.status, solver_seconds=result.info.run_time,
                iterations=result.info.iter, dense_inside=int(inside.sum()),
                dense_outside=int((~inside).sum()), unchanged_frame_and_basis=True,
                short_correction=short, solver_limit_seconds=seconds)
    if result.x is None or not np.isfinite(result.x).all():
        return None, dict(info, accepted=False)
    delta = result.x*scale; values = constraint@delta
    violation = float(max(0., np.max(lo-values), np.max(values-hi)))
    # The QP has reserved position and boundary margins. A finite timed-out
    # iterate may be proposed within a small part of those reserves, but it
    # must still pass the unchanged physical geometry and continuity gates.
    proposal_tolerance = min(config.distance*.02, config.boundary*.4*.35)*.1
    physical_slack = np.concatenate(slack)
    physical_violation = float(max(0., np.max(lo-physical_slack-values), np.max(values-hi-physical_slack)))
    info.update(linear_constraint_violation_mm=violation, linear_feasible=violation <= 5e-8,
                accepted=physical_violation <= 5e-8 if short else violation <= proposal_tolerance,
                physical_linear_violation_mm=physical_violation, proposal_tolerance_mm=proposal_tolerance,
                requires_geometric_validation=True,
                feasible_inexact_iterate=result.info.status_val not in (1, 2))
    return (dict(record, height=current+delta) if info['accepted'] else None), info


def continuity_change(group, candidate, reference, active, curves, lookup):
    """Bound new local regressions against the pre-repair surface pair."""
    from .surface_jets import seam_sampling
    from .regional_quality import weighted_stats
    failures = []; worst_angle_increase = 0.; worst_shape_increase = 0.
    for curve in curves:
        owners = list(map(int, curve['owners']))
        old = [reference[i] for i in owners]
        new = [candidate if i in group else active[i] for i in owners]
        _, xyz, weight, smooth = seam_sampling(curve, lookup, 129, records=old+new)
        if not smooth.any():
            continue
        measures = []
        for pair in (old, new):
            n0, s0 = jets(pair[0], xyz); n1, s1 = jets(pair[1], xyz)
            angle = np.degrees(np.arccos(np.clip(np.sum(n0*n1, axis=1), -1., 1.)))
            shape = np.linalg.norm(s0-s1, axis=(1, 2))
            measures.append([weighted_stats(value[smooth], weight[smooth]) for value in (angle, shape)])
        (an, ak), (bn, bk) = measures
        worst_angle_increase = max(worst_angle_increase, bn['p95']-an['p95'])
        worst_shape_increase = max(worst_shape_increase, bk['p95']-ak['p95'])
        if (bn['p95'] > an['p95']+2. or bn['max'] > an['max']+2.
                or bk['p95'] > ak['p95']+max(.5, .25*ak['p95'])
                or bk['max'] > ak['max']+max(1., .25*ak['max'])):
            failures.append(dict(charts=owners, before_normal=an, after_normal=bn,
                                 before_shape=ak, after_shape=bk))
    return dict(passed=not failures, failures=failures,
                worst_normal_p95_increase_degrees=worst_angle_increase,
                worst_shape_p95_increase_per_mm=worst_shape_increase)
