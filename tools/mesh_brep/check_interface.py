"""Read-only original-neighbor continuity check of the assembled mesh patch."""
import argparse
import json
from pathlib import Path
from .adapt_interface import make_layout
from .mesh import dump
from geometry import read_step
from designer_audit import original_boundaries


def main():
    p = argparse.ArgumentParser()
    for name in ['reference', 'mesh-result', 'candidate', 'output']:
        p.add_argument('--'+name, required=True)
    a = p.parse_args(); reference, output = Path(a.reference), Path(a.output)
    if output.exists() or output.with_suffix('.summary.json').exists():
        raise FileExistsError(output)
    layout, _, _, _ = make_layout(a.mesh_result, read_step(reference/'original_region.step'))
    protocol = json.loads((reference/'protocol.json').read_text(encoding='utf-8'))
    report = original_boundaries(reference, protocol, {'candidate': read_step(a.candidate)},
                                 {'candidate': {'boundary_mapping': layout['boundary']}}, output=output)
    dump(output.with_suffix('.summary.json'), report)
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
