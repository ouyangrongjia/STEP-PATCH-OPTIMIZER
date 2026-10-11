"""Automatic whole-model curve network, support and continuity optimization.

Failed support neighborhoods are excluded and rebuilt from the unchanged
baseline. Compatible completed proposals are reused, then the ordinary STEP
distance-repair pipeline performs independent actual-surface validation.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .distance_guard import validate_fit, Options
from .partition_fit import spline_graph_error
from .global_network import reconstruct as network
from .network_support import refit, validate_wires
from .global_continuity import optimize as continuity, default_config, OBJECTIVE_VERSION
from .surface_jets import audit
from .boundary_quality import compare
from .route_audit import audit as audit_routes
from .skeleton_reconstruct import reconstruct as finalize
from .continuity_guard import optimize as guard_continuity, optimize_curvature, regressions, curvature_regressions


def reuse_supports(candidate, previous):
    """Reuse a previously validated support only if it fits the new curves."""
    root, old = Path(candidate), Path(previous)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    prior = {r['chart']: r for r in json.loads((old/'fitting.json').read_text(encoding='utf-8'))}
    fitting = {r['chart']: r for r in json.loads((root/'fitting.json').read_text(encoding='utf-8'))}
    curves = [dict(np.load(p)) for p in (root/'curves').glob('*.npz')]
    # A failed attempt can still contain supports inherited from an earlier
    # successful local fit. Carry those compatible records across every retry,
    # not just proposals computed in the immediately preceding attempt.
    candidates = {}
    old_report = json.loads((old/'network_support.json').read_text(encoding='utf-8'))
    old_input = Path(old_report['source_fit'])
    for di in old_report.get('reused_compatible_charts', []):
        candidates[int(di)] = old_input/'charts'/f'{int(di):05d}.npz'
    candidates.update({int(p.stem):p for p in (old/'support_proposals').glob('*.npz')})
    accepted = []
    from .shared_support import restore_groups
    references = {int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    restore_groups(references)
    groups = {tuple(map(int,r.get('shared_support_group',[di]))) for di,r in references.items()}
    for group in sorted(groups):
        if any(di not in candidates or not prior.get(di, {}).get('accepted') for di in group):
            continue
        records = {di:dict(np.load(candidates[di])) for di in group}
        if any(any(not np.array_equal(records[di][k], references[di][k]) for k in ('region','triangle_ids','vertex_ids')) for di in group):
            raise ValueError('Support cache changed region or source triangle identity')
        if any(any(not np.array_equal(records[di][k], records[group[0]][k]) for k in ('origin','axes','lo','size','knots','height')) for di in group):
            continue
        try:
            error = max(spline_graph_error(c['poles'], c['knots'], records[int(di)])
                        for c in curves for di in c['owners'] if di in group)
        except ValueError:
            continue
        if np.isfinite(error) and error < meta['config']['boundary']*.4:
            for di in group:
                rec = records[di]
                if len(group) > 1: rec['shared_support_group'] = np.array(group)
                np.savez_compressed(root/'charts'/f'{di:05d}.npz', **rec)
                fitting[di] = dict(prior[di], reused_from=str(candidates[di].resolve()))
                accepted.append(di)
    dump(root/'fitting.json', list(fitting.values()))
    meta['previous_support_reuse'] = dict(source=str(old.resolve()), charts=accepted)
    dump(root/'run.json', meta)
    return accepted


def retain_changed_objective(output, reference):
    """Keep obsolete downstream results when the joint objective changes."""
    out=Path(output); stage=out/'continuity'
    if not stage.exists():
        return None
    path=next((stage/n for n in ('global_continuity.json','progress.json') if (stage/n).exists()),None)
    old={} if path is None else json.loads(path.read_text(encoding='utf-8'))
    expected=asdict(default_config()); reference=str(Path(reference).resolve())
    if (old.get('config')==expected and old.get('reference_fit')==reference
            and old.get('objective_version')==OBJECTIVE_VERSION):
        return None
    archive=out/f'before_objective_change_{len(list(out.glob("before_objective_change_*"))):02d}'
    if archive.resolve().parent!=out.resolve():
        raise ValueError('Invalid objective archive path')
    archive.mkdir()
    names=('continuity','continuity_guard','curvature_guard','validated','images','final_metrics',
           'continuity_prevalidation.json','optimization.json','visual_review.json','visual_review_rejected.json')
    for name in names:
        source=out/name; target=archive/name
        if source.exists():
            if source.resolve().parent!=out.resolve() or target.resolve().parent!=archive.resolve():
                raise ValueError('Cached objective path escapes its run')
            source.rename(target)
    baseline=archive/'final_metrics/baseline_continuity.json'
    if baseline.exists():
        (out/'final_metrics').mkdir(); shutil.copy2(baseline,out/'final_metrics/baseline_continuity.json')
    info=dict(archive=str(archive.resolve()),previous_config=old.get('config'),config=expected,
              previous_objective_version=old.get('objective_version'),objective_version=OBJECTIVE_VERSION,
              reference_fit=reference,reason='Joint objective or fixed fidelity reference changed; downstream geometry and diagnostics are not reusable.')
    dump(archive/'retention.json',info)
    return info


def optimize(source, output, reference_faces, *, workers=8, network_attempts=3, sweeps=2,
             resume=False, distance_options=None, creo_root=None, native_probe=None):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=resume)
    distance_options = distance_options or Options(preserve_continuity=True)
    started = time.perf_counter(); m = json.loads((root/'run.json').read_text(encoding='utf-8'))
    validate_fit(root, m['partition'], m['stl'])
    retained=retain_changed_objective(out,root) if resume else None
    state = dict(status='running', source_fit=str(root.resolve()), can_apply=False, stages=[],
                 original_distance_mm=m['config']['distance'], original_support_boundary_mm=m['config']['boundary']*.4,
                 source_coordinates_and_labels_unchanged=True, source_CAD_read=False, commercial_engine_used=False)
    if retained is not None:
        state['stages'].append(dict(stage='retained_previous_objective',**retained))
    excluded = set(); previous = None
    try:
        for attempt in range(1, network_attempts+1):
            net = out/f'network_{attempt:02d}'; support = out/f'support_{attempt:02d}'
            if resume and (net/'global_network.json').exists():
                report = json.loads((net/'global_network.json').read_text(encoding='utf-8'))
                if set(report['excluded_charts']) != excluded:
                    raise ValueError('Cached network has different excluded support neighborhoods')
            else:
                report = network(root, net, excluded_charts=excluded)
                if previous is not None:
                    reuse_supports(net, previous)
            event = dict(stage='network_support', attempt=attempt, network=str(net.resolve()),
                         supports=str(support.resolve()), candidate_regions=report['changed_regions'],
                         candidate_curves=len(report['changed_curves']))
            state['stages'].append(event); dump(out/'optimization.json', state)
            if resume and (support/'network_support.json').exists():
                fit = json.loads((support/'network_support.json').read_text(encoding='utf-8'))
                if Path(fit['source_fit']).resolve() != net.resolve():
                    raise ValueError('Cached support result belongs to another network')
            else:
                fit = refit(net, support, workers=workers, boundary_normal_weight=0.,
                            resume=resume and support.exists())
            if fit is None:
                raise ValueError('Global candidate wires failed before support fitting')
            event.update(accepted=fit['accepted'], failed_charts=fit['failed_charts'])
            dump(out/'optimization.json', state)
            if fit['accepted']:
                break
            old_count = len(excluded); excluded.update(map(int, fit['failed_charts']))
            if len(excluded) == old_count:
                raise ValueError('Automatic neighborhood backoff made no progress')
            previous = support
        else:
            raise ValueError('Automatic network support attempt budget exhausted')
        state['network_fit'] = str(net.resolve()); state['support_fit'] = str(support.resolve())
        joined = out/'continuity'
        state['stages'].append(dict(stage='global_continuity', output=str(joined.resolve())))
        dump(out/'optimization.json', state)
        if resume and (joined/'global_continuity.json').exists():
            joint = json.loads((joined/'global_continuity.json').read_text(encoding='utf-8'))
            if Path(joint['source_fit']).resolve() != support.resolve():
                raise ValueError('Cached continuity result belongs to another support fit')
        else:
            joint = continuity(support, joined, sweeps=sweeps, resume=resume and joined.exists(),
                               workers=min(workers, 4),reference_fit=root)
        state['joint_fit'] = str(joined.resolve()); state['joint_changed_charts'] = joint['changed_charts']
        metrics = out/'final_metrics'; metrics.mkdir(exist_ok=True)
        baseline_audit = metrics/'baseline_continuity.json'; initial_audit = out/'continuity_prevalidation.json'
        if not baseline_audit.exists():
            audit(root,baseline_audit,samples=129)
        if not initial_audit.exists():
            audit(joined,initial_audit,samples=129)
        guarded = out/'continuity_guard'
        state['stages'].append(dict(stage='continuity_regression_recovery',output=str(guarded.resolve())))
        dump(out/'optimization.json',state)
        recovered = guard_continuity(root,joined,guarded,workers=min(workers,4),resume=resume and guarded.exists(),
                                     baseline_report=baseline_audit,initial_report=initial_audit)
        joined=Path(recovered['surface_fit']); state['continuity_recovery']=recovered
        if 'network_fit' in recovered:
            state['network_fit']=recovered['network_fit']; state['support_fit']=recovered['support_fit']
            support=Path(recovered['support_fit'])
            report=json.loads((Path(recovered['network_fit'])/'global_network.json').read_text(encoding='utf-8'))
            excluded=set(report['excluded_charts'])
        curvature_guarded=out/'curvature_guard'
        state['stages'].append(dict(stage='curvature_regression_recovery',output=str(curvature_guarded.resolve())))
        dump(out/'optimization.json',state)
        curvature=optimize_curvature(root,joined,curvature_guarded,workers=min(workers,4),
            resume=resume and curvature_guarded.exists(),baseline_report=baseline_audit,initial_report=guarded/'final.json')
        joined=Path(curvature['surface_fit']); state['curvature_recovery']=curvature
        product = out/'validated'
        state['stages'].append(dict(stage='actual_STEP_validation', output=str(product.resolve())))
        dump(out/'optimization.json', state)
        result = None; resume_validation = False
        if resume and (product/'run.json').exists():
            result = json.loads((product/'run.json').read_text(encoding='utf-8'))
            if result['status'] != 'DiagnosticOnly':
                from .fit_checkpoint import matches, adopt_audit
                snapshot=product/'distance_guard/round_00/input'
                if not snapshot.exists():
                    adopt_audit(joined,snapshot.parent)
                if (not any((product/name).exists() for name in ('step','regions','creo'))
                        and matches(joined,snapshot)):
                    resume_validation=True
                else:
                    retained=out/f'validation_before_resume_{len(list(out.glob("validation_before_resume_*"))):02d}'
                    if product.resolve().parent!=out.resolve() or retained.resolve().parent!=out.resolve():
                        raise ValueError('Invalid retained validation path')
                    product.rename(retained)
                    state['stages'].append(dict(stage='retained_incomplete_validation',output=str(retained.resolve())))
                result=None
        if result is None:
            result = finalize(m['stl'], m['partition'], product, reference_faces=reference_faces,
                              surface_fit=joined, prior_fit=support, render=False,
                              distance_options=distance_options, creo_root=creo_root, native_probe=native_probe,
                              resume=resume_validation)
        for label, source_fit in (('baseline', root), ('final', result['surface_fit'])):
            target = metrics/f'{label}_continuity.json'
            if not (resume and target.exists()):
                if label == 'final' and Path(source_fit).resolve() == joined.resolve():
                    shutil.copy2(curvature_guarded/'final.json',target)
                else:
                    audit(source_fit, target, samples=129)
        trim_path=metrics/'native_trim_validation.json'
        if not (resume and trim_path.exists()):
            final_root=Path(result['surface_fit']); data=np.load(m['partition'])
            records={int(p.stem):dict(np.load(p)) for p in (final_root/'charts').glob('*.npz')}
            curves={int(p.stem):dict(np.load(p)) for p in (final_root/'curves').glob('*.npz')}
            failures=validate_wires(data['mesh_vertex_original'],data['mesh_triangle'],records,curves,
                                    set(records),native_frame=True)
            dump(trim_path,dict(passed=not failures,checked_charts=len(records),failures=failures,
                                surface_fit=str(final_root.resolve())))
        if not json.loads(trim_path.read_text(encoding='utf-8'))['passed']:
            raise ValueError('Final native support trim projection failed')
        before=json.loads((metrics/'baseline_continuity.json').read_text(encoding='utf-8'))
        after=json.loads((metrics/'final_continuity.json').read_text(encoding='utf-8'))
        dump(metrics/'continuity_regressions.json',dict(normal_p95_allowance_degrees=2.,
             remaining=regressions(before,after),curvature_absolute_allowance_per_mm=.5,
             curvature_relative_allowance=.25,curvature_remaining=curvature_regressions(before,after),
             surface_fit=result['surface_fit']))
        if not (resume and (metrics/'boundaries.json').exists()):
            compare(root, result['surface_fit'], metrics/'boundaries.json')
        curve_path = metrics/'curve_audit.json'
        if resume and curve_path.exists():
            curve_check = json.loads(curve_path.read_text(encoding='utf-8'))
        else:
            curve_check = audit_routes(root, result['surface_fit'], curve_path,
                                       tube=report['config']['tube'], movement=report['config']['movement'])
        if (not all(curve_check[k] for k in ('master_restrictions_passed', 'source_tube_passed', 'movement_passed'))
                or curve_check['maximum_junction_disagreement_mm'] > 1e-8):
            raise ValueError('Final shared curve network failed its independent audit')
        state.update(status='DiagnosticOnly', surface_fit=result['surface_fit'], model=result['model'],
                     excluded_chart_neighborhoods=sorted(excluded), final_metrics=str(metrics.resolve()))
    except (Exception, KeyboardInterrupt) as error:
        state.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                     error=f'{type(error).__name__}: {error}')
        raise
    finally:
        state['seconds'] = time.perf_counter()-started
        dump(out/'optimization.json', state)
    return state


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--reference-faces', type=int, required=True); parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--network-attempts', type=int, default=3); parser.add_argument('--sweeps', type=int, default=2)
    parser.add_argument('--resume', action='store_true'); parser.add_argument('--distance-seed', type=int, default=810572)
    parser.add_argument('--creo-root'); parser.add_argument('--native-probe')
    args = parser.parse_args()
    optimize(args.run, args.output, args.reference_faces, workers=args.workers, network_attempts=args.network_attempts,
             sweeps=args.sweeps, resume=args.resume, distance_options=Options(seed=args.distance_seed, preserve_continuity=True),
             creo_root=args.creo_root, native_probe=args.native_probe)
