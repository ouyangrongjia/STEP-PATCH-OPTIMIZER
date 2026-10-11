"""Bounded one-pass merging followed by one independent CAD acceptance pass."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time
from .fast_merge import Budget, merge
from .distance_guard import Options, validate_fit
from .mesh import dump


def reconstruct(source, output, *, reference_faces, budget=None, merged_fit=None,
                creo_root=None, native_probe=None):
    budget = budget or Budget(); budget.validate()
    started = time.perf_counter(); root, out = Path(source), Path(output)
    out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', can_apply=False, source_fit=str(root.resolve()),
                  algorithm='bounded-merge-single-validation-v1', budget=asdict(budget),
                  local_repair_rounds=0, continuity_optimization_rounds=0,
                  Geomagic_API_used=False, visual_review_required=True)
    dump(out/'run.json', report)
    try:
        validate_fit(root, meta['partition'], meta['stl'])
        if merged_fit is None:
            merged_fit = out/'fit'; report['merging'] = merge(root, merged_fit, budget)
        else:
            merged_fit = Path(merged_fit)
            report['merging'] = json.loads((merged_fit/'merge.json').read_text(encoding='utf-8'))
            if (Path(report['merging']['source_fit']).resolve() != root.resolve()
                    or report['merging']['status'] != 'fitted_pending_CAD_validation'):
                raise ValueError('Merge checkpoint does not belong to this source or is incomplete')
            report['cached_merge_seconds'] = report['merging']['seconds']
        report['merge_checkpoint'] = str(Path(merged_fit).resolve()); dump(out/'run.json', report)
        from .skeleton_reconstruct import reconstruct as validate
        validation_start = time.perf_counter()
        report['validation'] = validate(meta['stl'], meta['partition'], out/'validated',
            reference_faces=reference_faces, surface_fit=merged_fit, render=False,
            distance_options=Options(max_repairs=0, preventive_repair=False),
            creo_root=creo_root, native_probe=native_probe)
        report['validation_seconds'] = time.perf_counter()-validation_start
        if (creo_root is not None and report['validation']['creo']['geometry_check_features'] != 0):
            raise ValueError('CAD candidate has Creo geometry-check features; retain the baseline')
        report.update(status='DiagnosticOnly', model=report['validation']['model'],
                      remaining_review='Actual STEP visual smoothness; no whole-model G1/G2 proof.')
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--reference-faces', type=int, required=True); p.add_argument('--merged-fit')
    p.add_argument('--seconds', type=float, default=180.); p.add_argument('--workers', type=int, default=4)
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    a = p.parse_args(); reconstruct(a.run, a.output, reference_faces=a.reference_faces,
        budget=Budget(seconds=a.seconds, workers=a.workers), merged_fit=a.merged_fit,
        creo_root=a.creo_root, native_probe=a.native_probe)
