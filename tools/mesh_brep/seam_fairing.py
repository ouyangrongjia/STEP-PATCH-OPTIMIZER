"""Coupled Gauss--Newton fairing of graph supports on fixed shared curves.

The variables are height coefficients. A common seam contributes one residual
between the two actual unit normals, including charts with different frames.
Source creases are excluded. Positional tubes remain hard constraints.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline
from .mesh import dump
from .partition_fit import parameters, spline_graph_error
from .splines import tensor
from .bounded_support import bending_matrix
from .support_probe import probes
from .source_normals import crease_fan_normals
from .shared_support import source_angles


@dataclass
class Config:
    fairness: float = 1e-5
    normal_weight: float = .03
    seam_weight: float = .05
    smooth_degrees: float = 10.
    iterations: int = 3
    solver_seconds: float = 90.


def normal_jacobian(record, xyz):
    q = parameters(xyz, record); t = record['knots']; ax = record['axes']
    du = tensor(q, t, t, 1, 0)/record['size'][0]
    dv = tensor(q, t, t, 0, 1)/record['size'][1]
    raw = ax[2]-np.outer(du@record['height'], ax[0])-np.outer(dv@record['height'], ax[1])
    size = np.linalg.norm(raw, axis=1); n = raw/size[:, None]
    projection = (np.eye(3)[None]-n[:, :, None]*n[:, None, :])/size[:, None, None]
    gu = -projection@ax[0]; gv = -projection@ax[1]
    derivative = [du.multiply(gu[:, k, None])+dv.multiply(gv[:, k, None]) for k in range(3)]
    return n, derivative


def embed(a, offset, total):
    a = a.tocoo()
    return sparse.coo_matrix((a.data, (a.row, a.col+offset)), shape=(a.shape[0], total)).tocsr()


def seam_cost(records, seams):
    energy = 0.; all_angles = []
    for pair, xyz, weight in seams:
        left, _ = normal_jacobian(records[pair[0]], xyz)
        right, _ = normal_jacobian(records[pair[1]], xyz)
        energy += float(weight@np.sum((left-right)**2, axis=1))
        all_angles.extend(np.degrees(np.arccos(np.clip(np.sum(left*right, axis=1), -1, 1))))
    return dict(normal_difference_energy=energy, samples=len(all_angles),
                normal_p95_degrees=float(np.quantile(all_angles, .95)), normal_max_degrees=float(max(all_angles)))


def optimize(source, output, regions, config=None):
    import osqp
    config = config or Config(); root, out = Path(source), Path(output)
    out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    if min(config.fairness, config.normal_weight, config.seam_weight, config.solver_seconds) <= 0:
        raise ValueError('Positive fairing weights and solver budget are required')
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if not 0 < config.smooth_degrees <= meta['config']['feature_angle']:
        raise ValueError('Smooth-join threshold must preserve the declared source crease threshold')
    if meta.get('requires_support_refit'):
        raise ValueError('Fit the moved network before coupling its normals')
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    selected = {di for di, r in records.items() if int(r['region']) in regions}
    from .shared_support import restore_groups
    restore_groups(records)
    selected.update(i for di in list(selected) for i in records[di].get('shared_support_group', []))
    offsets = {}; total = 0
    for di in sorted(selected):
        offsets[di] = total; total += len(records[di]['height'])
    if not selected:
        raise ValueError('No selected source region')
    normals = crease_fan_normals(v, f, meta['config']['feature_angle'], filter_steps=meta['normal_filter_steps'])
    lookup = source_angles(v, f); seams = []; boundary = {di: [] for di in selected}
    incident = {di: [] for di in selected}
    for ci, c in curves.items():
        pair = tuple(map(int, c['owners']))
        if not selected.intersection(pair):
            continue
        cut = np.unique(np.r_[c['parameters'], c['knots']])
        q = np.unique((cut[:-1, None]+np.diff(cut)[:, None]*np.linspace(0, 1, 5)).ravel())
        spline = BSpline(c['knots'], c['poles'], 3); xyz = spline(q)
        for di in selected.intersection(pair):
            boundary[di].append(xyz); incident[di].append(c)
        source = np.array([lookup[tuple(sorted((a, b)))] for a, b in zip(c['chain'][:-1], c['chain'][1:])])
        smooth = source[np.clip(np.searchsorted(c['parameters'], q, side='right')-1, 0, len(source)-1)] <= config.smooth_degrees
        if smooth.any():
            weights = np.r_[np.diff(q), 0.]+np.r_[0., np.diff(q)]
            weights *= np.linalg.norm(spline(q, 1), axis=1)/2
            seams.append((pair, xyz[smooth], weights[smooth]))
    if not seams:
        raise ValueError('No smooth-source seams in selected regions')
    hblocks = []; linear = []; constraints = []; lower = []; upper = []; prepared = {}
    distance = meta['config']['distance']; boundary_limit = meta['config']['boundary']*.4
    for di in sorted(selected):
        rec = records[di]; used = rec['vertex_ids']; ids = rec['triangle_ids']; t = rec['knots']
        tri = np.searchsorted(used, f[ids]); source = np.r_[v[used], v[f[ids]].mean(axis=1)]
        q = parameters(source, rec); z = (source-rec['origin'])@rec['axes'][2]
        area2 = np.linalg.norm(np.cross(v[f[ids, 1]]-v[f[ids, 0]], v[f[ids, 2]]-v[f[ids, 0]]), axis=1)
        va = np.bincount(tri.ravel(), weights=np.repeat(area2/6, 3), minlength=len(used))
        weights = np.r_[va/2, area2/4]
        a = tensor(q, t, t); h = a.T@a.multiply(weights[:, None]); lin = a.T@(weights*z)
        nq = parameters(v[f[ids]].reshape(-1, 3), rec); nn = normals[ids].reshape(-1, 3)@rec['axes'].T
        good = nn[:, 2] > .15; nw = np.repeat(area2/6, 3)[good]*config.normal_weight
        for axis in range(2):
            d = tensor(nq[good], t, t, int(axis == 0), int(axis == 1))/rec['size'][axis]
            op = d.multiply(nn[good, 2, None])
            h += op.T@op.multiply(nw[:, None]); lin -= op.T@(nw*nn[good, axis])
        h += bending_matrix(t, rec['size'])*config.fairness*(area2.sum()/2)
        hblocks.append(h); linear.append(-lin)
        iq, iz = probes(rec, v[used], tri)
        bxyz = np.concatenate(boundary[di]); bq = parameters(bxyz, rec); bz = (bxyz-rec['origin'])@rec['axes'][2]
        b = tensor(bq, t, t); p = tensor(iq, t, t)
        constraints.append(embed(sparse.vstack([a, p, b]), offsets[di], total))
        lower.append(np.r_[z-distance*.999, iz-distance*.999, bz-boundary_limit*.65])
        upper.append(np.r_[z+distance*.999, iz+distance*.999, bz+boundary_limit*.65])
        prepared[di] = dict(probes=p, target=iz, boundary=b, boundary_target=bz,
                            position=a, position_target=z,
                            source_points=source, source_normal_points=v[f[ids]].mean(axis=1),
                            source_normals=normals[ids].mean(axis=1))
    base = sparse.block_diag(hblocks, format='csc'); base_linear = np.concatenate(linear)
    constraint = sparse.vstack(constraints, format='csc'); lower = np.concatenate(lower); upper = np.concatenate(upper)
    before = seam_cost(records, seams); current = dict(records); history = []; accepted = False
    print(dict(stage='coupled_supports', charts=len(selected), variables=total, seams=len(seams), before=before), flush=True)
    for iteration in range(config.iterations):
        ops = []; rhs = []
        for pair, xyz, weights in seams:
            ns = []; js = []
            for di in pair:
                n, j = normal_jacobian(current[di], xyz); ns.append(n); js.append(j)
            for axis in range(3):
                op = sparse.csr_matrix((len(xyz), total)); target = -(ns[0][:, axis]-ns[1][:, axis])
                for side, di in enumerate(pair):
                    if di in offsets:
                        sign = 1 if side == 0 else -1
                        op += embed(js[side][axis], offsets[di], total)*sign
                        target += sign*(js[side][axis]@current[di]['height'])
                w = np.sqrt(config.seam_weight*weights)
                ops.append(op.multiply(w[:, None])); rhs.append(target*w)
        op = sparse.vstack(ops, format='csr'); target = np.concatenate(rhs)
        h = (base+op.T@op).tocsc(); linear = base_linear-op.T@target
        diagonal = np.sqrt(np.maximum(h.diagonal(), np.median(h.diagonal())*1e-12))
        inv = sparse.diags(1/diagonal); scaled = inv@h@inv+sparse.eye(total, format='csc')*1e-14
        warm = np.concatenate([current[di]['height'] for di in sorted(selected)])
        solver = osqp.OSQP()
        solver.setup(P=sparse.triu(scaled, format='csc'), q=linear/diagonal, A=constraint@inv,
                     l=lower, u=upper, eps_abs=1e-7, eps_rel=1e-7, max_iter=16000,
                     time_limit=config.solver_seconds, polishing=True, verbose=False)
        solver.warm_start(x=warm*diagonal); result = solver.solve(raise_error=False)
        coeff = result.x/diagonal; val = constraint@coeff
        violation = float(max(np.max(lower-val), np.max(val-upper), 0.))
        event = dict(iteration=iteration, solver_status=result.info.status, solver_seconds=result.info.run_time,
                     constraint_violation_mm=violation); history.append(event)
        np.savez_compressed(out/f'iteration_{iteration:02d}.npz', height=coeff, charts=np.array(sorted(selected)))
        # The linear system reserves a margin inside the unchanged geometric
        # budgets. An inexact iterate may use that margin; actual bounds below
        # still decide acceptance, and solver convergence is reported verbatim.
        if result.info.status_val not in (1, 2, 7, 8) or not np.isfinite(coeff).all() or violation > distance*.1:
            break
        candidate = dict(current); rejected = []; accepted_fractions = {}
        for di in sorted(selected):
            old = current[di]; count = len(old['height']); proposal = coeff[offsets[di]:offsets[di]+count]
            p = prepared[di]
            target = p['source_normals']; target = target/np.linalg.norm(target, axis=1)[:, None]
            old_n, _ = normal_jacobian(old, p['source_normal_points'])
            old_angle = np.degrees(np.arccos(np.clip(np.sum(old_n*target, axis=1), -1, 1)))
            chosen = None
            for alpha in (1., .5, .25, .125):
                c = dict(old, height=old['height']+alpha*(proposal-old['height']))
                boundary_error = max(spline_graph_error(curve['poles'], curve['knots'], c) for curve in incident[di])
                interior_error = float(abs(p['probes']@c['height']-p['target']).max(initial=0.))
                position_error = float(abs(p['position']@c['height']-p['position_target']).max(initial=0.))
                new_n, _ = normal_jacobian(c, p['source_normal_points'])
                new_angle = np.degrees(np.arccos(np.clip(np.sum(new_n*target, axis=1), -1, 1)))
                if (boundary_error < boundary_limit and max(interior_error, position_error) < distance and
                        np.quantile(new_angle, .95) <= np.quantile(old_angle, .95)+2 and
                        new_angle.max() <= max(60., old_angle.max()+2)):
                    chosen = c; accepted_fractions[str(di)] = alpha; break
            if chosen is None:
                rejected.append(di)
                continue
            c.update(continuous_boundary_error_mm=boundary_error, interior_probe_max_mm=interior_error,
                     coupled_normal_version=1, sampled_source_max_mm=position_error)
            c.pop('shared_support_group', None); candidate[di] = c
        after = seam_cost(candidate, seams)
        event.update(rejected_supports=rejected, accepted_fractions=accepted_fractions, after=after)
        old_cost = seam_cost(current, seams)
        if (after['normal_difference_energy'] >= old_cost['normal_difference_energy']*.999 or
                after['normal_p95_degrees'] > old_cost['normal_p95_degrees']+.25 or
                after['normal_max_degrees'] > old_cost['normal_max_degrees']+2):
            event['accepted'] = False; break
        current = candidate; accepted = True; event['accepted'] = True
        print(dict(stage='coupled_support_iteration', iteration=iteration,
                   solver_status=event['solver_status'], accepted_supports=len(accepted_fractions),
                   rejected_supports=len(rejected), after=after), flush=True)
        dump(out/'seam_fairing.json', dict(before=before, history=history))
    changed = [di for di in selected if not np.array_equal(current[di]['height'], records[di]['height'])]
    report = dict(accepted=accepted, before=before, after=seam_cost(current, seams), history=history,
                  changed_charts=sorted(map(int, changed)), regions=sorted(map(int, regions)), config=asdict(config),
                  seconds=time.perf_counter()-started, shared_curves_unchanged=True,
                  continuity_scope='Sampled normal agreement, not a G1/G2 certificate', source_CAD_read=False,
                  source_fit=str(root.resolve()), commercial_engine_used=False)
    dump(out/'seam_fairing.json', report)
    if not accepted:
        return report
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for di in changed:
        np.savez_compressed(out/'charts'/f'{di:05d}.npz', **current[di])
    if (root/'routes').exists():
        shutil.copytree(root/'routes', out/'routes')
    meta.update(source_fit=str(out.resolve()), seam_fairing=report, can_apply=False)
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', type=int, nargs='+', required=True)
    p.add_argument('--iterations', type=int, default=3); p.add_argument('--fairness', type=float, default=1e-5)
    p.add_argument('--seam-weight', type=float, default=.05)
    p.add_argument('--smooth-degrees', type=float, default=10.)
    a = p.parse_args(); optimize(a.run, a.output, a.regions, Config(iterations=a.iterations, fairness=a.fairness,
        seam_weight=a.seam_weight, smooth_degrees=a.smooth_degrees))
