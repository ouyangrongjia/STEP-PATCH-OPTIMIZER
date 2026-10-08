"""Fast support-normal diagnostic on the original shared mesh edges.

Uses the same source edge samples across unchanged chart layouts. This is a
pre-export diagnostic; it cannot replace actual STEP trim/Creo verification.
"""
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump
from .partition_fit import incidence, parameters, positions
from .regional_quality import weighted_stats


def audit(run, output, regions=None, samples=17, smooth_degrees=10.):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    data = np.load(meta['partition']); v, f, labels = (data[k] for k in
        ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    charts = np.load(root/'chart_index.npz')['face_chart']
    edges, owners = incidence(f)
    keep = charts[owners[:, 0]] != charts[owners[:, 1]]
    if regions is not None:
        keep &= np.any(np.isin(labels[owners], regions), axis=1)
    edges, owners = edges[keep], owners[keep]; chart_pair = charts[owners]
    w = np.linspace(0, 1, samples)
    points = v[edges[:, 0], None]*(1-w[None, :, None])+v[edges[:, 1], None]*w[None, :, None]
    normals = np.empty((len(edges), 2, samples, 3)); xyz = np.empty_like(normals)
    paths = sorted((root/'charts').glob('*.npz'))
    for chart in np.unique(chart_pair):
        rec = dict(np.load(paths[chart])); edge, side = np.where(chart_pair == chart)
        q = parameters(points[edge].reshape(-1, 3), rec)
        normal = np.cross(positions(q, rec, 1, 0), positions(q, rec, 0, 1))
        normal /= np.linalg.norm(normal, axis=1)[:, None]
        normals[edge, side] = normal.reshape(-1, samples, 3)
        xyz[edge, side] = positions(q, rec).reshape(-1, samples, 3)
    angle = np.degrees(np.arccos(np.clip(np.sum(normals[:, 0]*normals[:, 1], axis=2), -1, 1)))
    gap = np.linalg.norm(xyz[:, 0]-xyz[:, 1], axis=2)
    fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    fn /= np.linalg.norm(fn, axis=1)[:, None]
    dihedral = np.degrees(np.arccos(np.clip(np.sum(fn[owners[:, 0]]*fn[owners[:, 1]], axis=1), -1, 1)))
    length = np.linalg.norm(v[edges[:, 1]]-v[edges[:, 0]], axis=1)
    weights = np.broadcast_to(length[:, None]/samples, angle.shape)
    smooth = dihedral <= smooth_degrees
    region_join = labels[owners[:, 0]] != labels[owners[:, 1]]
    result = dict(source_fit=str(root.resolve()), regions=regions, samples_per_edge=samples,
        status='support_diagnostic_only', source_smooth_dihedral_degrees=smooth_degrees,
        source_edge_count=len(edges), seconds=time.perf_counter()-started,
        scope='Unsmoothed source-edge UV samples; not exported STEP or certified continuity. Source dihedral is a feature proxy.')
    for name, selected in [('all', np.ones(len(edges), bool)), ('source_smooth', smooth),
                           ('regional_source_smooth', smooth & region_join)]:
        result[name] = dict(normal_degrees=weighted_stats(angle[selected].ravel(), weights[selected].ravel()),
                            gap_mm=weighted_stats(gap[selected].ravel(), weights[selected].ravel())) if np.any(selected) else None
    np.savez_compressed(out/'samples.npz', source_edges=edges, chart_pairs=chart_pair,
                        region_pairs=labels[owners], normal_degrees=angle, gap_mm=gap,
                        source_dihedral_degrees=dihedral, edge_length_mm=length)
    dump(out/'run.json', result); return result


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', nargs='+', type=int)
    a = p.parse_args(); print(audit(a.run, a.output, a.regions))
