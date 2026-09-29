"""Re-run the unchanged five-patch algorithm with disjoint stage timers."""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

import continuity_fit as fit
import designer_layout
import hard_boundary
import prepare
from geometry import dump, read_step, write_step, shapes, TopAbs_FACE, topology


class Timer:
    def __init__(self):
        self.stages = {}; self.stack = []

    def call(self, label, function, *args, **kwargs):
        frame = [time.perf_counter(), 0.]; self.stack.append(frame)
        try:
            return function(*args, **kwargs)
        finally:
            elapsed = time.perf_counter()-frame[0]; self.stack.pop()
            self.stages[label] = self.stages.get(label, 0.)+elapsed-frame[1]
            if self.stack:
                self.stack[-1][1] += elapsed

    def wrap(self, module, name, label):
        original = getattr(module, name)
        def timed(*args, **kwargs):
            return self.call(label, original, *args, **kwargs)
        setattr(module, name, timed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True); parser.add_argument('--reference', required=True)
    parser.add_argument('--baseline', required=True); parser.add_argument('--output', required=True)
    args = parser.parse_args(); started = time.perf_counter(); timer = Timer()
    out = Path(args.output); out.mkdir(parents=True, exist_ok=False)
    reference = Path(args.reference).resolve(); (out/'control').mkdir()
    shutil.copy2(reference/'control/patch_faces.step', out/'control/patch_faces.step')
    protocol = json.loads((reference/'protocol.json').read_text(encoding='utf-8'))
    protocol['previous_run'] = str(Path(protocol['previous_run']).resolve()); dump(out/'protocol.json', protocol)
    for module in (prepare, fit, designer_layout):
        timer.wrap(module, 'read_step', 'step_read')
        timer.wrap(module, 'write_step', 'step_write')
        timer.wrap(module, 'topology', 'topology_check')
    argv = sys.argv
    sys.argv = ['prepare.py', '--source', args.source, '--baseline', args.baseline, '--output', str(out)]
    try:
        timer.call('sampling_and_reference_preparation', prepare.main)
    finally:
        sys.argv = argv
    for module, name, label in [
        (fit, 'fixed_start', 'layout'), (fit, 'variable_maps', 'matrix_assembly'),
        (hard_boundary, 'parameterize', 'parameterization'), (fit, 'assemble', 'matrix_assembly'),
        (fit, 'export_layout', 'brep_construct'), (fit, 'differential', 'geometry_check'),
        (fit, 'edge_checks', 'geometry_check'), (fit, 'seam_quality', 'geometry_check')]:
        timer.wrap(module, name, label)
    assembly = fit.assemble; last_assembly = [None]
    def measured_assembly(*args, **kwargs):
        result = assembly(*args, **kwargs); last_assembly[0] = time.perf_counter(); return result
    fit.assemble = measured_assembly
    original_splu = fit.splu
    def factorize(matrix):
        elapsed = time.perf_counter()-last_assembly[0]
        timer.stages['matrix_assembly'] += elapsed
        if timer.stack:
            timer.stack[-1][1] += elapsed
        factor = timer.call('solve', original_splu, matrix)
        class Factor:
            def solve(self, rhs):
                return timer.call('solve', factor.solve, rhs)
        return Factor()
    fit.splu = factorize
    config = next(c for c in protocol['configurations'] if c['name'] == 'aligned_inner_cubic')
    timer.call('fit_bookkeeping', fit.fit, out, config['name'], config)
    from integrate import integrate
    original = timer.call('step_read', read_step, args.source)
    selected = set(json.loads((out/'input_audit.json').read_text(encoding='utf-8'))['selected_face_ids'])
    remaining = [f for i, f in enumerate(shapes(original, TopAbs_FACE)) if i not in selected]
    patch = timer.call('step_read', read_step, out/config['name']/'patch_faces.step')
    whole, report = timer.call('brep_construct', integrate, patch, remaining)
    timer.call('step_write', write_step, out/config['name']/'whole_model_diagnostic.step', whole)
    reread = timer.call('step_read', read_step, out/config['name']/'whole_model_diagnostic.step')
    report['after_step_read'] = timer.call('topology_check', topology, reread)
    dump(out/config['name']/'integration.json', report)
    elapsed = time.perf_counter()-started
    result = dict(stages_seconds=timer.stages, uninstrumented_seconds=elapsed-sum(timer.stages.values()),
                  wall_before_independent_geometry_and_cad_seconds=elapsed,
                  note='Disjoint nested wall timers; solve includes factorization and back substitution. Sampling includes exact-surface sampling, reference triangulation and NPZ preparation. Startup and input copying remain in the explicit uninstrumented remainder. Independent confirmation and CAD are separately measured.')
    dump(out/'timing.json', result); print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
