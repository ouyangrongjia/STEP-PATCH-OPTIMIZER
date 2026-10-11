"""Bounded incremental boundary optimization and one CAD validation pass."""
import argparse
from concurrent.futures import ProcessPoolExecutor
from contextlib import redirect_stdout
import json
from pathlib import Path
import time

from .boundary_refine import Config, refine
from .mesh import dump


def _quality(step, partition, output):
    from .step_quality import audit
    with Path(output).with_suffix('.log').open('w', encoding='utf-8') as log, redirect_stdout(log):
        return audit(step, partition, output, deflection=.005, count=30000)


def _regions(assembly, output):
    from .export_regions import export
    return export(assembly, output)


def completed_modelcheck_after_timeout(output, installation):
    """Accept a completed XML result, while retaining the process timeout.

    The input import must have exited successfully. No report is synthesized
    from process status, and incomplete/missing XML remains an error.
    """
    import xml.etree.ElementTree as ET
    root = Path(output)
    imported = json.loads((root/'audit/import_process.json').read_text(encoding='utf-8-sig'))
    checked = json.loads((root/'audit/modelcheck_process.json').read_text(encoding='utf-8-sig'))
    if imported['timed_out'] or imported['exit_code'] != 0 or not checked['timed_out']:
        raise ValueError('Only a completed import and timed-out ModelCHECK can use a saved complete report')
    paths = list((root/'audit/modelcheck').glob('*.p.xml'))
    if len(paths) != 1:
        raise ValueError('Missing unambiguous completed ModelCHECK XML')
    checks = {c.attrib['name']: c for c in ET.parse(paths[0]).findall('.//check')}
    params = {i.findtext('info1'): i.findtext('info2') for i in checks['PARAM_INFO'].findall('item')}
    state, score = params['PTC_VAL_IMP_PART_STATUS'], params['PTC_VAL_IMP_SCORE']
    result = dict(part_status=state, score=score, geometry_check_features=int(checks['GEOM_CHECKS'].findtext('ans')),
        profile=str(root/'import_mm.dip'), report=str(paths[0]), profile_mode='installed', profile_modified=False,
        profile_source=str(Path(installation)/'Common Files/x86e_win64/mworks/mw_import_mm.dip'),
        import_validation_passed=state == 'OK' and score == 'PASS',
        native_body_check_required=True, solid_import_passed=None,
        modelcheck_process_timed_out=True, process_failure_preserved=True,
        completed_report_read_after_timeout=True, timeout_process_record=checked)
    dump(root/'run.json', result)
    return result


def _creo(step, output, installation, probe):
    from .creo_validation import creo_check, native_body_check
    root = Path(output); started = time.perf_counter()
    try:
        imported = creo_check(step, root, installation, timeout=300,
                              boundary_preference='XYZ', profile_mode='installed')
    except RuntimeError:
        imported = completed_modelcheck_after_timeout(root, installation)
    part = next((root/'audit/import').glob('*.prt.*'))
    body = native_body_check(part, root/'native_body', installation, probe, timeout=120)
    return dict(imported=imported, native_body=body, seconds=time.perf_counter()-started)


def reconstruct(source, output, *, config=None, target_seconds=300., edge_length=None,
                creo_root=None, native_probe=None):
    from .distance_guard import validate_fit
    from .graph_reassemble import reconstruct as assemble
    from .graph_export import export
    started = time.perf_counter(); root, out = Path(source).resolve(), Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', algorithm='incremental-native-boundary-v1', source_fit=str(root),
                  target_seconds=target_seconds, timing={}, can_apply=False,
                  commercial_reconstruction_used=False, source_geometry_reused=True,
                  surface_refit_rounds=0, adaptive_subdivision_rounds=0,
                  timing_scope='Input verification, all-curve optimization, swept-strip and wire guards, native assembly, STEP roundtrip, independent distance, regional export and optional Creo import/body verification.')
    dump(out/'run.json', report)
    def stage(name, function, *args, **kwargs):
        tick = time.perf_counter()
        value = function(*args, **kwargs)
        report['timing'][name] = time.perf_counter()-tick
        dump(out/'run.json', report)
        print(dict(stage=name, seconds=report['timing'][name], elapsed=time.perf_counter()-started), flush=True)
        return value
    try:
        stage('verify_input', validate_fit, root, meta['partition'], meta['stl'])
        certificates = {}
        fair = stage('boundary_optimization_and_guards', refine, root, out/'fit', config, certificates)
        report['boundary'] = {k: v for k, v in fair.items() if k not in ('per_curve', 'wire_rollback_curves')}
        native_context = {}
        assembly = stage('native_assembly', assemble, out/'fit', out/'assembled',
                         maximum_edge_length=edge_length, smooth_segments=edge_length is None,
                         write_preview=False, curve_certificates=certificates,
                         native_context=native_context, binary_native=True)
        if not assembly['watertight']:
            raise ValueError('Native topology or boundary tolerance failed')
        step = stage('step_roundtrip', export, out/'assembled', out/'step', native_context=native_context)
        report['assembly'], report['step'] = assembly, step
        if not step['watertight'] or step['topology']['faces'] != len(list((root/'charts').glob('*.npz'))):
            raise ValueError('STEP changed the face count or failed topology/tolerance')
        # Independent consumers share one final STEP; none causes another fit.
        tick = time.perf_counter()
        with ProcessPoolExecutor(max_workers=3) as pool:
            tasks = dict(quality=pool.submit(_quality, out/'step/model.step', meta['partition'], out/'quality'),
                         regions=pool.submit(_regions, out/'assembled', out/'regions'))
            if creo_root is not None:
                if native_probe is None:
                    raise ValueError('Creo verification needs its native-body probe')
                tasks['creo'] = pool.submit(_creo, out/'step/model.step', out/'creo', creo_root, native_probe)
            for name, task in tasks.items():
                report[name] = task.result()
                dump(out/'run.json', report)
        report['timing']['independent_validation_and_regional_export'] = time.perf_counter()-tick
        report['distance_passed'] = report['quality']['reverse_native_surface_distance_mm']['max'] <= meta['config']['distance']
        report['creo_passed'] = (None if 'creo' not in report else
            report['creo']['native_body']['native_closed_solid'] and
            report['creo']['imported']['geometry_check_features'] == 0 and
            report['creo']['imported']['import_validation_passed'])
        report['geometry_gates_passed'] = bool(report['distance_passed'] and report['creo_passed'] is not False)
        report['status'] = 'DiagnosticOnly' if report['geometry_gates_passed'] else 'rejected'
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['seconds'] = time.perf_counter()-started
        report['time_target_met'] = report['seconds'] <= target_seconds
        report['timing_policy'] = 'Measured end-to-end target, not a hard interrupt of native CAD calls. Fixed solver limits; no surface refinement or repair retries.'
        dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--source', required=True); p.add_argument('--output', required=True)
    p.add_argument('--workers', type=int, default=8); p.add_argument('--target-seconds', type=float, default=300.)
    p.add_argument('--edge-length', type=float, help='Optional compact exchange-edge experiment; default keeps source-edge subdivisions')
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    a = p.parse_args()
    result = reconstruct(a.source, a.output, config=Config(workers=a.workers), target_seconds=a.target_seconds,
                         edge_length=a.edge_length, creo_root=a.creo_root, native_probe=a.native_probe)
    print({k: result[k] for k in ('status', 'seconds', 'time_target_met')}, flush=True)
