"""Minimum normal/curvature error with explicit positional fitting tubes."""
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from .splines import tensor, sparse_basis


FAIRNESS_VERSION = 2


def correct(coeff, a, z, b, bz, distance, boundary, seconds=1.5, data_limits=None):
    """One fixed-basis projection; the caller still checks geometric extrema.

    Minimize displacement of the existing solution, with a small coefficient
    regularizer. No new knots, faces, solver restart or tolerance increase.
    """
    import osqp
    constraint = sparse.vstack([a, b], format='csc')
    data_limits = np.full(len(z), distance) if data_limits is None else np.asarray(data_limits)
    target = np.r_[z, bz]; limits = np.r_[data_limits*.98,
                                         np.full(len(bz), boundary*.65)]
    residual = target-constraint@coeff
    h = (a.T@a/max(1, len(z))+sparse.eye(len(coeff), format='csc')*1e-3).tocsc()
    solver = osqp.OSQP()
    solver.setup(P=sparse.triu(h, format='csc'), q=np.zeros(len(coeff)), A=constraint,
        l=residual-limits, u=residual+limits, verbose=False, eps_abs=1e-7, eps_rel=1e-7,
        max_iter=3000, time_limit=seconds, polishing=True, adaptive_rho_interval=50)
    result = solver.solve(raise_error=False)
    info = dict(status=result.info.status, solver_seconds=result.info.run_time,
                iterations=result.info.iter, unchanged_basis=True, attempts=1)
    if result.x is None or not np.isfinite(result.x).all():
        return None, dict(info, proposed=False)
    candidate = coeff+result.x
    error = abs(constraint@candidate-target)
    # These are physical limits, not the solver's reserved target margins.
    physical = np.r_[data_limits, np.full(len(bz), boundary)]
    passed = bool(np.all(error < physical))
    return (candidate if passed else None), dict(info, proposed=passed,
        sampled_data_max_mm=float(error[:len(z)].max(initial=0.)),
        sampled_boundary_max_mm=float(error[len(z):].max(initial=0.)))


def bending_matrix(knot, size, order=2):
    """Exact average thin-plate energy for a cubic tensor height surface.

    A squared second derivative is still degree six in the other direction.
    Two-point tensor quadrature misses non-affine modes. Integrating the three
    one-dimensional Gram matrices separately is exact and avoids a dense 2-D
    quadrature grid when the adaptive basis has many knots.
    """
    if order not in (2, 3):
        raise ValueError('Fairness derivative order must be two or three')
    spans = np.unique(knot); nodes, weights = np.polynomial.legendre.leggauss(4)
    q = (spans[:-1, None]+np.diff(spans)[:, None]*(nodes+1)/2).ravel()
    w = (np.diff(spans)[:, None]*weights/2).ravel()
    gram = []
    for derivative in range(order+1):
        b = sparse_basis(knot, q, derivative)
        gram.append((b.T@b.multiply(w[:, None])).tocsr())
    if order == 3:
        return (sparse.kron(gram[3], gram[0])/size[0]**6
                +3*sparse.kron(gram[2], gram[1])/size[0]**4/size[1]**2
                +3*sparse.kron(gram[1], gram[2])/size[0]**2/size[1]**4
                +sparse.kron(gram[0], gram[3])/size[1]**6).tocsc()
    return (sparse.kron(gram[2], gram[0])/size[0]**4
            +2*sparse.kron(gram[1], gram[1])/(size[0]*size[1])**2
            +sparse.kron(gram[0], gram[2])/size[1]**4).tocsc()


def penalty_proposal(h, linear, constraint, lower, upper, weights, initial, seconds, iterations=6):
    """Short damped Newton proposal for squared distance to constraint boxes.

    This is a finite-penalty solve, not a certificate for the constrained QP.
    Feasibility and continuous geometric checks remain the caller's job.
    """
    import time
    start = time.perf_counter(); c = constraint.tocsr(); x = initial.copy(); history = []
    def residual(x):
        value = c@x
        return value-np.clip(value, lower, upper)
    def energy(x, r):
        return float(.5*x@(h@x)-linear@x+.5*np.dot(weights*r, r))
    r = residual(x); value = energy(x, r)
    for _ in range(iterations):
        if time.perf_counter()-start >= seconds: break
        active = r != 0
        gradient = h@x-linear+c.T@(weights*r)
        op = c[active]
        matrix = h+op.T@op.multiply(weights[active, None])
        direction = spsolve(matrix.tocsc(), -gradient)
        slope = float(gradient@direction)
        if not np.isfinite(direction).all() or slope >= 0: break
        fraction = 1.
        for _ in range(9):
            candidate = x+fraction*direction; next_r = residual(candidate); next_value = energy(candidate, next_r)
            if next_value <= value+1e-4*fraction*slope: break
            fraction *= .5
        else: break
        x, r, value = candidate, next_r, next_value
        history.append(dict(energy=value, step=fraction, maximum_box_residual=float(abs(r).max(initial=0.))))
        if np.linalg.norm(fraction*direction) <= 1e-10*max(1., np.linalg.norm(x)): break
    return x, dict(iterations=len(history), seconds=time.perf_counter()-start, history=history,
                   finite_penalty_only=True)


def solve(a, z, b, bz, normal_q, normals, knot, size, config, seam_limit, normal_limit=None,
          boundary_q=None, boundary_normals=None, bending=None, position_weights=None, normal_weights=None,
          solver_seconds=12., equilibrated_retry=True, retain_inexact=False, quick_penalty=False):
    import osqp
    count = len(knot)-4
    position_weights = np.ones(len(z)) if position_weights is None else np.asarray(position_weights)
    weight = np.sqrt(position_weights/position_weights.sum())
    blocks = [a.multiply(weight[:, None])]; rhs = [z*weight]
    good = normals[:, 2] > .15
    normal_weights = np.ones(len(normals)) if normal_weights is None else np.asarray(normal_weights)
    for axis in (range(2) if np.any(good) else ()):
        # Residual n_z * dh/dx + n_x measures tangent-plane disagreement.
        # It does not vanish simply because a chart happens to be narrow.
        weight = np.sqrt(config.normal_weight*normal_weights[good]/normal_weights[good].sum())
        op = tensor(normal_q[good], knot, knot, int(axis == 0), int(axis == 1))
        blocks.append(op.multiply((normals[good, 2]*weight)[:, None])/size[axis])
        rhs.append(-normals[good, axis]*weight)
    if config.boundary_normal_weight > 0:
        if boundary_q is None or boundary_normals is None:
            raise ValueError('Missing common-boundary tangent-plane targets')
        usable = boundary_normals[:, 2] > .05
        for axis in (range(2) if np.any(usable) else ()):
            weight = np.sqrt(config.boundary_normal_weight/max(1, usable.sum()))
            op = tensor(boundary_q[usable], knot, knot, int(axis == 0), int(axis == 1))
            blocks.append(op.multiply(boundary_normals[usable, 2, None])*weight/size[axis])
            rhs.append(-boundary_normals[usable, axis]*weight)
    matrix = sparse.vstack(blocks, format='csr'); target = np.concatenate(rhs)
    if bending is None:
        bending = bending_matrix(knot, size, config.fairness_order)
    h = (matrix.T@matrix+config.fairness*bending).tocsc()
    regularization = max(float(h.diagonal().max()), 1.)*1e-14
    h += sparse.eye(count*count, format='csc')*regularization
    linear = np.asarray(matrix.T@target).ravel()
    unconstrained = spsolve(h, linear)
    de = float(np.max(abs(a@unconstrained-z))); be = float(np.max(abs(b@unconstrained-bz)))
    if normal_limit is None and np.isfinite(unconstrained).all() and de <= config.distance*.98 and be <= seam_limit*.65:
        return unconstrained, dict(status='unconstrained optimum feasible', solved=True,
            iterations=0, solver_seconds=0., sampled_data_error_mm=de, sampled_boundary_error_mm=be)
    proposal_info = None
    if quick_penalty and normal_limit is None and np.isfinite(unconstrained).all():
        constraint = sparse.vstack([a, b], format='csr')
        lower = np.r_[z-config.distance*.98, bz-seam_limit*.65]
        upper = np.r_[z+config.distance*.98, bz+seam_limit*.65]
        weights = np.r_[np.full(len(z), 1/(len(z)*config.distance**2)),
                        np.full(len(bz), 1/(len(bz)*seam_limit**2))]
        proposed, proposal_info = penalty_proposal(h, linear, constraint, lower, upper, weights,
                                                   unconstrained, solver_seconds*.25)
        de = float(np.max(abs(a@proposed-z))); be = float(np.max(abs(b@proposed-bz)))
        if np.isfinite(proposed).all() and de <= config.distance*.99 and be <= seam_limit*.7:
            return proposed, dict(status='finite penalty proposal pending geometric check', solved=False,
                proposal_source='short_penalty_newton', iterations=proposal_info['iterations'],
                solver_seconds=proposal_info['seconds'], sampled_data_error_mm=de,
                sampled_boundary_error_mm=be, penalty=proposal_info)
    # A warm start and a useful residual for knot refinement when infeasible.
    warm_h = h+b.T@b*(3000/max(1, len(bz)))
    warm = spsolve(warm_h.tocsc(), linear+b.T@bz*(3000/max(1, len(bz))))
    constraint = sparse.vstack([a, b], format='csc')
    lower = np.r_[z-config.distance*.98, bz-seam_limit*.65]
    upper = np.r_[z+config.distance*.98, bz+seam_limit*.65]
    if normal_limit is not None:
        from .graph_fair_optimize import normal_cone_constraints
        du = tensor(normal_q[good], knot, knot, 1, 0)/size[0]
        dv = tensor(normal_q[good], knot, knot, 0, 1)/size[1]
        cone, cone_lower, cone_upper = normal_cone_constraints(du, dv, normals[good], normal_limit)
        constraint = sparse.vstack([constraint, cone], format='csc')
        lower = np.r_[lower, cone_lower]; upper = np.r_[upper, cone_upper]
    model = osqp.OSQP()
    model.setup(P=sparse.triu(h, format='csc'), q=-linear, A=constraint,
        l=lower, u=upper, verbose=False, eps_abs=1e-8, eps_rel=1e-8,
        max_iter=8000, time_limit=solver_seconds, polishing=True, adaptive_rho_interval=50)
    model.warm_start(x=warm)
    result = model.solve(raise_error=False)
    original_status = result.info.status
    original_seconds = result.info.run_time
    original_iterations = result.info.iter
    balanced = False
    candidate = result.x
    def feasible(x):
        if x is None or not np.isfinite(x).all():
            return False
        values = constraint@x
        return bool(np.all(values >= lower-5e-8) and np.all(values <= upper+5e-8))
    # Small knot spans give very different coefficient energy scales. Retry
    # the identical QP in equilibrated variables, retaining all physical bounds.
    # Inexact solver iterates are proposals, accepted only after checking the
    # actual constraints; fit_support still checks continuous boundaries and
    # interior source probes independently.
    if equilibrated_retry and not feasible(candidate):
        scale = 1/np.sqrt(np.maximum(h.diagonal(), 1e-30))
        ph = h.multiply(scale[:, None]).multiply(scale[None, :]).tocsc()
        ac = constraint.multiply(scale[None, :]).tocsc()
        retry = osqp.OSQP()
        retry.setup(P=sparse.triu(ph, format='csc'), q=-linear*scale, A=ac,
            l=lower, u=upper, verbose=False, eps_abs=1e-8, eps_rel=1e-8,
            max_iter=16000, time_limit=solver_seconds, polishing=True, adaptive_rho_interval=50)
        retry.warm_start(x=warm/scale)
        result = retry.solve(raise_error=False)
        candidate = None if result.x is None else result.x*scale
        balanced = True
    feasible_iterate = feasible(candidate)
    # The boundary-penalty solve is itself a geometric proposal. A stalled QP
    # must not discard it when it satisfies every actual linear constraint.
    # It is not reported as a converged constrained optimum; continuous trim
    # and source-interior checks in fit_support remain mandatory.
    feasible_warm = not feasible_iterate and feasible(warm)
    solved = bool(feasible_iterate or feasible_warm)
    coeff = candidate if feasible_iterate else warm
    proposal_source = 'qp' if feasible_iterate else 'boundary_penalty_warm_start'
    if retain_inexact and not solved and candidate is not None and np.isfinite(candidate).all():
        def score(x):
            return max(float(np.max(abs(a@x-z)))/config.distance,
                       float(np.max(abs(b@x-bz)))/seam_limit)
        if score(candidate) < score(warm):
            coeff = candidate; proposal_source = 'inexact_qp_pending_geometric_check'
    values = constraint@coeff
    violation = float(max(0., np.max(lower-values), np.max(values-upper)))
    return coeff, dict(status=result.info.status, solved=solved, penalty=proposal_info,
        iterations=result.info.iter+(original_iterations if balanced else 0),
        solver_seconds=result.info.run_time+(original_seconds if balanced else 0)
                       +(proposal_info['seconds'] if proposal_info is not None else 0),
        initial_status=original_status, equilibrated_retry=balanced,
        proposal_source=proposal_source,
        linear_constraint_violation=violation,
        feasible_inexact_iterate=bool(feasible_iterate and result.info.status_val not in (1, 2)),
        sampled_data_error_mm=float(np.max(abs(a@coeff-z))),
        sampled_boundary_error_mm=float(np.max(abs(b@coeff-bz))))
