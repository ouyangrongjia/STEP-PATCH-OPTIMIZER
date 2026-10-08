"""Fixed STL partitions -> shared smooth skeleton -> regional supports -> solid.

The optional reference STEP is read only by the independent face counter. No
reference geometry or commercial fitting engine is passed to reconstruction.
"""
import argparse
from pathlib import Path
import time
from .mesh import dump
from .partition_fit import Config


def reconstruct(stl, partition, output, *, reference=None, reference_faces=None,
                config=None, smoothing=.12, workers=4, rounds=12, from_fit=None,
                render=True, creo_root=None, native_probe=None,
                profile_mode='installed'):
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter()
    report = dict(status='running', can_apply=False, algorithm='shared-skeleton-regional-solid-v1',
        stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()), units='mm',
        fitter_reads_source_CAD=False, commercial_reconstruction_used=False,
        visual_review_required=True, from_fit=None if from_fit is None else str(Path(from_fit).resolve()))
    dump(out/'run.json', report)
    try:
        if (reference is None) == (reference_faces is None):
            raise ValueError('Supply exactly one reference STEP or independently measured face count')
        if (creo_root is None) != (native_probe is None):
            raise ValueError('Creo validation requires both the Creo installation and the native body probe')
        if reference is not None:
            from .reference_catalog import transferred_face_count
            reference_faces = transferred_face_count(reference)
            report['reference_for_face_count_only'] = str(Path(reference).resolve())
        report['reference_faces'] = int(reference_faces)
        if config is None and from_fit is None:
            config = Config(distance=.03, boundary=.005, controls=(65,), normal_weight=.001, fairness=1e-6)
        from .skeleton_graph import reconstruct as fit
        fitted = fit(partition, stl, out/'fit', smoothing=smoothing, workers=workers,
                     rounds=rounds, face_limit=int(reference_faces)-1, config=config, from_fit=from_fit)
        report['fitting'] = fitted
        if not fitted['complete_source_coverage'] or not fitted['partition_labels_unchanged']:
            raise ValueError('Incomplete source partition coverage')
        from .graph_reassemble import reconstruct as assemble
        from .graph_export import export
        # Exact subcurves preserve the same smooth shared geometry. Short trim
        # spans avoid the long-edge exchange failure observed in actual Creo.
        report['assembly'] = assemble(fitted['source_fit'], out/'assembled',
                                     smooth_segments=True, write_preview=False)
        report['step'] = export(out/'assembled', out/'step')
        report['model'] = str((out/'step/model.step').resolve())
        report['face_budget_passed'] = report['step']['topology']['faces'] < int(reference_faces)
        if not report['step']['watertight'] or not report['face_budget_passed']:
            raise ValueError('STEP topology, measured tolerance or face-count gate failed')
        from .step_quality import audit
        report['quality'] = audit(out/'step/model.step', partition, out/'quality', render=render)
        from .export_regions import export as regions
        report['regions'] = regions(out/'assembled', out/'regions')
        report['regional_directory'] = str((out/'regions').resolve())
        if report['regions']['exported_faces'] != report['step']['topology']['faces']:
            raise ValueError('Regional export does not cover the whole reconstructed STEP')
        if creo_root is not None:
            from .creo_validation import creo_check, native_body_check, export_native_part
            report['creo'] = creo_check(out/'step/model.step', out/'creo', creo_root, timeout=480,
                                       boundary_preference='XYZ', profile_mode=profile_mode)
            part = next((out/'creo/audit/import').glob('*.prt.*'))
            report['creo_native_body'] = native_body_check(part, out/'creo/native_body', creo_root, native_probe)
            if report['creo_native_body']['native_closed_solid']:
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
    p.add_argument('--from-fit'); p.add_argument('--no-render', action='store_true')
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    p.add_argument('--profile-mode', choices=['installed', 'preserve_splines'], default='installed')
    a = p.parse_args()
    reconstruct(a.stl, a.partition, a.output, reference=a.reference, reference_faces=a.reference_faces,
                smoothing=a.smoothing, workers=a.workers, rounds=a.rounds, from_fit=a.from_fit,
                render=not a.no_render, creo_root=a.creo_root, native_probe=a.native_probe,
                profile_mode=a.profile_mode)
