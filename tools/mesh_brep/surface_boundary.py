"""Fair smooth-source interfaces inside a certified tube of the original mesh.

The planes used by sequential quadratic programming move to the closest source
triangles. The acceptance test uses distance to the triangles themselves, with
a Lipschitz bound between samples; passing the linearized planes is insufficient.
"""
import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline
from .splines import knots, sparse_basis
from .mesh import Locator
from .regional_rectangle import curve_polyline_error


def certify_tube(poles, knot, locator, limit, max_refinements=14):
    spline = BSpline(knot, poles, 3)
    derivative = spline.derivative()
    speed_bound = float(np.linalg.norm(derivative.c[:len(derivative.t)-derivative.k-1], axis=1).max())
    cuts = np.unique(knot)
    left, right = cuts[:-1], cuts[1:]
    largest = 0.; sampled = 0.; count = 0
    for _ in range(max_refinements):
        middle = (left+right)/2
        q = np.r_[left, middle, right]
        _, distance, _, _ = locator.closest(spline(q)); count += len(q)
        if distance.max(initial=0.) > limit:
            return dict(passed=False, sampled_max_mm=float(distance.max()), samples=count,
                        violating_parameters=q[distance > limit])
        sampled = max(sampled, float(distance.max(initial=0.)))
        # Every parameter is within one quarter interval of these three samples.
        upper = distance.reshape(3, -1).max(axis=0)+speed_bound*(right-left)/4
        accepted = upper <= limit
        largest = max(largest, float(upper[accepted].max(initial=0.)))
        if accepted.all():
            return dict(passed=True, certified_max_mm=largest, sampled_max_mm=sampled, samples=count,
                        violating_parameters=np.array([]))
        left, middle, right = left[~accepted], middle[~accepted], right[~accepted]
        left, right = np.r_[left, middle], np.r_[middle, right]
    return dict(passed=False, sampled_max_mm=sampled, samples=count,
                reason='distance_certificate_refinement_limit', violating_parameters=(left+right)/2)


def fair_curve(points, parameters, budget, vertices, triangles, tube_limit, initial_poles, initial_knot,
               control_counts=(6, 10, 18, 34, 66), iterations=10, endpoint_normals=None,
               triangle_groups=None):
    import osqp
    groups = [np.arange(len(triangles))] if triangle_groups is None else triangle_groups
    locators = [Locator(vertices, triangles[ids]) for ids in groups]
    normals = np.cross(vertices[triangles[:,1]]-vertices[triangles[:,0]], vertices[triangles[:,2]]-vertices[triangles[:,0]])
    normals /= np.linalg.norm(normals, axis=1)[:,None]
    origin = points.mean(axis=0); scale = max(float(np.linalg.norm(np.ptp(points, axis=0))), 1e-8)
    seed = BSpline(initial_knot, initial_poles, 3)
    ends = points[[0,-1]]
    history = []
    for count in control_counts:
        knot = knots(count)
        q = np.unique(np.r_[parameters, np.linspace(0,1,max(65,4*count)), knot])
        current = seed(q)
        nodes, weights = np.polynomial.legendre.leggauss(3)
        cuts = np.unique(knot); widths = np.diff(cuts)
        g = (cuts[:-1,None]+widths[:,None]*(nodes+1)/2).ravel()
        d2 = sparse_basis(knot,g,2).multiply(np.sqrt((widths[:,None]*weights/2).ravel())[:,None])
        previous = None
        for iteration in range(iterations):
            b = sparse_basis(knot,q)
            target = (np.column_stack([np.interp(q,parameters,points[:,k]) for k in range(3)])-origin)/scale
            h = (d2.T@d2+b.T@b*(.01/len(q))+sparse.eye(count)*1e-9).tocsc()
            cost_scale = 10/max(float(h.diagonal().max()),1.)
            objective = sparse.block_diag([h]*3,format='csc')*cost_scale
            linear = np.concatenate([-(b.T@target[:,k])*.01/len(q) for k in range(3)])*cost_scale
            blocks = [sparse.block_diag([b]*3,format='csr')]
            lower = [target.T.ravel()-budget/(np.sqrt(3)*scale)]
            upper = [target.T.ravel()+budget/(np.sqrt(3)*scale)]
            for locator, group in zip(locators, groups):
                nearest, distance, ids, _ = locator.closest(current)
                nn = normals[group[ids]]
                blocks.append(sparse.hstack([b.multiply(nn[:,k,None]) for k in range(3)],format='csr'))
                z = np.sum(nn*(nearest-origin)/scale,axis=1)
                lower.append(z-tube_limit*.8/scale); upper.append(z+tube_limit*.8/scale)
                # Closest points on a domain border have a tangential residual.
                # A triangle-plane constraint alone cannot bound that residual.
                active = distance > tube_limit*.1
                if np.any(active):
                    direction = (current[active]-nearest[active])/distance[active,None]
                    op = b[active]
                    blocks.append(sparse.hstack([op.multiply(direction[:,k,None]) for k in range(3)],format='csr'))
                    z = np.sum(direction*(nearest[active]-origin)/scale,axis=1)
                    lower.append(np.full(len(z),-np.inf)); upper.append(z+tube_limit*.8/scale)
            endpoint = sparse_basis(knot,np.array([0.,1.]))
            blocks.append(sparse.block_diag([endpoint]*3,format='csr'))
            lower.append(((ends-origin)/scale).T.ravel()); upper.append(lower[-1])
            derivative=sparse_basis(knot,np.array([0.,1.]),1)
            if endpoint_normals is not None:
                blocks.append(sparse.hstack([derivative.multiply(endpoint_normals[:,k,None]) for k in range(3)],format='csr'))
                lower.append(np.zeros(2)); upper.append(np.zeros(2))
            direction=points[[1,-1]]-points[[0,-2]]
            direction/=np.linalg.norm(direction,axis=1)[:,None]
            blocks.append(sparse.hstack([derivative.multiply(direction[:,k,None]) for k in range(3)],format='csr'))
            speed=np.linalg.norm(np.diff(points,axis=0),axis=1).sum()/scale
            lower.append(np.full(2,.01*speed)); upper.append(np.full(2,np.inf))
            solver = osqp.OSQP()
            solver.setup(P=objective,q=linear,A=sparse.vstack(blocks,format='csc'),
                         l=np.concatenate(lower),u=np.concatenate(upper),verbose=False,
                         eps_abs=2e-7,eps_rel=2e-7,max_iter=15000,time_limit=3.,polishing=True)
            result=solver.solve(raise_error=False)
            item=dict(controls=count,iteration=iteration,status=result.info.status); history.append(item)
            if result.info.status_val not in (1,2): break
            cp=result.x.reshape(3,count).T*scale+origin; cp[[0,-1]]=ends
            spline=BSpline(knot,cp,3); current=spline(q)
            distance = max(float(locator.closest(current)[1].max()) for locator in locators)
            item['sampled_distance_mm']=distance
            if iteration >= 1 and distance <= tube_limit*.95:
                movement,_=curve_polyline_error(cp,knot,points,parameters)
                checks=[certify_tube(cp,knot,locator,tube_limit) for locator in locators]
                if movement <= budget and all(c['passed'] for c in checks):
                    for check in checks: check.pop('violating_parameters')
                    check=dict(passed=True, certified_max_mm=max(c['certified_max_mm'] for c in checks),
                               domains=checks, samples=sum(c['samples'] for c in checks))
                    return cp,knot,dict(accepted=True,method='source_surface_tube',movement_mm=movement,
                                        source_tube_limit_mm=tube_limit,certificate=check,history=history,
                                        constrained_source_domains=len(groups))
            if previous is not None and np.max(np.linalg.norm(current-previous,axis=1)) < 1e-8: break
            previous=current.copy()
    return None,None,dict(accepted=False,history=history)


def _proposal(task):
    ci, rec, vertices, triangles, normals, points, budget, limit, endpoint_normals, groups = task
    cp, knot, info = fair_curve(points, rec['parameters'], budget, vertices, triangles, limit,
                               rec['poles'], rec['knots'], endpoint_normals=endpoint_normals,
                               triangle_groups=groups)
    info['curve'] = ci
    if not info['accepted']: return ci, None, info
    def energy(p, t):
        cuts=np.unique(t); nodes, weights=np.polynomial.legendre.leggauss(3)
        q=(cuts[:-1,None]+np.diff(cuts)[:,None]*(nodes+1)/2).ravel()
        w=(np.diff(cuts)[:,None]*weights/2).ravel()
        return float(w@np.sum(BSpline(t,p,3)(q,2)**2,axis=1))
    old, new = energy(rec['poles'],rec['knots']), energy(cp,knot)
    info.update(bending_before=old,bending_after=new)
    from .skeleton_graph import normal_error
    tangent_improvement=False
    if endpoint_normals is not None:
        def angles(p,t):
            derivative=BSpline(t,p,3)([0.,1.],1)
            sine=abs(np.sum(derivative*endpoint_normals,axis=1))/np.linalg.norm(derivative,axis=1)
            return np.degrees(np.arcsin(np.clip(sine,0,1)))
        before,after=angles(rec['poles'],rec['knots']),angles(cp,knot)
        info.update(endpoint_plane_error_before_degrees=before.tolist(),endpoint_plane_error_after_degrees=after.tolist())
        tangent_improvement=before.max()>1. and after.max()<1e-4 and new<=old*1.25
    if new >= old*.99 and not tangent_improvement:
        info.update(accepted=False,reason='no_bending_improvement'); return ci,None,info
    from .boundary_quality import measures
    before = measures(rec); after = measures(dict(poles=cp,knots=knot))
    info.update(geometry_before=before,geometry_after=after)
    if (after['total_tangent_turn_degrees'] > before['total_tangent_turn_degrees']*1.01+.1
        or after['curvature_squared_integral_per_mm'] > before['curvature_squared_integral_per_mm']*1.01+1e-4):
        info.update(accepted=False,reason='geometric_fairness_regression'); return ci,None,info
    candidate = dict(rec, poles=cp,knots=knot,movement_mm=info['movement_mm'],method=info['method'],
                     source_plane_error_mm=normal_error(cp,knot,points,rec['parameters'],normals)[0],
                     source_tube_certified_max_mm=info['certificate']['certified_max_mm'],
                     source_tube_certificate_version=1, source_tube_limit_mm=limit,
                     source_tube_domains=info['constrained_source_domains'])
    # Keep an exact rollback for the tested network. A failed new curve must
    # not force its already valid neighbors back to the source staircase.
    for field in ('poles','knots','movement_mm','method','source_plane_error_mm','movement_budget_mm',
                  'endpoint_tangent_locked','source_tube_certified_max_mm','source_tube_certificate_version',
                  'source_tube_limit_mm','source_tube_domains'):
        if field in rec: candidate['rollback_'+field] = rec[field]
    if endpoint_normals is not None:
        candidate['source_tangent_planes']=endpoint_normals
    return ci,candidate,info


def optimize(source, output, regions=None, workers=4, tangent_planes=False, tube_limit=None,
             include_creases=False, separate_domains=False):
    import json
    import shutil
    import time
    from pathlib import Path
    from concurrent.futures import ProcessPoolExecutor
    from .partition_fit import incidence
    from .mesh import dump
    root,out=Path(source),Path(output); out.mkdir(parents=True,exist_ok=False)
    started=time.perf_counter(); meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
    tube_limit = meta['source_plane_budget_mm'] if tube_limit is None else float(tube_limit)
    if not np.isfinite(tube_limit) or not 0 < tube_limit < meta['config']['distance']:
        raise ValueError('The source tube must be positive and below the final source-distance budget')
    data=np.load(meta['partition']); v,f,labels=[data[k] for k in ('mesh_vertex_original','mesh_triangle','face_instance_id')]
    assignment=np.load(root/'chart_index.npz')['face_chart']
    region_ids=set(regions if regions is not None else meta.get('partial_regions') or np.unique(labels))
    selected=set(np.unique(assignment[np.isin(labels,list(region_ids))]).tolist())
    edges,owners=incidence(f); keys=edges[:,0]*len(v)+edges[:,1]
    fn=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]]); fn/=np.linalg.norm(fn,axis=1)[:,None]
    corner=None
    if tangent_planes:
        from .source_normals import crease_fan_normals
        corner=crease_fan_normals(v,f,meta['config']['feature_angle'],(edges,owners),meta['normal_filter_steps'])
    records={int(p.stem):dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))}
    tasks=[]; skipped=[]
    for ci,rec in records.items():
        if not selected.intersection(map(int,rec['owners'])): continue
        ch=rec['chain']
        if len(ch)<4:
            skipped.append(dict(curve=ci,reason='short_source_chain')); continue
        kk=np.minimum(ch[:-1],ch[1:])*len(v)+np.maximum(ch[:-1],ch[1:])
        nn=fn[owners[np.searchsorted(keys,kk)]]
        angle=np.degrees(np.arccos(np.clip(np.sum(nn[:,0]*nn[:,1],axis=1),-1,1)))
        if angle.max()>meta['config']['feature_angle'] and not include_creases:
            skipped.append(dict(curve=ci,reason='source_crease',max_angle=float(angle.max()))); continue
        ids=np.flatnonzero(np.isin(assignment,rec['owners'])); used=np.unique(f[ids]); tri=np.searchsorted(used,f[ids])
        endpoint=None
        if corner is not None:
            ff=owners[np.searchsorted(keys,kk[[0,-1]]),0]
            endpoint=corner[ff,np.argmax(f[ff]==ch[[0,-1],None],axis=1)]
        # Separate tubes are an explicit diagnostic: they preserve both old
        # trimmed domains, but can also lock a rasterized artificial interface.
        groups = [np.flatnonzero(assignment[ids] == owner) for owner in rec['owners']] if separate_domains else None
        tasks.append((ci,rec,v[used],tri,nn,v[ch],min(meta['smoothing_mm'],float(rec.get('movement_budget_mm',meta['smoothing_mm']))),tube_limit,endpoint,groups))
    attempts=[]; changed=[]
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for ci,candidate,info in pool.map(_proposal,tasks):
            attempts.append(info)
            if candidate is not None: records[ci]=candidate; changed.append(ci)
            if len(attempts)%25==0 or len(attempts)==len(tasks):
                print(dict(done=len(attempts),total=len(tasks),accepted=len(changed)),flush=True)
                dump(out/'proposals.json',attempts)
    for name in ('charts','curves'): shutil.copytree(root/name,out/name)
    for name in ('chart_index.npz','fitting.json'): shutil.copy2(root/name,out/name)
    for ci in changed: np.savez_compressed(out/'curves'/f'{ci:05d}.npz',**records[ci])
    report=dict(changed_curves=changed,attempted=len(attempts),skipped=skipped,seconds=time.perf_counter()-started,
                source_fit=str(root.resolve()),source_CAD_read=False,commercial_engine_used=False,
                source_tube_limit_mm=tube_limit,sharp_source_interfaces_preserved=not include_creases,
                both_incident_source_domains_constrained=separate_domains,
                source_distance_scope='separate incident domains' if separate_domains else 'union of incident domains',
                common_source_tangent_planes=tangent_planes,
                continuous_topology_and_support_refit_required=True)
    dump(out/'boundary_optimization.json',report)
    # A changed curve invalidates both incident supports, including neighbors
    # outside the requested regions. Preserve that complete refit requirement.
    affected = np.unique(np.concatenate([records[ci]['owners'] for ci in changed])) if changed else []
    meta['required_refit_regions'] = np.unique(labels[np.isin(assignment,affected)]).tolist()
    meta.update(status='boundary_candidate',source_fit=str(out.resolve()),requires_support_refit=True,
                boundary_optimization=report)
    dump(out/'run.json',meta)
    return report


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(); p.add_argument('--run',required=True); p.add_argument('--output',required=True)
    p.add_argument('--regions',type=int,nargs='+'); p.add_argument('--workers',type=int,default=4)
    p.add_argument('--tangent-planes',action='store_true')
    p.add_argument('--tube-limit',type=float)
    p.add_argument('--include-creases',action='store_true')
    p.add_argument('--separate-domains',action='store_true')
    a=p.parse_args(); optimize(a.run,a.output,a.regions,a.workers,a.tangent_planes,a.tube_limit,a.include_creases,a.separate_domains)
