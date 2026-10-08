"""Locally reparameterize ill-conditioned source-normal cones at bad seams."""
from dataclasses import replace
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump
from .partition_fit import Config
from .source_normals import crease_fan_normals
from .graph_join_audit import audit
from .graph_repair import repair


def condition(run, output, smooth_degrees=10., seam_degrees=20., minimum_axis_dot=.15):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    config = Config(**meta['config']); data = np.load(meta['partition'])
    v, f = data['mesh_vertex_original'], data['mesh_triangle']
    audit(root, out/'before', smooth_degrees=smooth_degrees)
    measured = np.load(out/'before/samples.npz')
    bad = ((measured['source_dihedral_degrees'] <= smooth_degrees)
           & (measured['normal_degrees'].max(axis=1) > seam_degrees))
    normals = crease_fan_normals(v, f, config.feature_angle); selected = []
    for chart in np.unique(measured['chart_pairs'][bad]):
        rec = dict(np.load(root/'charts'/f'{chart:05d}.npz'))
        if np.min(normals[rec['triangle_ids']]@rec['axes'][2]) < minimum_axis_dot:
            selected.append(int(chart))
    report = dict(status='running', source_fit=str(root.resolve()), source_CAD_read=False,
        selection=dict(source_smooth_degrees=smooth_degrees, seam_degrees=seam_degrees,
                       minimum_axis_dot=minimum_axis_dot, selected_charts=selected),
        scope='Source-derived hotspot selection; output seam audit is diagnostic, not a visual or CAD acceptance gate.')
    dump(out/'run.json', report)
    try:
        config = replace(config, normal_weight=max(.03, config.normal_weight),
                         boundary_normal_weight=max(.1, config.boundary_normal_weight))
        report['refinement'] = repair(root, out/'fit', refine_charts=selected, normal_cone=True, config=config)
        report['after'] = audit(out/'fit', out/'after', smooth_degrees=smooth_degrees)
        report.update(status='DiagnosticOnly', fit=str((out/'fit').resolve()), can_apply=False)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--smooth-degrees', type=float, default=10.); p.add_argument('--seam-degrees', type=float, default=20.)
    p.add_argument('--minimum-axis-dot', type=float, default=.15)
    a = p.parse_args(); condition(a.run, a.output, a.smooth_degrees, a.seam_degrees, a.minimum_axis_dot)
