"""Couple native regional height supports through movable shared curves.

Only the interfaces are coupled. Regional interior fits supply the initial
supports. One displacement variable is shared at each mesh boundary vertex;
Bezier edge controls move along an interpolated source normal. Each incident
support constrains the same curve through its own affine graph coordinates.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve, lsmr
from .mesh import dump, vertex_normals, stats
from .partition_fit import incidence, parameters
from .splines import tensor
from .regional_reconstruct import bernstein


def extend_support(rec, distance):
    """Exactly extend end polynomial spans without changing the fitted surface."""
    from scipy.interpolate import BSpline
    old = rec['knots']; n = len(old)-4
    margin = 1.1*distance/np.min(rec['size'])
    extended = old.copy(); extended[:4] = -margin; extended[-4:] = 1+margin
    new = (extended+margin)/(1+2*margin)
    # Canonical endpoints prevent a rounded value just below 1 from making
    # BSpline.design_matrix reject the exact upper boundary of the domain.
    new[:4] = 0.; new[-4:] = 1.
    g = np.array([np.mean(new[i+1:i+4]) for i in range(n)])
    target = BSpline(old, np.eye(n), 3, extrapolate=True)(g*(1+2*margin)-margin)
    transform = np.linalg.solve(BSpline(new, np.eye(n), 3)(g), target)
    rec['height'] = (transform@rec['height'].reshape(n, n)@transform.T).ravel()
    rec['lo'] = rec['lo']-margin*rec['size']
    rec['size'] = rec['size']*(1+2*margin); rec['knots'] = new


def prepare(root, degree, movement_limit=0.):
    metadata = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(metadata['partition']); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    assignment = np.load(root/'chart_index.npz')['face_chart']
    edges, owners = incidence(f); pair = np.sort(assignment[owners], axis=1)
    selected = pair[:, 0] != pair[:, 1]; edges, pair = edges[selected], pair[selected]
    vertices = np.unique(edges); normal = vertex_normals(v, f)
    maps = np.empty((len(edges), degree+1), dtype=int)
    maps[:, 0] = np.searchsorted(vertices, edges[:, 0]); maps[:, -1] = np.searchsorted(vertices, edges[:, 1])
    maps[:, 1:-1] = len(vertices)+np.arange(len(edges)*(degree-1)).reshape(len(edges), degree-1)
    t = np.linspace(0, 1, degree+1)
    base = (1-t[None, :, None])*v[edges[:, 0], None]+t[None, :, None]*v[edges[:, 1], None]
    directions = (1-t[None, :, None])*normal[edges[:, 0], None]+t[None, :, None]*normal[edges[:, 1], None]
    directions /= np.maximum(np.linalg.norm(directions, axis=2)[..., None], 1e-30)
    records = [dict(np.load(path)) for path in sorted((root/'charts').glob('*.npz'))]
    if movement_limit:
        for rec in records:
            extend_support(rec, movement_limit)
    offsets = np.r_[0, np.cumsum([len(r['height']) for r in records])]
    original = np.concatenate([r['height'] for r in records])
    return metadata, records, offsets, original, edges, pair, vertices, maps, base, directions


def curve_constraints(records, offsets, curves, pair, maps, directions, t):
    degree = curves.shape[1]-1; b = bernstein(degree, t)
    points = np.einsum('tc,ecd->etd', b, curves)
    rows, cols, values, residuals = [], [], [], []; row_offset = 0
    total_heights = int(offsets[-1]); variables = total_heights+int(maps.max())+1
    for fi, rec in enumerate(records):
        use = np.flatnonzero(np.any(pair == fi, axis=1))
        if not len(use):
            continue
        xyz = points[use].reshape(-1, 3); q = parameters(xyz, rec)
        # The original charts cover the regional bbox. Moving a boundary may
        # require a small polynomial extension; scipy's spline basis supports
        # evaluation only within its declared domain in the production path.
        if np.any(q < -1e-9) or np.any(q > 1+1e-9):
            raise ValueError(f'Boundary left graph support {fi}; extend its support before coupling')
        q = np.clip(q, 0, 1); knots = rec['knots']; a = tensor(q, knots, knots)
        value = a@rec['height']-(xyz-rec['origin'])@rec['axes'][2]
        gradient = ((tensor(q, knots, knots, 1, 0)@rec['height'])[:, None]*rec['axes'][0]/rec['size'][0]
                    +(tensor(q, knots, knots, 0, 1)@rec['height'])[:, None]*rec['axes'][1]/rec['size'][1]
                    -rec['axes'][2])
        coo = a.tocoo(); rows.append(coo.row+row_offset); cols.append(coo.col+offsets[fi]); values.append(coo.data)
        derivative = np.einsum('etd,ecd->etc', gradient.reshape(len(use), len(t), 3), directions[use])*b[None]
        rr = np.arange(len(xyz)).reshape(len(use), len(t), 1)+row_offset
        rows.append(np.broadcast_to(rr, derivative.shape).ravel())
        cols.append(np.broadcast_to(maps[use, None, :]+total_heights, derivative.shape).ravel())
        values.append(derivative.ravel()); residuals.append(value); row_offset += len(xyz)
    matrix = sparse.coo_matrix((np.concatenate(values), (np.concatenate(rows), np.concatenate(cols))),
                               shape=(row_offset, variables)).tocsr()
    return matrix, np.concatenate(residuals)


def couple(run, output, degree=3, samples=13, iterations=5, penalty=1000., solver='direct', movement_limit=.02,
           boundary_regularization=10.):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False); (out/'charts').mkdir()
    started = time.perf_counter()
    metadata, records, offsets, original, edges, pair, vertices, maps, base, directions = prepare(root, degree, movement_limit)
    report = dict(status='running', can_apply=False, commercial_engine_used=False, source_fit=str(root.resolve()),
        partition=metadata['partition'], stl=metadata['stl'], algorithm='joint-regional-graph-boundary-v1',
        degree=degree, samples_per_edge=samples, iterations=iterations, penalty=penalty, solver=solver,
        movement_limit_mm=movement_limit, boundary_regularization=boundary_regularization,
        source_CAD_read=False, fitted_charts=len(records))
    dump(out/'run.json', report); history = []
    current = np.zeros(int(offsets[-1])+int(maps.max())+1); t = np.linspace(0, 1, samples)
    prior = np.ones(len(current)); prior[offsets[-1]:] = boundary_regularization
    try:
        for iteration in range(iterations):
            tick = time.perf_counter(); displacement = current[offsets[-1]:]
            curves = base+displacement[maps, None]*directions
            jacobian, residual = curve_constraints(records, offsets, curves, pair, maps, directions, t)
            scale = 1/np.maximum(np.sqrt(np.asarray(jacobian.power(2).sum(axis=0)).ravel()+1/penalty**2), 1e-15)
            scaled = jacobian@sparse.diags(scale)
            regularizer = sparse.diags(scale*prior/penalty)
            if solver == 'direct':
                h = (scaled.T@scaled+regularizer.T@regularizer).tocsc()
                delta = spsolve(h, -scaled.T@residual-current*scale*prior**2/penalty**2)*scale
                solve_info = dict(method='sparse-normal-equations')
            else:
                matrix = sparse.vstack([scaled, regularizer], format='csr')
                solved = lsmr(matrix, np.r_[-residual, -current*prior/penalty], atol=1e-10, btol=1e-10, maxiter=1200)
                delta = solved[0]*scale
                solve_info = dict(method='lsmr', stop=int(solved[1]), iterations=int(solved[2]), residual=float(solved[3]))
            if not np.isfinite(delta).all():
                raise ValueError('Boundary coupling produced a nonfinite step')
            accepted = False
            for damping in [1., .5, .25, .125, .0625]:
                trial = current+damping*delta
                if np.max(abs(trial[offsets[-1]:])) > movement_limit:
                    continue
                for fi, rec in enumerate(records):
                    rec['height'] = original[offsets[fi]:offsets[fi+1]]+trial[offsets[fi]:offsets[fi+1]]
                candidate = base+trial[offsets[-1]:][maps, None]*directions
                try:
                    _, error = curve_constraints(records, offsets, candidate, pair, maps, directions, t)
                except ValueError:
                    continue
                if np.linalg.norm(error) <= np.linalg.norm(residual)*(1+1e-8):
                    current = trial; accepted = True; break
            if not accepted:
                raise ValueError('No feasible boundary coupling step inside the declared movement/support limits')
            item = dict(iteration=iteration, before_mm=stats(abs(residual)), after_mm=stats(abs(error)),
                movement_mm=stats(abs(current[offsets[-1]:])), damping=damping, seconds=time.perf_counter()-tick, solver=solve_info)
            history.append(item); dump(out/'iterations.json', history); print(item, flush=True)
        curves = base+current[offsets[-1]:][maps, None]*directions
        _, validation = curve_constraints(records, offsets, curves, pair, maps, directions, np.linspace(0, 1, 51))
        for fi, rec in enumerate(records):
            np.savez_compressed(out/'charts'/f'{fi:05d}.npz', **rec)
        np.savez_compressed(out/'boundary_curves.npz', mesh_edges=edges, owners=pair, vertex_ids=vertices,
                            poles=curves, control_displacements=current[offsets[-1]:], original_poles=base)
        with np.load(root/'chart_index.npz') as index:
            np.savez_compressed(out/'chart_index.npz', **dict(index))
        report.update(status='DiagnosticOnly', history=history, validation_curve_support_mm=stats(abs(validation)),
            actual_control_points=int(offsets[-1]), shared_curves=len(edges),
            maximum_boundary_control_displacement_mm=float(np.max(abs(current[offsets[-1]:]))))
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--degree', type=int, default=3); p.add_argument('--samples', type=int, default=13)
    p.add_argument('--iterations', type=int, default=5); p.add_argument('--penalty', type=float, default=1000.)
    p.add_argument('--solver', choices=['direct', 'lsmr'], default='direct'); p.add_argument('--movement-limit', type=float, default=.02)
    p.add_argument('--boundary-regularization', type=float, default=10.)
    a = p.parse_args(); couple(a.run, a.output, a.degree, a.samples, a.iterations, a.penalty, a.solver,
                              a.movement_limit, a.boundary_regularization)
