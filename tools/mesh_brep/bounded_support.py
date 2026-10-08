"""Minimum normal/curvature error with explicit positional fitting tubes."""
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from .splines import tensor


def solve(a, z, b, bz, normal_q, normals, knot, size, config, seam_limit, normal_limit=None,
          boundary_q=None, boundary_normals=None):
    import osqp
    count = len(knot)-4
    blocks = [a/np.sqrt(len(z))]; rhs = [z/np.sqrt(len(z))]
    good = normals[:, 2] > .15
    for axis in (range(2) if np.any(good) else ()):
        # Residual n_z * dh/dx + n_x measures tangent-plane disagreement.
        # It does not vanish simply because a chart happens to be narrow.
        weight = np.sqrt(config.normal_weight/max(1, good.sum()))
        op = tensor(normal_q[good], knot, knot, int(axis == 0), int(axis == 1))
        blocks.append(op.multiply(normals[good, 2, None])*weight/size[axis])
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
    spans = np.unique(knot); nodes, weights = np.polynomial.legendre.leggauss(2)
    grid = (spans[:-1, None]+np.diff(spans)[:, None]*(nodes+1)/2).ravel()
    w = (np.diff(spans)[:, None]*weights/2).ravel()
    fq = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    area_weights = np.sqrt(np.outer(w, w).ravel()*config.fairness)
    for du, dv in [(2, 0), (1, 1), (0, 2)]:
        scale = area_weights*np.sqrt(2 if du == dv else 1)/(size[0]**du*size[1]**dv)
        blocks.append(tensor(fq, knot, knot, du, dv).multiply(scale[:, None]))
        rhs.append(np.zeros(len(fq)))
    matrix = sparse.vstack(blocks, format='csr'); target = np.concatenate(rhs)
    h = (matrix.T@matrix).tocsc()
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
        l=lower, u=upper, verbose=False, eps_abs=1e-7, eps_rel=1e-7,
        max_iter=8000, time_limit=12., polishing=True, adaptive_rho_interval=50)
    model.warm_start(x=warm)
    result = model.solve(raise_error=False)
    solved = result.info.status_val in (1, 2)
    coeff = result.x if solved else warm
    return coeff, dict(status=result.info.status, solved=solved,
        iterations=result.info.iter, solver_seconds=result.info.run_time,
        sampled_data_error_mm=float(np.max(abs(a@coeff-z))),
        sampled_boundary_error_mm=float(np.max(abs(b@coeff-bz))))
