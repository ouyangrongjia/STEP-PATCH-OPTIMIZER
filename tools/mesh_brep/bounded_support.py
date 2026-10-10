"""Minimum normal/curvature error with explicit positional fitting tubes."""
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from .splines import tensor, sparse_basis


FAIRNESS_VERSION = 2


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


def solve(a, z, b, bz, normal_q, normals, knot, size, config, seam_limit, normal_limit=None,
          boundary_q=None, boundary_normals=None, bending=None, position_weights=None, normal_weights=None):
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
        max_iter=8000, time_limit=12., polishing=True, adaptive_rho_interval=50)
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
    if not feasible(candidate):
        scale = 1/np.sqrt(np.maximum(h.diagonal(), 1e-30))
        ph = h.multiply(scale[:, None]).multiply(scale[None, :]).tocsc()
        ac = constraint.multiply(scale[None, :]).tocsc()
        retry = osqp.OSQP()
        retry.setup(P=sparse.triu(ph, format='csc'), q=-linear*scale, A=ac,
            l=lower, u=upper, verbose=False, eps_abs=1e-8, eps_rel=1e-8,
            max_iter=16000, time_limit=12., polishing=True, adaptive_rho_interval=50)
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
    values = constraint@coeff
    violation = float(max(0., np.max(lower-values), np.max(values-upper)))
    return coeff, dict(status=result.info.status, solved=solved,
        iterations=result.info.iter+(original_iterations if balanced else 0),
        solver_seconds=result.info.run_time+(original_seconds if balanced else 0),
        initial_status=original_status, equilibrated_retry=balanced,
        proposal_source='qp' if feasible_iterate else 'boundary_penalty_warm_start',
        linear_constraint_violation=violation,
        feasible_inexact_iterate=bool(feasible_iterate and result.info.status_val not in (1, 2)),
        sampled_data_error_mm=float(np.max(abs(a@coeff-z))),
        sampled_boundary_error_mm=float(np.max(abs(b@coeff-bz))))
