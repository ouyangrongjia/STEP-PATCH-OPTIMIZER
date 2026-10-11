"""Recover shared-support identity and automatically revisit regressed seams.

The reference is a previous native fit of the same source partitions. No CAD
geometry is read. Geometry budgets remain unchanged; failed common supports
feed back to network construction before bounded local continuity correction.
"""
from dataclasses import asdict
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .shared_support import restore_groups
from .network_support import refit
from .global_continuity import optimize as fair
from .seam_fairing import Config
from .surface_jets import audit
from .partition_fit import spline_graph_error


GEOMETRY = ('origin', 'axes', 'lo', 'size', 'knots', 'height')


def reuse_native_supports(target, source, protected=()):
    """Warm-start compatible supports, keeping old common surfaces atomic."""
    root, old = Path(target), Path(source); protected=set(protected)
    records={int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    previous={int(p.stem):dict(np.load(p)) for p in (old/'charts').glob('*.npz')}
    curves=[dict(np.load(p)) for p in (root/'curves').glob('*.npz')]
    meta=json.loads((root/'run.json').read_text(encoding='utf-8')); restore_groups(records)
    groups={tuple(map(int,r.get('shared_support_group',[di]))) for di,r in records.items()}
    reused=[]
    for group in sorted(groups):
        if protected.intersection(group): continue
        if any(any(not np.array_equal(records[i][k],previous[i][k]) for k in ('region','triangle_ids','vertex_ids')) for i in group):
            raise ValueError('Native warm start changed source ownership')
        if any(any(not np.array_equal(previous[i][k],previous[group[0]][k]) for k in GEOMETRY) for i in group):
            continue
        try:
            error=max(spline_graph_error(c['poles'],c['knots'],previous[int(i)])
                      for c in curves for i in c['owners'] if i in group)
        except ValueError:
            continue
        if not np.isfinite(error) or error >= meta['config']['boundary']*.4: continue
        for i in group:
            rec=dict(previous[i])
            if len(group)>1: rec['shared_support_group']=np.array(group)
            np.savez_compressed(root/'charts'/f'{i:05d}.npz',**rec); reused.append(i)
    meta['native_warm_start']=dict(source_fit=str(old.resolve()),charts=reused,protected_charts=sorted(protected))
    dump(root/'run.json',meta)
    return reused


def regressions(before, after, allowance_degrees=2.):
    """Screen all paired seams, retaining short interfaces and endpoints."""
    old = {r['curve']:r for r in before['per_curve']}; rows = []
    for row in after['per_curve']:
        reference = old[row['curve']]
        if 'normal_degrees' not in row or 'normal_degrees' not in reference:
            continue
        excess = row['normal_degrees']['p95']-reference['normal_degrees']['p95']-allowance_degrees
        if excess > 0:
            rows.append(dict(curve=row['curve'], charts=row['charts'], regions=row['regions'],
                             before_p95_degrees=reference['normal_degrees']['p95'],
                             after_p95_degrees=row['normal_degrees']['p95'], excess_degrees=excess,
                             length_mm=row['smooth_length_mm']))
    return sorted(rows, key=lambda r:-r['excess_degrees'])


def curvature_regressions(before, after, absolute_allowance=.5, relative_allowance=.25):
    """Screen curvature separately: improved normals can hide worsened jets."""
    old={r['curve']:r for r in before['per_curve']}; rows=[]
    for row in after['per_curve']:
        reference=old[row['curve']]
        if 'shape_jump_per_mm' not in row or 'shape_jump_per_mm' not in reference:
            continue
        first=reference['shape_jump_per_mm']['p95']; last=row['shape_jump_per_mm']['p95']
        allowance=max(absolute_allowance,relative_allowance*first)
        if last-first > allowance:
            rows.append(dict(curve=row['curve'],charts=row['charts'],regions=row['regions'],
                before_p95_per_mm=first,after_p95_per_mm=last,allowance_per_mm=allowance,
                excess_per_mm=last-first-allowance,length_mm=row['smooth_length_mm']))
    return sorted(rows,key=lambda r:-r['excess_per_mm'])


def rollback_regressions(source, candidate, before, after, output, *, rounds=3, resume=False):
    """Restore newly worsened support neighborhoods, retaining safe updates.

    Both fits already satisfy the same fixed-curve geometry constraints. A
    rollback copies whole original support groups and never interpolates them.
    The assembled mixture is audited again, including neighboring interfaces.
    """
    root,current,out=Path(source),Path(candidate),Path(output)
    out.mkdir(parents=True,exist_ok=resume)
    originals={int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    records={int(p.stem):dict(np.load(p)) for p in (current/'charts').glob('*.npz')}
    if set(originals)!=set(records) or any(any(not np.array_equal(originals[i][k],records[i][k])
            for k in ('region','triangle_ids','vertex_ids')) for i in originals):
        raise ValueError('Quality rollback changed source ownership')
    for group in ('curves','routes'):
        first={p.name:p for p in (root/group).glob('*.npz')}; last={p.name:p for p in (current/group).glob('*.npz')}
        if set(first)!=set(last): raise ValueError('Quality rollback requires fixed shared curves')
        for name in first:
            with np.load(first[name]) as a,np.load(last[name]) as b:
                if set(a.files)!=set(b.files) or any(not np.array_equal(a[k],b[k]) for k in a.files):
                    raise ValueError('Quality rollback requires identical shared curves')
    restore_groups(originals)
    reference={r['curve']:r for r in before['per_curve']}; history=[]; measured=after
    policy=dict(normal_p95_allowance_degrees=.25,normal_max_allowance_degrees=2.,
                curvature_p95_absolute_per_mm=.1,curvature_p95_relative=.05,
                curvature_max_absolute_per_mm=1.,curvature_max_relative=.1)
    for attempt in range(rounds):
        bad=[]
        for row in measured['per_curve']:
            old=reference[row['curve']]
            if 'normal_degrees' not in row: continue
            an,bn=old['normal_degrees'],row['normal_degrees']; ac,bc=old['shape_jump_per_mm'],row['shape_jump_per_mm']
            if (bn['p95']>an['p95']+policy['normal_p95_allowance_degrees']
                    or bn['max']>an['max']+policy['normal_max_allowance_degrees']
                    or bc['p95']>ac['p95']+max(policy['curvature_p95_absolute_per_mm'],ac['p95']*policy['curvature_p95_relative'])
                    or bc['max']>ac['max']+max(policy['curvature_max_absolute_per_mm'],ac['max']*policy['curvature_max_relative'])):
                bad.append(row)
        selected={i for row in bad for i in row['charts']
                  if any(not np.array_equal(records[i][k],originals[i][k]) for k in GEOMETRY)}
        selected.update(i for di in list(selected) for i in originals[di].get('shared_support_group',[]))
        if not selected: break
        destination=out/f'round_{attempt:02d}'; event=dict(attempt=attempt,restored_charts=sorted(map(int,selected)),
            triggering_curves=[r['curve'] for r in bad],source_fit=str(root.resolve()),prior_candidate=str(current.resolve()))
        if destination.exists():
            cached=json.loads((destination/'run.json').read_text(encoding='utf-8'))
            if cached.get('quality_rollback')!=event: raise ValueError('Rollback checkpoint has different neighborhoods')
        else:
            destination.mkdir()
            for group in ('charts','curves','routes'):
                if (current/group).exists(): shutil.copytree(current/group,destination/group)
            for name in ('chart_index.npz','fitting.json'): shutil.copy2(current/name,destination/name)
            for di in selected: np.savez_compressed(destination/'charts'/f'{di:05d}.npz',**originals[di])
            meta=json.loads((current/'run.json').read_text(encoding='utf-8'))
            meta.update(source_fit=str(destination.resolve()),quality_rollback=event)
            dump(destination/'run.json',meta)
        audit_path=out/f'round_{attempt:02d}.json'
        measured=json.loads(audit_path.read_text(encoding='utf-8')) if audit_path.exists() else audit(destination,audit_path,samples=129)
        records.update({di:dict(originals[di]) for di in selected}); current=destination
        history.append(event); dump(out/'rollback.json',dict(policy=policy,history=history,surface_fit=str(current.resolve())))
    return current,measured,dict(policy=policy,history=history,surface_fit=str(current.resolve()))


def optimize_curvature(baseline, source, output, *, workers=4, rounds=1, resume=False,
                       baseline_report=None, initial_report=None):
    """Bounded curvature recovery after normal/common-support recovery."""
    root,reference,out=Path(source),Path(baseline),Path(output)
    out.mkdir(parents=True,exist_ok=resume); start=time.perf_counter()
    config=Config(normal_weight=.3,seam_weight=2.,curvature_weight=.02,variation_weight=1e-6,
                  iterations=3,solver_seconds=60.,normal_slack_degrees=.5,boundary_source_weight=2.)
    report=dict(status='running',input_fit=str(root.resolve()),baseline_fit=str(reference.resolve()),
                config=asdict(config),rounds=rounds,history=[],can_apply=False,
                absolute_curvature_allowance_per_mm=.5,relative_curvature_allowance=.25,
                normal_excess_relative_allowance=.02,normal_global_p95_allowance_degrees=.1,
                normal_global_max_allowance_degrees=2.,local_rollback_rounds=3)
    state_path=out/'curvature_guard.json'
    if resume and state_path.exists():
        previous=json.loads(state_path.read_text(encoding='utf-8'))
        if any(previous[k]!=report[k] for k in report if k not in ('status','history')):
            raise ValueError('Curvature recovery checkpoint has different inputs or settings')
        if previous['status']=='DiagnosticOnly': return previous
    dump(state_path,report)
    def measured(folder,path,supplied=None):
        if path.exists(): return json.loads(path.read_text(encoding='utf-8'))
        if supplied is None: return audit(folder,path,samples=129)
        result=json.loads(Path(supplied).read_text(encoding='utf-8'))
        if result['samples_per_curve']!=129 or 'both support knot crossings' not in result['sampling']:
            raise ValueError('Incompatible supplied continuity sampling protocol')
        dump(path,result); return result
    try:
        before=measured(reference,out/'baseline.json',baseline_report)
        current=root; measured_current=measured(root,out/'initial.json',initial_report)
        report['initial_regressions']=curvature_regressions(before,measured_current)
        dump(state_path,report)
        cost=lambda rows:sum(r['length_mm']*(r['excess_per_mm']/r['allowance_per_mm'])**2 for r in rows)
        normal_cost=lambda measured:sum(r['length_mm']*r['excess_degrees']**2 for r in regressions(before,measured))
        for iteration in range(rounds):
            bad=curvature_regressions(before,measured_current)
            if not bad: break
            selected=sorted({i for row in bad for i in row['charts']}); destination=out/f'correction_{iteration:02d}'
            saved=destination/'global_continuity.json'
            if saved.exists():
                step=json.loads(saved.read_text(encoding='utf-8'))
                if Path(step['source_fit']).resolve()!=current.resolve() or step.get('selected_charts')!=selected:
                    raise ValueError('Cached curvature correction has different neighborhoods')
            else:
                fair(current,destination,config=config,sweeps=1,workers=workers,
                     selected_charts=selected,resume=destination.exists(),reference_fit=reference)
            after=measured(destination,out/f'correction_{iteration:02d}.json')
            remaining=curvature_regressions(before,after)
            normal_before=normal_cost(measured_current); normal_after=normal_cost(after)
            an,bn=(r['smooth']['normal_degrees'] for r in (measured_current,after))
            preserves_normals=(normal_after<=normal_before*(1+report['normal_excess_relative_allowance'])+1e-10
                and bn['p95']<=an['p95']+report['normal_global_p95_allowance_degrees']
                and bn['max']<=an['max']+report['normal_global_max_allowance_degrees'])
            accepted=cost(remaining)<cost(bad) and preserves_normals
            rollback=None; rejected_full=None
            if not accepted:
                rejected_full=dict(curvature_excess_energy=cost(remaining),normal_excess_energy=normal_after,
                                   preserves_normals=preserves_normals)
                destination,after,rollback=rollback_regressions(current,destination,measured_current,after,
                    out/f'rollback_{iteration:02d}',rounds=report['local_rollback_rounds'],
                    resume=resume and (out/f'rollback_{iteration:02d}').exists())
                remaining=curvature_regressions(before,after); normal_after=normal_cost(after)
                bn=after['smooth']['normal_degrees']
                preserves_normals=(normal_after<=normal_before*(1+report['normal_excess_relative_allowance'])+1e-10
                    and bn['p95']<=an['p95']+report['normal_global_p95_allowance_degrees']
                    and bn['max']<=an['max']+report['normal_global_max_allowance_degrees'])
                accepted=cost(remaining)<cost(bad) and preserves_normals
            report['history'].append(dict(iteration=iteration,selected_charts=selected,accepted=accepted,
                before_regressions=bad,after_regressions=remaining,before_excess_energy=cost(bad),
                after_excess_energy=cost(remaining),normal_excess_before=normal_before,
                normal_excess_after=normal_after,preserves_normals=preserves_normals,output=str(destination.resolve()),
                rejected_full_proposal=rejected_full,local_rollback=rollback))
            dump(state_path,report)
            if not accepted: break
            current=destination; measured_current=after
        report.update(status='DiagnosticOnly',surface_fit=str(current.resolve()),
            remaining_regressions=curvature_regressions(before,measured_current),shared_curves_unchanged=True,
            scope='Curvature regression screening and bounded recovery, not a G2 acceptance tolerance or certificate.')
        dump(out/'final.json',measured_current)
    except (Exception,KeyboardInterrupt) as error:
        report.update(status='interrupted' if isinstance(error,KeyboardInterrupt) else 'failed',error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['seconds']=time.perf_counter()-start; dump(state_path,report)
    return report


def optimize(baseline, source, output, *, workers=4, rounds=2, resume=False,
             baseline_report=None, initial_report=None):
    root, reference, out = Path(source), Path(baseline), Path(output)
    out.mkdir(parents=True, exist_ok=resume); start=time.perf_counter()
    config = Config(normal_weight=.3,seam_weight=2.,curvature_weight=.005,variation_weight=1e-6,
                    iterations=3,solver_seconds=60.,normal_slack_degrees=.5,boundary_source_weight=2.)
    report = dict(status='running', input_fit=str(root.resolve()), baseline_fit=str(reference.resolve()),
                  config=asdict(config), rounds=rounds, normal_regression_allowance_degrees=2.,
                  can_apply=False, history=[], source_CAD_read=False, commercial_engine_used=False)
    state_path=out/'continuity_guard.json'
    if resume and state_path.exists():
        previous=json.loads(state_path.read_text(encoding='utf-8'))
        if any(previous[k] != report[k] for k in ('input_fit','baseline_fit','config','rounds')):
            raise ValueError('Continuity recovery checkpoint has different inputs or settings')
        if previous['status'] == 'DiagnosticOnly':
            return previous
        index=0
        while (out/f'prior_guard_{index:02d}.json').exists(): index+=1
        shutil.copy2(state_path,out/f'prior_guard_{index:02d}.json')
    dump(state_path,report)
    try:
        meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
        originals={int(p.stem):dict(np.load(p)) for p in (reference/'charts').glob('*.npz')}
        records={int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
        if set(originals) != set(records) or any(any(not np.array_equal(originals[i][k], records[i][k])
                for k in ('region','triangle_ids','vertex_ids')) for i in records):
            raise ValueError('Continuity recovery requires identical source chart ownership')
        restore_groups(originals)
        groups=sorted({tuple(map(int,r['shared_support_group'])) for r in originals.values() if 'shared_support_group' in r})
        broken=[list(group) for group in groups if any(any(not np.array_equal(records[group[0]][k],records[i][k])
                    for k in GEOMETRY) for i in group[1:])]
        report.update(baseline_common_groups=[list(g) for g in groups], broken_common_groups=broken)
        def measured(folder, path, supplied=None):
            if path.exists():
                return json.loads(path.read_text(encoding='utf-8'))
            if supplied is not None:
                result=json.loads(Path(supplied).read_text(encoding='utf-8'))
                if result['samples_per_curve'] != 129 or 'both support knot crossings' not in result['sampling']:
                    raise ValueError('Incompatible supplied continuity sampling protocol')
                dump(path,result)
                return result
            return audit(folder,path,samples=129)
        before=measured(reference,out/'baseline.json',baseline_report)
        initial=measured(root,out/'initial.json',initial_report)
        report['initial_regressions']=regressions(before,initial)
        dump(state_path,report)
        current=root; measured_current=initial
        if broken:
            proposal=out/'shared_proposal'; completed=out/'shared_fit'
            members=sorted({i for group in broken for i in group})
            if not proposal.exists():
                shutil.copytree(root,proposal,ignore=shutil.ignore_patterns('blocks'))
                candidate=dict(meta, source_fit=str(proposal.resolve()), requires_support_refit=True,
                               required_refit_charts=members, force_refit_charts=members,
                               required_shared_support_groups=broken)
                dump(proposal/'run.json',candidate)
            candidate=json.loads((proposal/'run.json').read_text(encoding='utf-8'))
            candidate['common_support_reference_fit']=str(reference.resolve())
            dump(proposal/'run.json',candidate)
            saved=completed/'network_support.json'
            cached=json.loads(saved.read_text(encoding='utf-8')) if saved.exists() else None
            if cached is not None and cached.get('common_support_reference_fit')==str(reference.resolve()):
                # A completed failed proposal is useful feedback too. Resume
                # its network backoff instead of repeating the same solve.
                restored=cached
            else:
                restored=refit(proposal,completed,workers=workers,boundary_normal_weight=0.,resume=completed.exists())
            report['common_support_recovery']=restored; dump(state_path,report)
            if restored is not None and restored['accepted']:
                current=completed; measured_current=measured(current,out/'shared.json')
            else:
                # A moved curve need not be representable by the old common
                # support. Feed the failed group back to network construction
                # instead of silently splitting its previously smooth surface.
                from .global_network import reconstruct as network
                from .curve_routes import Config as CurveConfig
                network_meta=meta['global_network']; excluded=set(network_meta['excluded_charts'])
                if restored is None: raise ValueError('Common-support proposal has invalid input trims')
                excluded.update(restored['failed_charts']); report['network_backoff']=[]
                for retry in range(3):
                    net=out/f'network_backoff_{retry:02d}'; fitted=out/f'support_backoff_{retry:02d}'
                    if (net/'global_network.json').exists():
                        nreport=json.loads((net/'global_network.json').read_text(encoding='utf-8'))
                        if set(nreport['excluded_charts']) != excluded: raise ValueError('Backoff checkpoint has different protected neighborhoods')
                    else:
                        nreport=network(reference,net,config=CurveConfig(**network_meta['config']),excluded_charts=excluded)
                        reuse_native_supports(net,root,protected=excluded)
                    saved=fitted/'network_support.json'
                    if saved.exists():
                        result=json.loads(saved.read_text(encoding='utf-8'))
                    else:
                        result=refit(net,fitted,workers=workers,boundary_normal_weight=0.,resume=fitted.exists())
                    report['network_backoff'].append(dict(attempt=retry,network=str(net.resolve()),
                        supports=str(fitted.resolve()),result=result,excluded_charts=sorted(excluded)))
                    dump(state_path,report)
                    if result is not None and result['accepted']:
                        current=fitted; report['network_fit']=str(net.resolve()); report['support_fit']=str(fitted.resolve())
                        measured_current=measured(current,out/f'backoff_{retry:02d}.json')
                        break
                    if result is None: raise ValueError('Continuity feedback produced invalid network trims')
                    old_count=len(excluded); excluded.update(result['failed_charts'])
                    if len(excluded)==old_count: raise ValueError('Continuity network backoff made no progress')
                else:
                    raise ValueError('Common-support network backoff budget exhausted')
        cost=lambda rows:sum(r['length_mm']*r['excess_degrees']**2 for r in rows)
        for iteration in range(rounds):
            bad=regressions(before,measured_current)
            if not bad:
                break
            selected=sorted({i for row in bad for i in row['charts']})
            destination=out/f'correction_{iteration:02d}'
            saved=destination/'global_continuity.json'
            if saved.exists():
                step=json.loads(saved.read_text(encoding='utf-8'))
                if Path(step['source_fit']).resolve() != current.resolve() or step.get('selected_charts') != selected:
                    raise ValueError('Cached correction has different seam neighborhoods')
            else:
                step=fair(current,destination,config=config,sweeps=1,workers=workers,
                          selected_charts=selected,resume=destination.exists(),reference_fit=reference)
            after=measured(destination,out/f'correction_{iteration:02d}.json')
            remaining=regressions(before,after)
            accepted=cost(remaining) < cost(bad)
            report['history'].append(dict(iteration=iteration, selected_charts=selected,
                before_regressions=bad, after_regressions=remaining, accepted=accepted,
                before_excess_energy=cost(bad), after_excess_energy=cost(remaining), output=str(destination.resolve())))
            dump(state_path,report)
            if not accepted:
                break
            current=destination; measured_current=after
        report.update(status='DiagnosticOnly',surface_fit=str(current.resolve()),
                      remaining_regressions=regressions(before,measured_current),
                      shared_curves_unchanged=not bool(report.get('network_backoff')),
                      scope='Bounded automatic recovery. Residual seam regressions remain explicit; this is not an exact G1/G2 certificate.')
        dump(out/'final.json',measured_current)
    except (Exception,KeyboardInterrupt) as error:
        report.update(status='interrupted' if isinstance(error,KeyboardInterrupt) else 'failed',error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['seconds']=time.perf_counter()-start; dump(state_path,report)
    return report
