"""Fixed STL partitions -> shared smooth skeleton -> regional supports -> solid.

The optional reference STEP is read only by the independent face counter. No
reference geometry or commercial fitting engine is passed to reconstruction.
"""
import argparse
from pathlib import Path
import shutil
import time
from .mesh import dump
from .partition_fit import Config


def reconstruct(stl, partition, output, *, reference=None, reference_faces=None,
                config=None, smoothing=.12, workers=4, rounds=12, from_fit=None,
                render=True, creo_root=None, native_probe=None,
                profile_mode='installed', boundary_mode='chain', normal_field='vertex',
                normal_filter_steps=0, support_solver='least-squares', boundary_cache=None, boundary_plane_budget=None,
                repair_charts=(), repair_smooth_joins=False, refit_regions=None,
                surface_fit=None, distance_options=None, prior_fit=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter()
    report = dict(status='running', can_apply=False, algorithm='shared-skeleton-regional-solid-v1',
        stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()), units='mm',
        fitter_reads_source_CAD=False, commercial_reconstruction_used=False,
        visual_review_required=True, from_fit=None if from_fit is None else str(Path(from_fit).resolve()))
    dump(out/'run.json', report)
    try:
        if surface_fit is not None and (from_fit is not None or config is not None or boundary_cache is not None
                                       or repair_charts or refit_regions is not None):
            raise ValueError('Completed surface-fit input cannot also request skeleton refitting')
        if (reference is None) == (reference_faces is None):
            raise ValueError('Supply exactly one reference STEP or independently measured face count')
        if (creo_root is None) != (native_probe is None):
            raise ValueError('Creo validation requires both the Creo installation and the native body probe')
        if reference is not None:
            from .reference_catalog import transferred_face_count
            reference_faces = transferred_face_count(reference)
            report['reference_for_face_count_only'] = str(Path(reference).resolve())
        report['reference_faces'] = int(reference_faces)
        if config is None and from_fit is None and surface_fit is None:
            config = Config(distance=.03, boundary=.005, controls=(65,), normal_weight=.001, fairness=1e-6)
        if surface_fit is None:
            from .skeleton_graph import reconstruct as fit
            fitted = fit(partition, stl, out/'fit', smoothing=smoothing, workers=workers,
                         rounds=rounds, face_limit=int(reference_faces)-1, config=config, from_fit=from_fit,
                         boundary_mode=boundary_mode, normal_field=normal_field, normal_filter_steps=normal_filter_steps,
                         support_solver=support_solver, boundary_cache=boundary_cache, boundary_plane_budget=boundary_plane_budget,
                         repair_charts=repair_charts, refit_regions=refit_regions)
        else:
            from .distance_guard import validate_fit
            fitted = validate_fit(surface_fit, partition, stl)
        report['fitting'] = fitted
        if not fitted['complete_source_coverage'] or not fitted['partition_labels_unchanged']:
            raise ValueError('Incomplete source partition coverage')
        surface_fit = fitted['source_fit']
        if repair_smooth_joins:
            from .shared_support import optimize
            report['shared_support_repair'] = optimize(surface_fit, out/'shared_support', workers=workers)
            surface_fit = str((out/'shared_support').resolve())
        from .distance_guard import finalize
        guard = finalize(surface_fit, out/'distance_guard', reference_faces=int(reference_faces),
                         options=distance_options, prior_fit=prior_fit, render=render)
        report['distance_guard'] = guard
        final = guard['history'][-1]; stage = Path(guard['final_round'])
        report['surface_fit'] = guard['source_fit']
        report['assembly'] = final['assembly']; report['step'] = final['step']
        # Retain the existing public STEP/quality paths; all failed attempts
        # remain in distance_guard and are never published as passing results.
        for name in ('step', 'quality'):
            shutil.copytree(stage/name, out/name)
        report['model'] = str((out/'step/model.step').resolve())
        report['face_budget_passed'] = report['step']['topology']['faces'] < int(reference_faces)
        report['quality'] = final['quality']; report['stratified'] = final['stratified']
        report['sampled_distance_gate_passed'] = guard['sampled_distance_gate_passed']
        from .export_regions import export as regions
        report['regions'] = regions(stage/'assembled', out/'regions')
        report['regional_directory'] = str((out/'regions').resolve())
        if report['regions']['exported_faces'] != report['step']['topology']['faces']:
            raise ValueError('Regional export does not cover the whole reconstructed STEP')
        if creo_root is not None:
            from .creo_validation import creo_check, native_body_check, export_native_part
            report['creo'] = creo_check(out/'step/model.step', out/'creo', creo_root, timeout=480,
                                       boundary_preference='XYZ', profile_mode=profile_mode)
            part = next((out/'creo/audit/import').glob('*.prt.*'))
            report['creo_native_body'] = native_body_check(part, out/'creo/native_body', creo_root, native_probe)
            if not report['creo_native_body']['native_closed_solid']:
                raise ValueError('Actual Creo import did not create a closed active solid')
            report['creo_raster'] = str(export_native_part(part, out/'creo/raster', creo_root, format='jpeg'))
        report.update(status='DiagnosticOnly',
            native_solid_verified=report.get('creo_native_body', {}).get('native_closed_solid', False))
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                      error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['seconds'] = time.perf_counter()-start
        dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stl', required=True); p.add_argument('--partition', required=True)
    p.add_argument('--output', required=True)
    reference = p.add_mutually_exclusive_group(required=True)
    reference.add_argument('--reference'); reference.add_argument('--reference-faces', type=int)
    p.add_argument('--smoothing', type=float, default=.12)
    p.add_argument('--workers', type=int, default=4); p.add_argument('--rounds', type=int, default=12)
    cache = p.add_mutually_exclusive_group()
    cache.add_argument('--from-fit'); cache.add_argument('--surface-fit')
    p.add_argument('--prior-fit', help='Optional compatible support checkpoint for bounded coefficient backtracking')
    p.add_argument('--distance-repairs', type=int, default=3)
    p.add_argument('--distance-samples-per-face', type=int, default=1024)
    p.add_argument('--distance-seed', type=int, default=810572)
    p.add_argument('--no-render', action='store_true')
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    p.add_argument('--profile-mode', choices=['installed', 'preserve_splines'], default='installed')
    p.add_argument('--boundary-mode', choices=['chain', 'fair-network', 'fair-chains'], default='chain')
    p.add_argument('--normal-field', choices=['vertex', 'crease-fans'], default='vertex')
    p.add_argument('--normal-filter-steps', type=int, default=0)
    p.add_argument('--support-solver', choices=['least-squares', 'bounded'], default='least-squares')
    p.add_argument('--boundary-cache')
    p.add_argument('--normal-weight', type=float, default=.001)
    p.add_argument('--fairness', type=float, default=1e-6)
    p.add_argument('--boundary-plane-budget', type=float)
    p.add_argument('--boundary-normal-weight', type=float, default=0.)
    p.add_argument('--normal-angle-limit', type=float)
    p.add_argument('--fairness-domain', choices=['rectangle', 'source'], default='rectangle')
    p.add_argument('--area-weighted', action='store_true')
    p.add_argument('--fairness-order', type=int, choices=[2, 3], default=2)
    p.add_argument('--repair-charts', type=int, nargs='+', default=[])
    p.add_argument('--repair-smooth-joins', action='store_true')
    p.add_argument('--refit-regions', type=int, nargs='+')
    a = p.parse_args()
    from .distance_guard import Options
    reconstruct(a.stl, a.partition, a.output, reference=a.reference, reference_faces=a.reference_faces,
                smoothing=a.smoothing, workers=a.workers, rounds=a.rounds, from_fit=a.from_fit,
                render=not a.no_render, creo_root=a.creo_root, native_probe=a.native_probe,
                profile_mode=a.profile_mode, boundary_mode=a.boundary_mode, normal_field=a.normal_field,
                normal_filter_steps=a.normal_filter_steps, support_solver=a.support_solver, boundary_cache=a.boundary_cache,
                boundary_plane_budget=a.boundary_plane_budget, repair_charts=a.repair_charts,
                repair_smooth_joins=a.repair_smooth_joins, refit_regions=a.refit_regions,
                surface_fit=a.surface_fit, prior_fit=a.prior_fit,
                distance_options=Options(max_repairs=a.distance_repairs, samples_per_face=a.distance_samples_per_face,
                                         seed=a.distance_seed),
                config=None if a.from_fit or a.surface_fit else Config(distance=.03, boundary=.005, controls=(65,),
                    normal_weight=a.normal_weight, fairness=a.fairness, boundary_normal_weight=a.boundary_normal_weight,
                    normal_angle_limit=a.normal_angle_limit, fairness_domain=a.fairness_domain,
                    area_weighted=a.area_weighted, fairness_order=a.fairness_order))
