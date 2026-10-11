"""Native v10: bounded transition fairing, feature-guided curves and verified STEP."""
import argparse
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import time

from .boundary_reconstruct import _quality, _regions, _creo
from .boundary_refine import Config as BoundaryConfig, refine
from .transition_fair import Config as SurfaceConfig, fair
from .mesh import dump


def reconstruct(source, output, workers=8, target_seconds=60., creo_root=None, native_probe=None):
    from .distance_guard import validate_fit
    from .graph_reassemble import reconstruct as assemble
    from .graph_export import export
    from .transition_guard import missing_triangulations, restore_neighborhood
    root, out = Path(source).resolve(), Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', can_apply=False, algorithm='native-transition-and-feature-boundary-v10',
                  source_fit=str(root), target_seconds=target_seconds, timing={},
                  priority=['watertight', 'surface_smoothness', 'performance', 'boundary_smoothness'],
                  source_geometry_reused=True, commercial_reconstruction_used=False,
                  timing_scope='Input verification through fresh STEP readback and independent geometry, regional exports and optional Creo validation.')
    def stage(name, function, *args, **kwargs):
        tick = time.perf_counter(); value = function(*args, **kwargs)
        report['timing'][name] = time.perf_counter()-tick; dump(out/'run.json', report)
        print(dict(stage=name, seconds=report['timing'][name], elapsed=time.perf_counter()-started), flush=True)
        return value
    dump(out/'run.json', report); runtime = {}
    try:
        stage('verify_input', validate_fit, root, meta['partition'], meta['stl'])
        support_error = meta['config']['boundary']*.6
        surface = stage('transition_fairing', fair, root, out/'surface',
                        SurfaceConfig(workers=workers, support_error=support_error), runtime)
        report['surface'] = {k: v for k, v in surface.items() if k != 'per_chart'}
        certificates = {}
        boundary_config = BoundaryConfig(movement=.025, fairness_length=.3, support_error=support_error,
                                         iterations=1, workers=workers, feature_weight=200., solver_seconds=.025,
                                         feature_fraction=.05)
        boundary = stage('boundary_fairing', refine, out/'surface', out/'fit', boundary_config, certificates, runtime)
        runtime.pop('pool').shutdown()
        report['boundary'] = {k: v for k, v in boundary.items() if k != 'per_curve'}
        context = {}
        assembly = stage('native_assembly', assemble, out/'fit', out/'assembled',
                         write_preview=False, curve_certificates=certificates, native_context=context,
                         binary_native=True, measure_volume=False, smooth_segments=True)
        report['assembly'] = assembly
        if not assembly['watertight']:
            raise ValueError('Native assembly failed its topology or tolerance gate')
        missing = stage('native_trim_preflight', missing_triangulations, context['shape'])
        assembly_dir = out/'assembled'
        if missing:
            failed_charts = [assembly['cad_face_charts'][fi] for fi in missing]
            report['native_trim_repair'] = stage('bounded_trim_repair', restore_neighborhood,
                root, out/'fit', failed_charts, certificates, out/'trim_repair')
            context = {}; assembly_dir = out/'assembled_guarded'
            assembly = stage('guarded_native_assembly', assemble, out/'fit', assembly_dir,
                write_preview=False, curve_certificates=certificates, native_context=context,
                binary_native=True, measure_volume=False, smooth_segments=True,
                segment_charts=report['native_trim_repair']['source_segmented_charts'])
            report['assembly'] = assembly
            if not assembly['watertight'] or stage('guarded_trim_preflight', missing_triangulations, context['shape']):
                raise ValueError('One bounded native trim repair did not pass')
            boundary = json.loads((out/'fit/boundary_refinement.json').read_text(encoding='utf-8'))
            report['boundary'] = {k: v for k, v in boundary.items() if k != 'per_curve'}
            report['surface']['final_retained_changed_charts'] = surface['charts_changed']-sum(
                surface['per_chart'][di]['accepted'] for di in report['native_trim_repair']['restored_charts'])
        step = stage('step_roundtrip', export, assembly_dir, out/'step', native_context=context,
                     measure_volume=False)
        report['step'] = step
        report['generation_seconds'] = time.perf_counter()-started
        report['generation_target_met'] = report['generation_seconds'] <= target_seconds
        if not step['watertight'] or step['topology']['faces'] != assembly['fitted_charts']:
            raise ValueError('STEP changed topology or exceeded the unchanged tolerance budget')
        dump(out/'run.json', report)
        tick = time.perf_counter()
        with ProcessPoolExecutor(max_workers=3) as pool:
            tasks = dict(quality=pool.submit(_quality, out/'step/model.step', meta['partition'], out/'quality'),
                         regions=pool.submit(_regions, assembly_dir, out/'regions'))
            if creo_root:
                if not native_probe:
                    raise ValueError('A native body probe is required for Creo validation')
                tasks['creo'] = pool.submit(_creo, out/'step/model.step', out/'creo', creo_root, native_probe)
            for name, task in tasks.items():
                report[name] = task.result(); dump(out/'run.json', report)
        report['timing']['independent_validation_and_exports'] = time.perf_counter()-tick
        report['distance_passed'] = report['quality']['reverse_native_surface_distance_mm']['max'] <= meta['config']['distance']
        report['creo_passed'] = (None if 'creo' not in report else bool(
            report['creo']['native_body']['native_closed_solid']
            and report['creo']['imported']['geometry_check_features'] == 0
            and report['creo']['imported']['import_validation_passed']))
        report['geometry_gates_passed'] = bool(report['distance_passed'] and report['creo_passed'] is not False)
        report['status'] = 'DiagnosticOnly' if report['geometry_gates_passed'] else 'rejected'
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        if 'pool' in runtime:
            runtime.pop('pool').shutdown()
        report['seconds'] = time.perf_counter()-started
        report['time_target_met'] = report['seconds'] <= target_seconds
        report['time_policy'] = 'One bounded support solve and one boundary proposal. At most one half-step guard; no patch subdivision or optimization retry. Volume integration is deferred to independent quality validation; full BRep checks and STEP readback remain mandatory.'
        dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--source', required=True); p.add_argument('--output', required=True)
    p.add_argument('--workers', type=int, default=8); p.add_argument('--target-seconds', type=float, default=60.)
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    a = p.parse_args()
    r = reconstruct(a.source, a.output, a.workers, a.target_seconds, a.creo_root, a.native_probe)
    print({k: r[k] for k in ('status', 'seconds', 'time_target_met')}, flush=True)
