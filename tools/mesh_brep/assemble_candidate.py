"""Assemble a reconstructed local patch with an explicitly supplied CAD remainder.

This is a CAD-assisted assembly stage, separate from the mesh-only reconstruction.
It never raises the sewing tolerance to cover a failed boundary fit.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, write_step, shapes, topology, TopAbs_FACE
from integrate import integrate
from .mesh import dump


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--audit', required=True)
    p.add_argument('--patch', required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    out = Path(a.output); out.mkdir(parents=True, exist_ok=False)
    audit = json.loads(Path(a.audit).read_text(encoding='utf-8'))
    started = time.perf_counter()
    source = read_step(audit['source'])
    chosen = set(audit['selected_face_ids'])
    remaining = [f for i, f in enumerate(shapes(source, TopAbs_FACE)) if i not in chosen]
    patch = read_step(a.patch)
    read_seconds = time.perf_counter()-started
    shape, sewing = integrate(patch, remaining)
    before = topology(shape)
    accepted = before['solids'] == 1 and before['free_edges'] == 0 and before['brep_valid']
    report = dict(stage='CAD-assisted assembly of an independently reconstructed mesh patch',
                  source=audit['source'], patch=str(Path(a.patch).resolve()), selected_face_ids=sorted(chosen),
                  cad_read_seconds=read_seconds, sewing=sewing, before_write=before,
                  watertight_solid=accepted, output_written=False)
    if accepted:
        write_step(out/'sewn.step', shape)
        reread = topology(read_step(out/'sewn.step'))
        report.update(after_step_read=reread, output_written=True,
                      watertight_solid=reread['solids'] == 1 and reread['free_edges'] == 0 and reread['brep_valid'])
    report['wall_seconds'] = time.perf_counter()-started
    dump(out/'assembly.json', report)
    print(json.dumps(dict(watertight_solid=report['watertight_solid'], sewing=sewing,
                          topology={k: v for k, v in before.items() if k != 'face_valid'},
                          wall_seconds=report['wall_seconds'])), flush=True)


if __name__ == '__main__':
    main()
