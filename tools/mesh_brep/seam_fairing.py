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
from .partition_fit import parameters, positions, spline_graph_error
from .splines import tensor
from .bounded_support import bending_matrix
from .support_probe import probes
from .source_normals import crease_fan_normals
from .shared_support import source_angles
from .surface_jets import jets, COMPONENTS, support_span_parameters
from .regional_quality import weighted_stats
from .support_probe import normal_targets


@dataclass
class Config:
    fairness: float = 1e-5
    normal_weight: float = .03
    seam_weight: float = .05
    smooth_degrees: float = 10.
    iterations: int = 3
    solver_seconds: float = 90.
    curvature_weight: float = 0.
    variation_weight: float = 0.
    normal_slack_degrees: float = 2.
    boundary_source_weight: float = 0.
    solver_retry: bool = True
    step_trials: int = 5
    physical_distance: bool = False
    protect_curvature_energy: bool = False


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


def seam_cost(records, seams, targets=None):
    energy = 0.; curvature_energy = 0.; all_angles = []; all_curvature = []; all_weights = []
    source_energy=0.
    for index,(pair, xyz, weight) in enumerate(seams):
        left, ls = jets(records[pair[0]], xyz)
        right, rs = jets(records[pair[1]], xyz)
        energy += float(weight@np.sum((left-right)**2, axis=1))
        if targets is not None:
            source_energy+=float(weight@np.sum(((left+right)/2-targets[index])**2,axis=1))
        jump = np.linalg.norm(ls-rs, axis=(1, 2))
        curvature_energy += float(weight@jump**2)
        all_angles.extend(np.degrees(np.arccos(np.clip(np.sum(left*right, axis=1), -1, 1))))
        all_curvature.extend(jump); all_weights.extend(weight)
    ns = weighted_stats(np.asarray(all_angles), np.asarray(all_weights))
    cs = weighted_stats(np.asarray(all_curvature), np.asarray(all_weights))
    return dict(normal_difference_energy=energy, source_mean_normal_energy=source_energy, samples=len(all_angles),
                normal_p95_degrees=ns['p95'], normal_max_degrees=ns['max'],
                curvature_difference_energy=curvature_energy, curvature_p95_per_mm=cs['p95'], curvature_max_per_mm=cs['max'])


def seam_objective(cost, config):
    return (config.seam_weight*cost['normal_difference_energy']
            +config.curvature_weight*cost['curvature_difference_energy']
            +config.boundary_source_weight*cost['source_mean_normal_energy'])


def optimize(source, output, regions=None, config=None, chart_ids=None, context=None):
    import osqp
    config = config or Config(); root, out = Path(source), Path(output)
    out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    if min(config.fairness, config.normal_weight, config.seam_weight, config.solver_seconds) <= 0:
        raise ValueError('Positive fairing weights and solver budget are required')
    if not 1 <= config.step_trials <= 5:
        raise ValueError('Fairing step trials must be between one and five')
    meta = context['meta'] if context is not None else json.loads((root/'run.json').read_text(encoding='utf-8'))
    if min(config.curvature_weight, config.variation_weight, config.boundary_source_weight) < 0:
        raise ValueError('Negative curvature or curvature-variation weight')
    if not np.isfinite(config.normal_slack_degrees) or config.normal_slack_degrees < 0:
        raise ValueError('Invalid fixed-reference normal allowance')
    if not 0 < config.smooth_degrees <= meta['config']['feature_angle']:
        raise ValueError('Smooth-join threshold must preserve the declared source crease threshold')
    if meta.get('requires_support_refit'):
        raise ValueError('Fit the moved network before coupling its normals')
    if context is None:
        data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
        records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
        curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    else:
        v, f, records, curves = (context[k] for k in ('vertices', 'triangles', 'records', 'curves'))
    selected = set(map(int, chart_ids)) if chart_ids is not None else {di for di, r in records.items() if regions is None or int(r['region']) in regions}
    from .shared_support import restore_groups
    restore_groups(records)
    selected.update(i for di in list(selected) for i in records[di].get('shared_support_group', []))
    offsets = {}; total = 0
    for di in sorted(selected):
        offsets[di] = total; total += len(records[di]['height'])
    if not selected:
        raise ValueError('No selected source region')
    normals = context['normals'] if context is not None else crease_fan_normals(v, f, meta['config']['feature_angle'], filter_steps=meta['normal_filter_steps'])
    reference_records = context.get('reference_records', records) if context is not None else records
    lookup = context['lookup'] if context is not None else source_angles(v, f)
    seams = []; seam_targets=[]; boundary = {di: [] for di in selected}
    incident = {di: [] for di in selected}
    for ci, c in curves.items():
        pair = tuple(map(int, c['owners']))
        if not selected.intersection(pair):
            continue
        q = support_span_parameters(c, [records[di] for di in pair])
        spline = BSpline(c['knots'], c['poles'], 3); xyz = spline(q)
        for di in selected.intersection(pair):
            boundary[di].append(xyz); incident[di].append(c)
        source = np.array([lookup[tuple(sorted((a, b)))] for a, b in zip(c['chain'][:-1], c['chain'][1:])])
        smooth = source[np.clip(np.searchsorted(c['parameters'], q, side='right')-1, 0, len(source)-1)] <= config.smooth_degrees
        if smooth.any():
            weights = np.r_[np.diff(q), 0.]+np.r_[0., np.diff(q)]
            weights *= np.linalg.norm(spline(q, 1), axis=1)/2
            seams.append((pair, xyz[smooth], weights[smooth]))
            if config.boundary_source_weight:
                # Anchor the common mode as well as the difference. Two
                # matching normals can otherwise rotate together into a
                # ripple. Interpolate each owner's original source triangles
                # independently, preserving source crease-side identity.
                target=np.zeros((int(smooth.sum()),3))
                for di in pair:
                    rec=records[di]; used=rec['vertex_ids']; ids=rec['triangle_ids']
                    tri=np.searchsorted(used,f[ids])
                    target+=normal_targets(rec,v[used],tri,normals[ids],parameters(xyz[smooth],rec))@rec['axes']
                target/=np.maximum(np.linalg.norm(target,axis=1)[:,None],1e-30)
                seam_targets.append(target)
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
        if config.variation_weight:
            h += bending_matrix(t, rec['size'], 3)*config.variation_weight*(area2.sum()/2)
        hblocks.append(h); linear.append(-lin)
        iq, iz = probes(rec, v[used], tri)
        bxyz = np.concatenate(boundary[di]); bq = parameters(bxyz, rec); bz = (bxyz-rec['origin'])@rec['axes'][2]
        b = tensor(bq, t, t); p = tensor(iq, t, t)
        constraints.append(embed(sparse.vstack([a, p, b]), offsets[di], total))
        positional_target = np.r_[z, iz]
        positional_limit = np.full(len(positional_target), distance*.999)
        locator = None
        if config.physical_distance:
            from .mesh import Locator
            locator = Locator(v[used], tri)
            xyz = positions(np.r_[q, iq], rec)
            nearest, _, ids_near, _ = locator.closest(xyz)
            facets = v[used][tri[ids_near]]
            normal = np.cross(facets[:, 1]-facets[:, 0], facets[:, 2]-facets[:, 0])
            normal /= np.linalg.norm(normal, axis=1)[:, None]
            cosine = normal@rec['axes'][2]
            cosine = np.where(cosine >= 0, 1., -1.)*np.maximum(abs(cosine), 1e-6)
            signed = np.einsum('ij,ij->i', xyz-nearest, normal)
            positional_target = (xyz-rec['origin'])@rec['axes'][2]-signed/cosine
            positional_limit /= abs(cosine)
        lower.append(np.r_[positional_target-positional_limit, bz-boundary_limit*.65])
        upper.append(np.r_[positional_target+positional_limit, bz+boundary_limit*.65])
        prepared[di] = dict(probes=p, target=iz, boundary=b, boundary_target=bz,
                            position=a, position_target=z,
                            source_points=source, source_normal_points=v[f[ids]].mean(axis=1),
                            source_normals=normals[ids].mean(axis=1),
                            source_locator=locator, source_q=q, interior_q=iq)
        # Keep the reference fixed across iterations, blocks and sweeps. A
        # per-iteration allowance otherwise accumulates into visible ripples.
        center_target=normals[ids].mean(axis=1)
        center_target/=np.linalg.norm(center_target,axis=1)[:,None]
        span_xyz=rec['origin']+(rec['lo']+iq*rec['size'])@rec['axes'][:2]+iz[:,None]*rec['axes'][2]
        span_target=normal_targets(rec,v[used],tri,normals[ids],iq)@rec['axes']
        normal_checks=[]
        for points,target_n,weight in ((v[f[ids]].mean(axis=1),center_target,area2),
                                        (span_xyz,span_target,np.ones(len(iq)))):
            qq=parameters(points,rec)
            du=tensor(qq,t,t,1,0)/rec['size'][0]; dv=tensor(qq,t,t,0,1)/rec['size'][1]
            reference_n,_=normal_jacobian(reference_records[di],points)
            angles=np.degrees(np.arccos(np.clip(np.sum(reference_n*target_n,axis=1),-1,1)))
            normal_checks.append((du,dv,target_n,weight,weighted_stats(angles,weight)))
        prepared[di]['normal_checks']=normal_checks
    # Identical shared supports remain one geometric surface, even when the
    # existing cache stores a copy for each region trim.
    groups = {}
    for di in sorted(selected):
        group = tuple(map(int, records[di].get('shared_support_group', [di])))
        groups[group] = group
        if len(group) > 1 and di != min(group):
            identity = sparse.eye(len(records[di]['height']), format='csr')
            constraints.append(embed(identity, offsets[di], total)-embed(identity, offsets[min(group)], total))
            lower.append(np.zeros(identity.shape[0])); upper.append(np.zeros(identity.shape[0]))
    base = sparse.block_diag(hblocks, format='csc'); base_linear = np.concatenate(linear)
    constraint = sparse.vstack(constraints, format='csc'); lower = np.concatenate(lower); upper = np.concatenate(upper)
    targets=seam_targets if config.boundary_source_weight else None
    before = seam_cost(records, seams,targets); current = dict(records); history = []; accepted = False
    def normal_measurements(di, record):
        measured=[]
        for du,dv,target_n,weight,reference in prepared[di]['normal_checks']:
            raw=record['axes'][2]-np.outer(du@record['height'],record['axes'][0])-np.outer(dv@record['height'],record['axes'][1])
            n=raw/np.linalg.norm(raw,axis=1)[:,None]
            angle=np.degrees(np.arccos(np.clip(np.sum(n*target_n,axis=1),-1,1)))
            measured.append(weighted_stats(angle,weight))
        return measured

    def constrained_record(di, old, height):
        p = prepared[di]; c = dict(old, height=height)
        boundary_error = max(spline_graph_error(curve['poles'], curve['knots'], c) for curve in incident[di])
        interior_error = float(abs(p['probes']@height-p['target']).max(initial=0.))
        position_error = float(abs(p['position']@height-p['position_target']).max(initial=0.))
        if p['source_locator'] is not None:
            errors = p['source_locator'].closest(positions(np.r_[p['source_q'], p['interior_q']], c))[1]
            position_error = float(errors[:len(p['source_q'])].max(initial=0.))
            interior_error = float(errors[len(p['source_q']):].max(initial=0.))
        if not (boundary_error < boundary_limit and max(interior_error, position_error) < distance):
            return None
        if p.get('old_height') is not old['height']:
            p['old_height']=old['height']; p['old_normal_measurements']=normal_measurements(di,old)
        for check,previous,measured in zip(p['normal_checks'],p['old_normal_measurements'],normal_measurements(di,c)):
            reference=check[-1]; slack=config.normal_slack_degrees
            # A support already outside the fixed envelope may recover in
            # several steps. Its existing excess may never grow; no new
            # allowance is added to the current iterate.
            if any(measured[key]>max(reference[key]+factor*slack,previous[key])+1e-10
                   for key,factor in (('rms',1),('p95',1),('max',4))):
                return None
        c.update(continuous_boundary_error_mm=boundary_error, interior_probe_max_mm=interior_error,
                 coupled_normal_version=2, sampled_source_max_mm=position_error)
        if config.physical_distance:
            c['interior_probe_metric'] = 'finite_source_euclidean'
        return c
    print(dict(stage='coupled_supports', charts=len(selected), variables=total, seams=len(seams), before=before), flush=True)
    for iteration in range(config.iterations):
        ops = []; rhs = []
        for seam_index,(pair, xyz, weights) in enumerate(seams):
            ns = []; js = []; ss = []; sjs = []
            for di in pair:
                n, s, j, sj = jets(current[di], xyz, True)
                ns.append(n); js.append(j); ss.append(s); sjs.append(sj)
            for axis in range(3):
                op = sparse.csr_matrix((len(xyz), total)); target = -(ns[0][:, axis]-ns[1][:, axis])
                for side, di in enumerate(pair):
                    if di in offsets:
                        sign = 1 if side == 0 else -1
                        op += embed(js[side][axis], offsets[di], total)*sign
                        target += sign*(js[side][axis]@current[di]['height'])
                w = np.sqrt(config.seam_weight*weights)
                ops.append(op.multiply(w[:, None])); rhs.append(target*w)
            if config.boundary_source_weight:
                for axis in range(3):
                    op=sparse.csr_matrix((len(xyz),total))
                    target=seam_targets[seam_index][:,axis]-(ns[0][:,axis]+ns[1][:,axis])/2
                    for side,di in enumerate(pair):
                        if di in offsets:
                            op+=embed(js[side][axis],offsets[di],total)/2
                            target+=(js[side][axis]@current[di]['height'])/2
                    w=np.sqrt(config.boundary_source_weight*weights)
                    ops.append(op.multiply(w[:,None]));rhs.append(target*w)
            if config.curvature_weight:
                for component, (a, b) in enumerate(COMPONENTS):
                    op = sparse.csr_matrix((len(xyz), total)); target = -(ss[0][:, a, b]-ss[1][:, a, b])
                    for side, di in enumerate(pair):
                        if di in offsets:
                            sign = 1 if side == 0 else -1
                            op += embed(sjs[side][component], offsets[di], total)*sign
                            target += sign*(sjs[side][component]@current[di]['height'])
                    w = np.sqrt(config.curvature_weight*weights*(1 if a == b else 2))
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
        initial_status = result.info.status; initial_seconds = result.info.run_time
        retry = config.solver_retry and result.info.status_val in (3, 4, 5, 6)
        if retry:
            # Re-express the identical QP as a displacement from the current
            # support and normalize constraint rows. Tiny spline spans can
            # otherwise trigger a numerical infeasibility certificate long
            # before the physical constraints have been resolved.
            current_scaled = warm*diagonal; ac = (constraint@inv).tocsc()
            row_norm = np.sqrt(np.asarray(ac.power(2).sum(axis=1)).ravel())
            row_scale = 1/np.maximum(row_norm, 1e-12)
            at_current = constraint@warm
            alternate = osqp.OSQP()
            alternate.setup(P=sparse.triu(scaled, format='csc'), q=scaled@current_scaled+linear/diagonal,
                            A=ac.multiply(row_scale[:, None]).tocsc(),
                            l=(lower-at_current)*row_scale, u=(upper-at_current)*row_scale,
                            eps_abs=1e-7, eps_rel=1e-7, eps_prim_inf=1e-9, eps_dual_inf=1e-9,
                            max_iter=16000, time_limit=config.solver_seconds, polishing=True,
                            adaptive_rho_interval=50, verbose=False)
            alternate.warm_start(x=np.zeros(total)); result = alternate.solve(raise_error=False)
            if result.x is not None:
                result.x = current_scaled+result.x
        if result.x is None:
            history.append(dict(iteration=iteration, solver_status=result.info.status,
                                initial_solver_status=initial_status, equilibrated_delta_retry=retry, accepted=False))
            break
        coeff = result.x/diagonal; val = constraint@coeff
        violation = float(max(np.max(lower-val), np.max(val-upper), 0.))
        event = dict(iteration=iteration, solver_status=result.info.status, solver_seconds=result.info.run_time,
                     initial_solver_status=initial_status, equilibrated_delta_retry=retry,
                     total_solver_seconds=result.info.run_time+(initial_seconds if retry else 0.),
                     constraint_violation_mm=violation); history.append(event)
        np.savez_compressed(out/f'iteration_{iteration:02d}.npz', height=coeff, charts=np.array(sorted(selected)))
        # The linear system reserves a margin inside the unchanged geometric
        # budgets. An inexact iterate may use that margin; actual bounds below
        # still decide acceptance, and solver convergence is reported verbatim.
        if result.info.status_val not in (1, 2, 7, 8) or not np.isfinite(coeff).all() or violation > distance*.1:
            break
        candidate = dict(current); rejected = []; accepted_fractions = {}
        for group in groups.values():
            proposal = np.mean([coeff[offsets[di]:offsets[di]+len(current[di]['height'])] for di in group], axis=0)
            chosen = None
            for alpha in (1., .5, .25, .125)[:config.step_trials]:
                trial = {}
                for di in group:
                    old = current[di]
                    c = constrained_record(di, old, old['height']+alpha*(proposal-old['height']))
                    if c is None:
                        break
                    trial[di] = c
                if len(trial) == len(group):
                    chosen = trial
                    for di in group:
                        accepted_fractions[str(di)] = alpha
                    break
            if chosen is None:
                rejected.extend(group)
                continue
            candidate.update(chosen)
        old_cost = seam_cost(current, seams,targets)
        admitted = None; coupled_trials = []
        for fraction in (1., .5, .25, .125, .0625)[:config.step_trials]:
            trial = dict(candidate) if fraction == 1 else dict(current)
            if fraction != 1:
                valid = True
                for di in accepted_fractions:
                    di = int(di); old = current[di]
                    c = constrained_record(di, old, old['height']+fraction*(candidate[di]['height']-old['height']))
                    if c is None:
                        valid = False
                        break
                    trial[di] = c
                if not valid:
                    continue
            after = seam_cost(trial, seams,targets)
            curvature_regression = (config.curvature_weight > 0 and
                (after['curvature_p95_per_mm'] > old_cost['curvature_p95_per_mm']*1.02+.01 or
                 after['curvature_max_per_mm'] > old_cost['curvature_max_per_mm']*1.05+.1))
            if config.protect_curvature_energy:
                curvature_regression |= after['curvature_difference_energy'] > old_cost['curvature_difference_energy']*1.02+1e-10
            passed = (seam_objective(after,config) < seam_objective(old_cost,config)*.999 and not curvature_regression and
                      after['normal_p95_degrees'] <= old_cost['normal_p95_degrees']+.25 and
                      after['normal_max_degrees'] <= old_cost['normal_max_degrees']+2)
            coupled_trials.append(dict(fraction=fraction, accepted=passed, after=after))
            if passed:
                admitted = trial
                accepted_fractions = {di:alpha*fraction for di,alpha in accepted_fractions.items()}
                break
        event.update(rejected_supports=rejected, accepted_fractions=accepted_fractions,
                     coupled_line_search=coupled_trials, after=after)
        if admitted is None:
            event['accepted'] = False; break
        current = admitted; accepted = True; event['accepted'] = True
        print(dict(stage='coupled_support_iteration', iteration=iteration,
                   solver_status=event['solver_status'], accepted_supports=len(accepted_fractions),
                   rejected_supports=len(rejected), after=after), flush=True)
        dump(out/'seam_fairing.json', dict(before=before, history=history))
    changed = [di for di in selected if not np.array_equal(current[di]['height'], records[di]['height'])]
    report = dict(accepted=accepted, before=before, after=seam_cost(current, seams,targets), history=history,
                  changed_charts=sorted(map(int, changed)), regions=sorted({int(records[i]['region']) for i in selected}), config=asdict(config),
                  seconds=time.perf_counter()-started, shared_curves_unchanged=True,
                  continuity_scope='Sampled unit-normal and physical shape-operator agreement, not an exact G1/G2 certificate', source_CAD_read=False,
                  seam_sampling='Five collocation points per interval split by both support knot crossings and source/curve parameters',
                  source_fit=str(root.resolve()), commercial_engine_used=False)
    dump(out/'seam_fairing.json', report)
    if not accepted:
        return report
    if context is not None:
        context['records'] = current
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
