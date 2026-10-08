"""Native fixed-partition fitting, agglomeration, assembly and independent QA.

CAD reference geometry enters only the independent face-count evaluator. The
reconstruction uses the original STL plus supplied discrete region labels.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time
from .mesh import dump
from .partition_fit import Config, load_partition, fit_partition


def reconstruct(stl, partition, output, *, reference=None, reference_faces=None,
                config=None, merge_trials=3200, from_fit=None, creo_root=None,
                native_probe=None, render=True, max_refinements=2, boundary_smoothing=0.,
                fairing=False, condition_normals=False, boundary_history=None, compact_edges=False,
                feature_split_angle=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); start = time.perf_counter()
    report = dict(status='running', can_apply=False, algorithm='native-regional-graph-reconstruction-v1',
        stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()), units='mm',
        commercial_reconstruction_used=False, fitter_reads_source_CAD=False)
    dump(out/'run.json', report)
    try:
        if max_refinements < 0:
            raise ValueError('max_refinements must be nonnegative')
        if boundary_smoothing < 0:
            raise ValueError('Boundary smoothing distance must be nonnegative')
        if compact_edges and not boundary_smoothing:
            raise ValueError('Compact shared edges require positive boundary smoothing')
        report['boundary_smoothing_mm'] = boundary_smoothing
        report.update(bounded_fairing=fairing, normal_cone_conditioning=condition_normals,
                      compact_shared_edges=compact_edges, feature_split_angle_degrees=feature_split_angle)
        if (reference is None) == (reference_faces is None):
            raise ValueError('Supply exactly one reference STEP or independently measured reference face count')
        if reference is not None:
            from .reference_catalog import transferred_face_count
            reference_faces = transferred_face_count(reference)
            report['reference'] = str(Path(reference).resolve())
        limit = int(reference_faces)-1
        report.update(reference_faces=int(reference_faces), maximum_reconstructed_faces=limit)
        v, f, labels, topo = load_partition(partition, stl)
        report['input_topology'] = topo
        if len(set(labels)) > limit:
            raise ValueError('Preserving all supplied regions already exceeds the strict reference face budget')
        if from_fit is None:
            config = config or Config(distance=.03, boundary=.01, max_charts=max(16, int(reference_faces)*4),
                shared_smooth_normals=True, normal_weight=.003, boundary_normal_weight=.01,
                fairness=1e-7, interior_subdivisions=4)
            fit = out/'fit'; fit.mkdir()
            metadata = dict(status='running', config=asdict(config), partition=report['partition'], stl=report['stl'],
                            source_CAD_read=False, commercial_reconstruction_used=False)
            dump(fit/'run.json', metadata); tick = time.perf_counter()
            fit_partition(v, f, labels, config, fit)
            metadata.update(status='DiagnosticOnly', seconds=time.perf_counter()-tick); dump(fit/'run.json', metadata)
            from .graph_merge import merge
            report['merging'] = merge(fit, out/'merged', trials=merge_trials)
            fit = out/'merged'
        else:
            fit = Path(from_fit)
            metadata = json.loads((fit/'run.json').read_text(encoding='utf-8'))
            if (Path(metadata['partition']).resolve() != Path(partition).resolve()
                    or Path(metadata['stl']).resolve() != Path(stl).resolve()):
                raise ValueError('The native fit cache belongs to a different declared STL or partition')
            if metadata.get('status') != 'DiagnosticOnly':
                raise ValueError('Native fit cache did not complete')
            from .graph_repair import repair
            report['cache_refinement'] = repair(fit, out/'refined')
            fit = out/'refined'
        if fairing:
            from .graph_fair_optimize import optimize
            report['fairing'] = optimize(fit, out/'faired', time_limit=5., boundary_normal_weight=.1,
                                        normal_field='crease-fans')
            fit = out/'faired'
        if condition_normals:
            from .graph_condition import condition
            report['conditioning'] = condition(fit, out/'conditioned')
            fit = Path(report['conditioning']['fit'])
        if feature_split_angle is not None:
            from .graph_repair import repair
            report['feature_split'] = repair(fit, out/'feature_split', normal_cone=condition_normals,
                                             feature_split_angle=feature_split_angle)
            fit = out/'feature_split'
        from .graph_reassemble import reconstruct as assemble
        from .graph_export import export
        from .step_quality import audit, UntriangulatedFaces
        from .graph_repair import repair, failed_step_charts
        report['attempts'] = []
        skip_smooth = set()
        histories = ([boundary_history] if isinstance(boundary_history, (str, Path)) else boundary_history) or []
        report['boundary_history'] = []
        for history in histories:
            previous = json.loads((Path(history)/'run.json').read_text(encoding='utf-8'))
            if (Path(previous['stl']).resolve() != Path(stl).resolve()
                    or Path(previous['partition']).resolve() != Path(partition).resolve()):
                raise ValueError('Boundary history belongs to a different STL or fixed partition')
            import numpy as np
            new_assignment = np.load(fit/'chart_index.npz')['face_chart']
            events = previous.get('attempts', [])
            if not events and 'source_fit' in previous and 'cad_face_charts' in previous:
                invalid = previous.get('topology_before', {}).get('invalid_faces', [])
                events = [dict(source_fit=previous['source_fit'],
                    boundary_fallback_charts=[previous['cad_face_charts'][i] for i in invalid],
                    smoothing_disabled_charts=previous.get('smoothing_disabled_charts', []))]
            for event in events:
                for chart in set(event.get('boundary_fallback_charts', [])+event.get('smoothing_disabled_charts', [])):
                    old = np.load(Path(event['source_fit'])/'charts'/f'{chart:05d}.npz')
                    skip_smooth.update(map(int, new_assignment[old['triangle_ids']]))
            invalid_step = previous.get('step', {}).get('topology', {}).get('invalid_faces', [])
            if invalid_step:
                old_step = Path(previous['attempts'][-1]['directory'])/'step/model.step'
                skip_smooth.update(failed_step_charts(old_step, invalid_step, fit))
            report['boundary_history'].append(dict(source=str(Path(history).resolve()),
                                                    cumulative_fallback_charts=sorted(skip_smooth)))
        for attempt in range(max_refinements+1):
            for key in ('assembly', 'step', 'quality', 'creo', 'creo_native_body'):
                report.pop(key, None)
            stage = out if attempt == 0 else out/f'recovery_{attempt}'
            stage.mkdir(exist_ok=True)
            report['native_fit'] = str(fit.resolve())
            faces = len(list((fit/'charts').glob('*.npz')))
            report.update(fitted_charts=faces, face_budget_passed=faces <= limit)
            if faces > limit:
                raise ValueError(f'Regional fitting still exceeds reference face count: {faces} >= {reference_faces}')
            current = dict(attempt=attempt, source_fit=str(fit.resolve()), directory=str(stage.resolve()),
                           smoothing_disabled_charts=sorted(skip_smooth), compact_shared_edges=compact_edges)
            report['attempts'].append(current); dump(out/'run.json', report)
            report['assembly'] = assemble(fit, stage/'assembled', segment_edges=not bool(boundary_smoothing),
                smooth_boundary=boundary_smoothing, smooth_segments=bool(boundary_smoothing) and not compact_edges,
                skip_smooth_faces=sorted(skip_smooth), write_preview=False)
            invalid = report['assembly']['topology_before']['invalid_faces']
            if invalid and boundary_smoothing and attempt < max_refinements:
                affected = {report['assembly']['cad_face_charts'][i] for i in invalid}
                if affected-skip_smooth:
                    skip_smooth.update(affected)
                    current.update(status='failed_native_trim', failed_faces=invalid,
                                   boundary_fallback_charts=sorted(affected))
                    continue
            report['step'] = export(stage/'assembled', stage/'step')
            step = stage/'step/model.step'
            report['face_budget_passed'] = report['step']['topology']['faces'] < int(reference_faces)
            failed_faces = report['step']['topology']['invalid_faces']
            if failed_faces and boundary_smoothing and attempt < max_refinements:
                affected = set(failed_step_charts(step, failed_faces, fit, assembly=stage/'assembled'))
                if affected-skip_smooth:
                    skip_smooth.update(affected)
                    current.update(status='failed_step_trim', failed_faces=failed_faces,
                                   boundary_fallback_charts=sorted(affected))
                    continue
            if not report['step']['watertight'] or not report['face_budget_passed']:
                if compact_edges and report['face_budget_passed'] and attempt < max_refinements:
                    # A long curve can trigger excess pcurve tolerance during
                    # STEP import. Exact spline subspans change representation,
                    # not geometry or the declared tolerance budget.
                    current.update(status='failed_compact_step', step_topology=report['step']['topology'],
                                   recovery='exact_curve_subspans')
                    compact_edges = False
                    continue
                raise ValueError('Exported STEP failed its topology, tolerance or actual face-count gate')
            refine = []; selection = {}
            try:
                report['quality'] = audit(step, partition, stage/'quality', render=render)
            except UntriangulatedFaces as error:
                current.update(status='failed_tessellation', failed_faces=error.faces)
                if attempt >= max_refinements:
                    raise
                refine = failed_step_charts(step, error.faces, fit, assembly=stage/'assembled', details=selection)
                current['failed_face_selection'] = selection
                if boundary_smoothing and set(refine)-skip_smooth:
                    skip_smooth.update(refine); current['boundary_fallback_charts'] = refine
                    continue
            if not refine and creo_root:
                from .creo_validation import creo_check, native_body_check, export_native_part
                report['creo'] = creo_check(step, stage/'creo', creo_root, timeout=480, boundary_preference='XYZ')
                if native_probe:
                    part = stage/'creo/audit/import/model.prt.1'
                    report['creo_native_body'] = native_body_check(part, stage/'creo/native_body', creo_root, native_probe)
                    if not report['creo_native_body']['native_closed_solid']:
                        current['status'] = 'failed_creo_solid'
                    if not report['creo_native_body']['native_closed_solid'] and attempt < max_refinements:
                        from .creo_boundary_audit import audit as diagnose
                        exported = export_native_part(part, stage/'creo_export', creo_root)
                        diagnostic = diagnose(exported, partition, stage/'creo_boundaries', fit/'chart_index.npz')
                        refine = sorted({int(k) for edge in diagnostic['abnormal_edges']
                                         for k in edge.get('nearby_chart_votes', {})})
                        invalid = diagnostic['topology']['invalid_faces']
                        if invalid:
                            refine = sorted(set(refine) | set(failed_step_charts(exported, invalid, fit)))
                            current['creo_invalid_faces'] = invalid
                        current.update(status='failed_creo_solid', diagnostic_only_CAD=str(exported))
                        if boundary_smoothing and set(refine)-skip_smooth:
                            skip_smooth.update(refine)
                            current.update(boundary_fallback_charts=refine,
                                           recovery='restore_source_shared_curves')
                            continue
            if not refine:
                current.setdefault('status', 'completed_diagnostics')
                break
            current['refined_source_charts'] = refine
            current['boundary_scale'] = .25**(attempt+1)
            next_fit = out/f'recovery_{attempt+1}'/'refined'
            current['grow_refinement_neighbors'] = not selection.get('all_supports_verified', False)
            repair(fit, next_fit, refine_charts=refine, boundary_scale=current['boundary_scale'],
                   grow_neighbors=current['grow_refinement_neighbors'],
                   normal_cone=condition_normals)
            if skip_smooth:
                # Refit may split or renumber charts. Preserve known curve
                # failures by source triangle identity instead of retrying
                # the same rejected smoothing after every geometry repair.
                import numpy as np
                assignment = np.load(next_fit/'chart_index.npz')['face_chart']
                remapped = set()
                for chart in skip_smooth:
                    original = np.load(fit/'charts'/f'{chart:05d}.npz')
                    remapped.update(map(int, assignment[original['triangle_ids']]))
                skip_smooth = remapped
            fit = next_fit
        report['native_Creo_solid_passed'] = report.get('creo_native_body', {}).get('native_closed_solid')
        report.update(status='DiagnosticOnly', model=str(step.resolve()),
            remaining_review='Review geometric extrema, smooth interfaces, actual Creo body evidence and local views jointly')
        (out/'RUN.md').write_text(
            '# 原生固定分区重建\n\n'
            f'输出 STEP：[{step.name}]({step.relative_to(out).as_posix()})。实际面数 {report["step"]["topology"]["faces"]}，'
            f'对应参考 {reference_faces} 面。拟合与合并保留原 STL 三角形标签。\n\n'
            '本目录保存独立几何与 Creo 检查事实，状态仍为 `DiagnosticOnly`；'
            '是否符合光顺与保真要求需结合最大误差、接缝和多视角局部图判断。\n\n'
            f'[完整记录](run.json) · [几何指标]({(stage/"quality/run.json").relative_to(out).as_posix()})\n', encoding='utf-8')
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-start; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--stl', required=True); p.add_argument('--partition', required=True)
    p.add_argument('--output', required=True); group = p.add_mutually_exclusive_group(required=True)
    group.add_argument('--reference'); group.add_argument('--reference-faces', type=int)
    p.add_argument('--from-fit'); p.add_argument('--creo-root'); p.add_argument('--native-probe')
    p.add_argument('--merge-trials', type=int, default=3200); p.add_argument('--no-render', action='store_true')
    p.add_argument('--max-refinements', type=int, default=2)
    p.add_argument('--boundary-smoothing', type=float, default=0.)
    p.add_argument('--fairing', action='store_true'); p.add_argument('--condition-normals', action='store_true')
    p.add_argument('--boundary-history', nargs='+')
    p.add_argument('--compact-edges', action='store_true')
    p.add_argument('--feature-split-angle', type=float)
    p.add_argument('--distance', type=float, default=.03); p.add_argument('--boundary', type=float, default=.01)
    p.add_argument('--normal-weight', type=float, default=.003); p.add_argument('--boundary-normal-weight', type=float, default=.01)
    p.add_argument('--fairness', type=float, default=1e-7); p.add_argument('--interior-subdivisions', type=int, default=4)
    p.add_argument('--fit-chart-limit', type=int, default=12000)
    a = p.parse_args(); config = Config(distance=a.distance, boundary=a.boundary, normal_weight=a.normal_weight,
        boundary_normal_weight=a.boundary_normal_weight, fairness=a.fairness, interior_subdivisions=a.interior_subdivisions,
        shared_smooth_normals=True, max_charts=a.fit_chart_limit)
    reconstruct(a.stl, a.partition, a.output, reference=a.reference, reference_faces=a.reference_faces,
        config=config, merge_trials=a.merge_trials, from_fit=a.from_fit, creo_root=a.creo_root,
        native_probe=a.native_probe, render=not a.no_render, max_refinements=a.max_refinements,
        boundary_smoothing=a.boundary_smoothing, fairing=a.fairing, condition_normals=a.condition_normals,
        boundary_history=a.boundary_history, compact_edges=a.compact_edges,
        feature_split_angle=a.feature_split_angle)
