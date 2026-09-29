"""Measure the omitted full STEP -> selected original region extraction cost."""
import argparse
import json
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, write_step, shell, shapes, TopAbs_FACE, dump
from .generate import cad_region


def main():
    p = argparse.ArgumentParser(); p.add_argument('--input-audit', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); out = Path(a.output); out.mkdir(parents=True, exist_ok=False)
    audit = json.loads(Path(a.input_audit).read_text(encoding='utf-8')); stages = {}; start = time.perf_counter()
    shape = read_step(audit['source']); stages['full_step_read'] = time.perf_counter()-start
    tick = time.perf_counter(); faces = shapes(shape, TopAbs_FACE); region = shell([faces[i] for i in audit['selected_face_ids']])
    stages['region_extract'] = time.perf_counter()-tick
    tick = time.perf_counter(); write_step(out/'original_region.step', region); stages['region_step_write'] = time.perf_counter()-tick
    report = cad_region(out/'original_region.step', out/'mesh', .003)
    stages['region_step_read'] = report['step_read_seconds']; stages['mesh_and_layout'] = report['mesh_and_layout_seconds']
    dump(out/'profile.json', dict(stages_seconds=stages, total_seconds=time.perf_counter()-start,
                                  note='Full original CAD import and region extraction included. No reconstruction or geometry tuning.'))


if __name__ == '__main__':
    main()
