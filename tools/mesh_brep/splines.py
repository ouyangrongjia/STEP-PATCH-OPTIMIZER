"""Tensor splines and a joint XYZ fit with shared boundary coefficients."""
import time
import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline
from scipy.sparse.linalg import splu
from .mesh import sample, side_uv


def knots(n, degree=3):
    return np.r_[np.zeros(degree), np.linspace(0, 1, n-degree+1), np.ones(degree)]


def basis(t, x, derivative=0):
    return BSpline(t, np.eye(len(t)-4), 3, extrapolate=False)(np.clip(x, 0, 1), nu=derivative)


def tensor(uv, tu, tv, du=0, dv=0):
    bu, bv = sparse.coo_matrix(basis(tu, uv[:, 0], du)), basis(tv, uv[:, 1], dv)
    rows, cols, data = [], [], []
    for j in range(bv.shape[1]):
        value = bu.data*bv[bu.row, j]; mask = value != 0
        rows.append(bu.row[mask]); cols.append(bu.col[mask]*bv.shape[1]+j); data.append(value[mask])
    return sparse.csr_matrix((np.concatenate(data), (np.concatenate(rows), np.concatenate(cols))),
                             shape=(len(uv), (len(tu)-4)*(len(tv)-4)))


def evaluate(p, tu, tv, uv):
    p = p.reshape(-1, 3)
    points = tensor(uv, tu, tv)@p
    a, b = tensor(uv, tu, tv, 1, 0)@p, tensor(uv, tu, tv, 0, 1)@p
    cross = np.cross(a, b); norm = np.linalg.norm(cross, axis=1)
    return points, cross/np.maximum(norm[:, None], 1e-30), norm


def side_indices(n, side):
    grid = np.arange(n*n).reshape(n, n)
    return [grid[:, 0], grid[-1, :], grid[::-1, -1], grid[0, ::-1]][side]


def shared_maps(vertices, charts, n, fixed_vertex_ids=None):
    total = len(charts)*n*n; parents = np.arange(total)
    def root(i):
        while parents[i] != i:
            parents[i] = parents[parents[i]]; i = parents[i]
        return i
    def join(a, b):
        parents[root(a)] = root(b)
    uses = {}
    corner_uses = {}
    for fi, chart in enumerate(charts):
        for side, chain in enumerate(chart['chains']):
            indices = side_indices(n, side)+fi*n*n
            key = tuple(sorted([chain[0], chain[-1]]))
            canonical = list(chain) if chain[0] == key[0] else list(reversed(chain))
            oriented = indices if chain[0] == key[0] else indices[::-1]
            if key in uses:
                old = uses[key][0]
                if canonical != old['chain']:
                    raise ValueError('Incident patches have different discrete seam chains')
                for a, b in zip(oriented, old['indices']):
                    join(a, b)
            uses.setdefault(key, []).append(dict(face=fi, side=side, indices=oriented,
                                                  chain=canonical, reverse=chain[0] != key[0]))
            for vertex, index in [(chain[0], indices[0]), (chain[-1], indices[-1])]:
                if vertex in corner_uses:
                    join(index, corner_uses[vertex])
                corner_uses[vertex] = index
    roots = np.array([root(i) for i in range(total)])
    fixed = {root(index): vertices[v] for v, index in corner_uses.items()
             if fixed_vertex_ids is None or v in fixed_vertex_ids}
    free = [x for x in np.unique(roots) if x not in fixed]; lookup = {x: i for i, x in enumerate(free)}
    maps, offsets = [], []
    for fi in range(len(charts)):
        r = roots[fi*n*n:(fi+1)*n*n]; rows = []; cols = []; p0 = np.zeros((n*n, 3))
        for j, x in enumerate(r):
            if x in fixed:
                p0[j] = fixed[x]
            else:
                rows.append(j); cols.append(lookup[x])
        maps.append(sparse.csr_matrix((np.ones(len(rows)), (rows, cols)), shape=(n*n, len(free))))
        offsets.append(p0)
    return maps, offsets, uses, len(free)


def quadrature(tu, tv, order=4):
    """Integrate each polynomial span; order four covers squared cubic terms."""
    nodes, weights = np.polynomial.legendre.leggauss(order)
    def spans(t):
        t = np.unique(t); widths = np.diff(t)
        return ((t[:-1, None]+t[1:, None])/2+widths[:, None]*nodes/2).ravel(), (widths[:, None]*weights/2).ravel()
    u, wu = spans(tu); v, wv = spans(tv)
    return np.stack(np.meshgrid(u, v, indexing='ij'), axis=-1).reshape(-1, 2), np.outer(wu, wv).ravel()


def joint_fit(specs, maps, offsets, seam_samples, *, normal_weight=1e-4, fairness=1e-6, seam_weight=.01,
              quadrature_order=1, hard_tangents=(), hard_positions=()):
    """Shared by A ablations and B. specs contain explicit XYZ/normal/UV data.

    It has no access to source geometry; fixed coefficients are eliminated.
    XYZ data/fairness and mesh tangent-plane equations are solved together.
    """
    started = time.perf_counter(); unknowns = maps[0].shape[1]
    blocks, rhs = [], []
    def coordinates(op, mapping, initial, target, weight):
        q = (op@mapping)*weight
        blocks.append(sparse.block_diag([q, q, q], format='csr'))
        rhs.append(((target-op@initial)*weight).T.ravel())
    def tangent(op, mapping, initial, normal, weight):
        q = op@mapping
        blocks.append(sparse.hstack([q.multiply((normal[:, k]*weight)[:, None]) for k in range(3)], format='csr'))
        rhs.append(-np.einsum('ij,ij->i', op@initial, normal)*weight)
    for spec, mapping, initial in zip(specs, maps, offsets):
        uv, points, normals, tu, tv = (spec[x] for x in ['uv', 'points', 'normals', 'tu', 'tv'])
        count = len(points); lu, lv = spec['lengths']
        coordinates(tensor(uv, tu, tv), mapping, initial, points, np.sqrt(spec.get('data_weight', 1.)/count))
        for du, dv, length in [(1, 0, lu), (0, 1, lv)]:
            tangent(tensor(uv, tu, tv, du, dv), mapping, initial, normals, np.sqrt(normal_weight/count)/length)
        if 'boundary_uv' in spec:
            buv, bp = spec['boundary_uv'], spec['boundary_points']
            coordinates(tensor(buv, tu, tv), mapping, initial, bp, np.sqrt(spec.get('boundary_weight', 10.)/len(bp)))
        # Four Gauss points integrate the squared bicubic derivative terms.
        # Order one remains available to reproduce the original experiment.
        quad, areas = quadrature(tu, tv, quadrature_order)
        for du, dv, length in [(2, 0, lu*lu), (1, 1, lu*lv/np.sqrt(2)), (0, 2, lv*lv)]:
            op = tensor(quad, tu, tv, du, dv).multiply(np.sqrt(areas)[:, None]).tocsr()
            coordinates(op, mapping, initial, np.zeros((len(quad), 3)), np.sqrt(fairness)/length)
    for fi, uv, normals in seam_samples:
        spec, mapping, initial = specs[fi], maps[fi], offsets[fi]
        for du, dv, length in [(1, 0, spec['lengths'][0]), (0, 1, spec['lengths'][1])]:
            tangent(tensor(uv, spec['tu'], spec['tv'], du, dv), mapping, initial, normals,
                    np.sqrt(seam_weight/len(uv))/length)
    matrix = sparse.vstack(blocks, format='csc'); target = np.concatenate(rhs)
    scale = 1/np.maximum(np.sqrt(np.asarray(matrix.power(2).sum(axis=0)).ravel()), 1e-15)
    a = matrix@sparse.diags(scale)
    h = (a.T@a).tocsc()+sparse.eye(unknowns*3, format='csc')*1e-12
    g = a.T@target
    constraints, values = [], []
    for fi, uv, normal, direction in hard_tangents:
        spec, mapping, initial = specs[fi], maps[fi], offsets[fi]
        op = (tensor(uv, spec['tu'], spec['tv'], 1, 0)*direction[0]+
              tensor(uv, spec['tu'], spec['tv'], 0, 1)*direction[1])
        q = op@mapping
        constraints.append(sparse.hstack([q.multiply(normal[:, k, None]) for k in range(3)], format='csr'))
        values.append(-np.einsum('ij,ij->i', op@initial, normal))
    for fi, uv, points in hard_positions:
        spec, mapping, initial = specs[fi], maps[fi], offsets[fi]
        op = tensor(uv, spec['tu'], spec['tv'])
        q = op@mapping
        constraints.append(sparse.block_diag([q, q, q], format='csr'))
        values.append((points-op@initial).T.ravel())
    system, system_rhs = h, g
    if constraints:
        constraint = sparse.vstack(constraints, format='csc'); value = np.concatenate(values)
        scaled_constraint = constraint@sparse.diags(scale)
        row_scale = 1/np.maximum(np.sqrt(np.asarray(scaled_constraint.power(2).sum(axis=1)).ravel()), 1e-15)
        c = sparse.diags(row_scale)@scaled_constraint
        # Shared derivatives can give the same corner-plane equation twice.
        # Select independent rows and verify all equations after the solve.
        from scipy.linalg import qr
        _, r, pivot = qr(c.toarray().T, pivoting=True, mode='economic')
        rank = int(np.sum(abs(np.diag(r)) > 1e-10))
        selected = pivot[:rank]
        c = c[selected]
        system = sparse.bmat([[h, c.T], [c, None]], format='csc')
        system_rhs = np.r_[g, (value*row_scale)[selected]]
    assembled = time.perf_counter()
    factor = splu(system); solution = factor.solve(system_rhs); x = solution[:unknowns*3]
    solved = time.perf_counter()
    coeff = (x*scale).reshape(3, unknowns).T
    hard_residual = float(np.max(abs(constraint@(scale*x)-value))) if constraints else None
    if hard_residual is not None and hard_residual > 1e-8:
        raise ValueError(f'Inconsistent or unresolved hard constraints: {hard_residual}')
    poles = [(m@coeff+p0).reshape(len(s['tu'])-4, len(s['tv'])-4, 3) for s, m, p0 in zip(specs, maps, offsets)]
    report = dict(independent_scalar_variables=unknowns*3, matrix_rows=matrix.shape[0], matrix_nnz=matrix.nnz,
                  normal_weight=normal_weight, fairness=fairness, seam_weight=seam_weight, quadrature_order=quadrature_order,
                  relative_linear_residual=float(np.linalg.norm(system@solution-system_rhs)/max(np.linalg.norm(system_rhs), 1e-30)),
                  hard_tangent_constraints=sum(len(v) for v in values),
                  independent_hard_constraints=rank if constraints else 0,
                  hard_tangent_max_residual=hard_residual,
                  weighted_residual_norm=float(np.linalg.norm(matrix@(scale*x)-target)),
                  matrix_assembly_seconds=assembled-started, solve_seconds=solved-assembled)
    return poles, report


def fit_mesh(vertices, charts, n, count, seed):
    started = time.perf_counter(); maps, offsets, uses, free = shared_maps(vertices, charts, n)
    specs = []; t = knots(n)
    areas = np.array([c['audit']['area_mm2'] for c in charts]); counts = np.maximum(1000, (count*areas/areas.sum()).astype(int))
    for i, c in enumerate(charts):
        points, normals, uv, _, _ = sample(vertices, c['triangles'], int(counts[i]), seed+i,
                                          c['uv'], c['normals'])
        bp, buv, lengths = [], [], []
        q = np.linspace(0, 1, 385)
        for side, (chain, ct) in enumerate(zip(c['chains'], c['chain_t'])):
            bp.append(np.stack([np.interp(q, ct, vertices[chain, k]) for k in range(3)], axis=1))
            buv.append(side_uv(side, q))
            lengths.append(float(np.linalg.norm(np.diff(vertices[chain], axis=0), axis=1).sum()))
        specs.append(dict(uv=uv, points=points, normals=normals, tu=t, tv=t,
                          lengths=[(lengths[0]+lengths[2])/2, (lengths[1]+lengths[3])/2],
                          boundary_points=np.concatenate(bp), boundary_uv=np.concatenate(buv)))
    seams = []
    for entries in uses.values():
        if len(entries) != 2:
            continue
        samples = np.linspace(.002, .998, 193); normals = []
        for e in entries:
            c = charts[e['face']]; side = e['side']; chain = c['chains'][side]
            q = 1-samples if e['reverse'] else samples
            normals.append(np.stack([np.interp(q, c['chain_t'][side], c['normals'][chain, k]) for k in range(3)], axis=1))
        nn = normals[0]+normals[1]; nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
        for e in entries:
            seams.append((e['face'], side_uv(e['side'], 1-samples if e['reverse'] else samples), nn))
    correspondence_seconds = time.perf_counter()-started
    poles, report = joint_fit(specs, maps, offsets, seams)
    report['correspondence_and_shared_map_seconds'] = correspondence_seconds
    report['training_samples'] = int(counts.sum())
    return poles, [(t, t)]*len(poles), uses, report
