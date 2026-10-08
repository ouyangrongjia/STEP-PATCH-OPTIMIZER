"""Fair cached graph supports inside explicit mesh/boundary error bounds.

The optional OSQP dependency is used only by this experimental postprocessor.
Chart topology, labels and spline dimensions stay fixed. Failed/infeasible
updates retain their original support and are recorded, never called solved.
"""
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy import sparse
from .mesh import dump, vertex_normals
from .splines import tensor
from .partition_fit import parameters, boundary_edges, incidence, Config, polyline_graph_error, loops


def normal_cone_constraints(du, dv, normals, degrees):
    """Linear inner approximation to a circular cone of oriented graph normals.

    The graph normal is (-du*h, -dv*h, 1). Four tangent inequalities form
    an inscribed square cone, so every feasible sample is within ``degrees``
    of its target. This is a sampled bound, not a continuous G1 certificate.
    """
    if not np.isfinite(degrees) or not 0 < degrees < 90:
        raise ValueError('Normal cone angle must be between 0 and 90 degrees')
    normal = np.asarray(normals, dtype=float)
    length = np.linalg.norm(normal, axis=1)
    if not np.isfinite(normal).all() or np.any(length <= 0):
        raise ValueError('Normal cone targets must be finite and nonzero')
    normal = normal/length[:, None]
    seed = np.eye(3)[np.argmin(abs(normal), axis=1)]
    tangent = np.cross(normal, seed); tangent /= np.linalg.norm(tangent, axis=1)[:, None]
    second = np.cross(normal, tangent); alpha = np.tan(np.radians(degrees))/np.sqrt(2)
    operators = []; upper = []
    for direction in (tangent, -tangent, second, -second):
        plane = direction-alpha*normal
        operators.append(-du.multiply(plane[:, 0, None])-dv.multiply(plane[:, 1, None]))
        upper.append(-plane[:, 2])
    # Exclude the backwards branch and the cone apex explicitly.
    operators.append(du.multiply(normal[:, 0, None])+dv.multiply(normal[:, 1, None]))
    upper.append(normal[:, 2]-1e-8)
    upper = np.concatenate(upper)
    return sparse.vstack(operators, format='csc'), np.full(len(upper), -np.inf), upper


def optimize(run, output, regions=None, normal_weight=.01, fairness=1e-7,
             position_weight=1., time_limit=3., subdivisions=4, boundary_normal_weight=0.,
             normal_field='regional', boundary_normal_limit=None, boundary_smoothing=0.,
             boundary_target_tolerance=None, normal_filter_steps=0):
    import osqp
    values = (normal_weight, fairness, position_weight, time_limit, boundary_normal_weight)
    if (not np.isfinite(values).all() or min(normal_weight, fairness, boundary_normal_weight) < 0
            or min(position_weight, time_limit) <= 0 or subdivisions < 1 or int(subdivisions) != subdivisions):
        raise ValueError('Finite nonnegative weights, positive position weight/time, and integral subdivisions >= 1 are required')
    if boundary_normal_limit is not None and (not np.isfinite(boundary_normal_limit)
                                              or not 0 < boundary_normal_limit < 90):
        raise ValueError('Boundary normal limit must be between 0 and 90 degrees')
    if not np.isfinite(boundary_smoothing) or boundary_smoothing < 0:
        raise ValueError('Boundary smoothing must be finite and nonnegative')
    if boundary_target_tolerance is not None and (not np.isfinite(boundary_target_tolerance)
                                                  or boundary_target_tolerance <= 0):
        raise ValueError('Boundary target tolerance must be finite and positive')
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False); (out/'charts').mkdir()
    started = time.perf_counter(); metadata = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if metadata.get('status') != 'DiagnosticOnly':
        raise ValueError('The source fitting cache has not completed')
    cfg = Config(**metadata['config']); data = np.load(metadata['partition'])
    v, f, labels = (data[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    if regions is not None and not set(regions).issubset(set(labels)):
        raise ValueError('Requested source regions are absent')
    if normal_field not in ('regional', 'crease-fans'):
        raise ValueError('Unknown source normal field')
    if normal_filter_steps and normal_field != 'crease-fans':
        raise ValueError('Normal filtering requires the shared crease-fan field')
    edges, owners = incidence(f); shared = vertex_normals(v, f)
    corners = None
    if normal_field == 'crease-fans':
        from .source_normals import crease_fan_normals
        corners = crease_fan_normals(v, f, cfg.feature_angle, (edges, owners), normal_filter_steps)
        edge_keys = edges[:, 0]*len(v)+edges[:, 1]
    fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    fn /= np.linalg.norm(fn, axis=1)[:, None]
    sharp = np.unique(edges[np.sum(fn[owners[:, 0]]*fn[owners[:, 1]], axis=1)
                            < np.cos(np.radians(cfg.feature_angle))])
    normals = {}
    for region in np.unique(labels):
        tri = f[labels == region]; used = np.unique(tri)
        nn = vertex_normals(v[used], np.searchsorted(used, tri))
        if cfg.shared_smooth_normals:
            smooth = ~np.isin(used, sharp); nn[smooth] = shared[used[smooth]]
        normals[int(region)] = used, nn
    report = dict(status='running', source_fit=str(root.resolve()),
        partition=metadata['partition'], stl=metadata['stl'], config=metadata['config'],
        source_CAD_read=False, commercial_engine_used=False, can_apply=False,
        algorithm='native-graph-bounded-quadratic-fairing-v1', solver_version=osqp.__version__,
        optimization=dict(regions=regions, normal_weight=normal_weight, fairness=fairness,
                          normal_field=normal_field,
                          normal_filter_steps=normal_filter_steps,
                          normal_filter_angle_degrees=cfg.feature_angle/2,
                          normal_filter_spatial_scale='median adjacent face-center distance',
                          boundary_normal_weight=boundary_normal_weight,
                          boundary_normal_limit_degrees=boundary_normal_limit,
                          boundary_smoothing_mm=boundary_smoothing,
                          boundary_target_tolerance_mm=boundary_target_tolerance,
                          position_weight=position_weight, solver_time_limit_seconds=time_limit,
                          subdivisions=subdivisions, maximum_iterations=6000))
    report['timing_scope'] = 'OSQP solve time limit excludes setup and independent geometric checks; total elapsed time includes them.'
    report['config_scope'] = 'Original fitter configuration retained for subsequent local refinement; QP weights are recorded separately.'
    dump(out/'run.json', report); events = []
    weights = np.array([[i, j, subdivisions-i-j] for i in range(subdivisions+1)
                        for j in range(subdivisions+1-i)], dtype=float)/subdivisions
    try:
        paths = sorted((root/'charts').glob('*.npz'))
        if boundary_smoothing:
            from .boundary_samples import smooth_samples
            if boundary_smoothing >= cfg.boundary:
                raise ValueError('Boundary smoothing must leave room within the declared boundary budget')
            selected = [i for i, path in enumerate(paths)
                        if regions is None or int(np.load(path)['region']) in regions]
            curve_keys, curve_points, curve_tangents, curve_report = smooth_samples(v, edges, owners,
                np.load(root/'chart_index.npz')['face_chart'], selected, boundary_smoothing)
            dump(out/'boundary_curves.json', curve_report)
        for chart, path in enumerate(paths):
            rec = dict(np.load(path)); region = int(rec['region']); tick = time.perf_counter()
            if regions is not None and region not in regions:
                shutil.copyfile(path, out/'charts'/path.name); continue
            if boundary_smoothing:
                from .joint_graph_boundary import extend_support
                extend_support(rec, boundary_smoothing*1.1)
            ids = rec['triangle_ids']; used = rec['vertex_ids']; tri = np.searchsorted(used, f[ids])
            boundary_budget = min(cfg.boundary, float(rec.get('boundary_budget_mm', cfg.boundary)))
            target_budget = min(boundary_budget*.549, boundary_target_tolerance
                                if boundary_target_tolerance is not None else boundary_budget*.549)
            boundary = used[boundary_edges(tri)]; t = rec['knots']; n = len(t)-4
            points = np.r_[v[used], v[f[ids]].mean(axis=1),
                           np.einsum('bc,tcd->tbd', weights, v[f[ids]]).reshape(-1, 3)]
            q = parameters(points, rec); z = (points-rec['origin'])@rec['axes'][2]
            a = tensor(q, t, t)
            w = np.linspace(0, 1, 17)
            bp = (v[boundary[:, 0], None]*(1-w[None, :, None])
                  + v[boundary[:, 1], None]*w[None, :, None]).reshape(-1, 3)
            if boundary_smoothing:
                canonical = np.sort(boundary, axis=1)
                ci = np.searchsorted(curve_keys, canonical[:, 0]*len(v)+canonical[:, 1])
                bp = curve_points[ci].copy(); tangent = curve_tangents[ci].copy()
                reverse = boundary[:, 0] > boundary[:, 1]
                bp[reverse] = bp[reverse, ::-1]; tangent[reverse] = tangent[reverse, ::-1]
                bp = bp.reshape(-1, 3); tangent = tangent.reshape(-1, 3)@rec['axes'].T
            bq = parameters(bp, rec); bz = (bp-rec['origin'])@rec['axes'][2]; b = tensor(bq, t, t)
            if boundary_smoothing and (np.any(bq < -1e-9) or np.any(bq > 1+1e-9)):
                raise ValueError('Smoothed boundary left the extended support domain')
            if corners is None:
                rn, nn = normals[region]; nn = nn[np.searchsorted(rn, used)]@rec['axes'].T
                uv = parameters(v[used], rec)
                local_boundary = np.searchsorted(used, boundary)
                boundary_nn = nn[local_boundary]
            else:
                nn = corners[ids].reshape(-1, 3)@rec['axes'].T
                uv = parameters(v[f[ids]].reshape(-1, 3), rec)
                sorted_edges = np.sort(boundary, axis=1)
                index = np.searchsorted(edge_keys, sorted_edges[:, 0]*len(v)+sorted_edges[:, 1])
                owner = np.where(np.isin(owners[index, 0], ids), owners[index, 0], owners[index, 1])
                local = np.argmax(f[owner, None] == boundary[:, :, None], axis=-1)
                boundary_nn = corners[owner[:, None], local]@rec['axes'].T
            good = nn[:, 2] > .1
            blocks = [a*np.sqrt(position_weight/len(q))]; rhs = [z*np.sqrt(position_weight/len(q))]
            for axis in range(2):
                op = tensor(uv[good], t, t, int(axis == 0), int(axis == 1))
                scale = np.sqrt(normal_weight/max(1, good.sum()))
                blocks.append(op.multiply(nn[good, 2, None]/rec['size'][axis])*scale)
                rhs.append(-nn[good, axis]*scale)
            if boundary_normal_weight or boundary_normal_limit is not None:
                bn = (boundary_nn[:, 0, None]*(1-w[None, :, None])
                      + boundary_nn[:, 1, None]*w[None, :, None]).reshape(-1, 3)
                if boundary_smoothing:
                    # Curve positions and normal targets must agree on the
                    # same tangent. Adjacent smooth fans receive one target.
                    bn -= np.sum(bn*tangent, axis=1)[:, None]*tangent
                bn /= np.maximum(np.linalg.norm(bn, axis=1)[:, None], 1e-30)
            if boundary_normal_weight:
                bgood = bn[:, 2] > .1
                scale = np.sqrt(boundary_normal_weight/max(1, bgood.sum()))
                for axis in range(2):
                    op = tensor(bq[bgood], t, t, int(axis == 0), int(axis == 1))
                    blocks.append(op.multiply(bn[bgood, 2, None]/rec['size'][axis])*scale)
                    rhs.append(-bn[bgood, axis]*scale)
            grid = np.linspace(0, 1, max(9, 2*n+1))
            fq = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
            for du, dv in [(2, 0), (1, 1), (0, 2)]:
                op = tensor(fq, t, t, du, dv)
                scale = np.sqrt(fairness/len(fq))/(rec['size'][0]**du*rec['size'][1]**dv)
                blocks.append(op*scale); rhs.append(np.zeros(len(fq)))
            matrix = sparse.vstack(blocks, format='csr'); target = np.concatenate(rhs)
            h = (matrix.T@matrix).tocsc(); linear = -np.asarray(matrix.T@target)
            scale = max(float(h.diagonal().max()), 1e-12); h /= scale; linear /= scale
            h += sparse.eye(n*n, format='csc')*1e-12
            constraint = sparse.vstack([a, b], format='csc'); center = np.r_[z, bz]
            budget = np.r_[np.full(len(z), cfg.distance*.999), np.full(len(bz), target_budget)]
            lower, upper = center-budget, center+budget
            if boundary_normal_limit is not None:
                bu = tensor(bq, t, t, 1, 0)/rec['size'][0]
                bv = tensor(bq, t, t, 0, 1)/rec['size'][1]
                cone, cone_lower, cone_upper = normal_cone_constraints(bu, bv, bn, boundary_normal_limit)
                constraint = sparse.vstack([constraint, cone], format='csc')
                lower = np.r_[lower, cone_lower]; upper = np.r_[upper, cone_upper]
            model = osqp.OSQP()
            model.setup(P=sparse.triu(h, format='csc'), q=linear, A=constraint, l=lower,
                        u=upper, eps_abs=1e-7, eps_rel=1e-7, max_iter=6000,
                        time_limit=time_limit, verbose=False, polishing=True)
            model.warm_start(x=rec['height']); result = model.solve(raise_error=False)
            info = dict(chart=chart, region=region, status=result.info.status,
                        iterations=result.info.iter, accepted=False)
            if result.x is not None and np.isfinite(result.x).all() and result.info.status_val in (1, 2):
                coeff = result.x; err = float(np.max(abs(a@coeff-z))); be = float(np.max(abs(b@coeff-bz)))
                old_objective = float(np.sum((matrix@rec['height']-target)**2))
                new_objective = float(np.sum((matrix@coeff-target)**2))
                info.update(distance_max_mm=err, boundary_sample_max_mm=be,
                            original_objective=old_objective, proposed_objective=new_objective)
                normal_pass = True
                if boundary_normal_limit is not None:
                    measured = np.c_[-bu@coeff, -bv@coeff, np.ones(len(bq))]
                    measured /= np.linalg.norm(measured, axis=1)[:, None]
                    angle = float(np.max(np.degrees(np.arccos(np.clip(np.sum(measured*bn, axis=1), -1, 1)))))
                    info['boundary_normal_max_degrees'] = angle
                    normal_pass = angle <= boundary_normal_limit+1e-4
                # A hard normal cone may require a larger soft objective than
                # the unconstrained input. Geometry and angle bounds decide it.
                objective_pass = boundary_normal_limit is not None or new_objective <= old_objective
                if err <= cfg.distance and be <= target_budget*1.001 and normal_pass and objective_pass:
                    original = rec['height']; rec['height'] = coeff
                    # Batch the same polynomial extrema check by closed loop;
                    # one sparse basis construction replaces one per mesh edge.
                    exact = max(polyline_graph_error(v[np.r_[loop, loop[0]]], rec) for loop in loops(boundary))
                    info['continuous_boundary_max_mm'] = exact
                    if exact <= boundary_budget*.9:
                        info['accepted'] = True
                    else:
                        rec['height'] = original
            info['seconds'] = time.perf_counter()-tick; events.append(info)
            np.savez_compressed(out/'charts'/path.name, **rec)
            if len(events) % 25 == 0:
                print(dict(processed=len(events), accepted=sum(x['accepted'] for x in events),
                           seconds=time.perf_counter()-started), flush=True)
        shutil.copyfile(root/'chart_index.npz', out/'chart_index.npz')
        report.update(status='DiagnosticOnly', can_apply=False, fitted_charts=len(list((out/'charts').glob('*.npz'))),
            optimized_charts=sum(x['accepted'] for x in events), retained_charts=sum(not x['accepted'] for x in events),
            attempted_charts=len(events), partition_labels_unchanged=True, complete_coverage=True)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        dump(out/'optimization.json', events); report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', type=int, nargs='+'); p.add_argument('--normal-weight', type=float, default=.01)
    p.add_argument('--fairness', type=float, default=1e-7); p.add_argument('--position-weight', type=float, default=1.)
    p.add_argument('--seconds-per-chart', type=float, default=3.); p.add_argument('--subdivisions', type=int, default=4)
    p.add_argument('--boundary-normal-weight', type=float, default=0.)
    p.add_argument('--normal-field', choices=['regional', 'crease-fans'], default='regional')
    p.add_argument('--boundary-normal-limit', type=float)
    p.add_argument('--boundary-smoothing', type=float, default=0.)
    p.add_argument('--boundary-target-tolerance', type=float)
    p.add_argument('--normal-filter-steps', type=int, default=0)
    a = p.parse_args(); optimize(a.run, a.output, a.regions, a.normal_weight, a.fairness,
                               a.position_weight, a.seconds_per_chart, a.subdivisions, a.boundary_normal_weight,
                               a.normal_field, a.boundary_normal_limit, a.boundary_smoothing, a.boundary_target_tolerance,
                               a.normal_filter_steps)
