"""Contextualize output seam angles against the original mesh dihedral.

This is a diagnostic classification, not a replacement acceptance gate. All
original seam metrics remain available, including sharp features and outliers.
"""
import json
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree
from .mesh import dump
from .partition_fit import incidence
from .regional_quality import weighted_stats


def audit(quality, fit, smooth_degrees=10., maximum_distance=.01):
    root, source = Path(quality), Path(fit)
    if (root/'source_join_context.json').exists():
        raise FileExistsError(root/'source_join_context.json')
    meta = json.loads((source/'run.json').read_text(encoding='utf-8')); data = np.load(meta['partition'])
    v, f = data['mesh_vertex_original'], data['mesh_triangle']
    edges, owners = incidence(f); charts = np.load(source/'chart_index.npz')['face_chart']
    keep = charts[owners[:, 0]] != charts[owners[:, 1]]; edges, owners = edges[keep], owners[keep]
    normals = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    normals /= np.linalg.norm(normals, axis=1)[:, None]
    angles = np.degrees(np.arccos(np.clip(np.sum(normals[owners[:, 0]]*normals[owners[:, 1]], axis=1), -1, 1)))
    records = json.loads((root/'edges.json').read_text(encoding='utf-8'))['records']
    records = [r for r in records if 'normal_degrees' in r]
    points = np.array([r['center'] for r in records]); a, b = v[edges[:, 0]], v[edges[:, 1]]
    _, choices = cKDTree((a+b)/2).query(points, k=min(16, len(edges)))
    delta = b[choices]-a[choices]; offset = points[:, None]-a[choices]
    t = np.clip(np.sum(offset*delta, axis=2)/np.sum(delta*delta, axis=2), 0, 1)
    distance = np.linalg.norm(offset-t[..., None]*delta, axis=2)
    best = np.argmin(distance, axis=1); nearest = choices[np.arange(len(points)), best]
    match = distance[np.arange(len(points)), best] <= maximum_distance
    smooth = match & (angles[nearest] <= smooth_degrees)
    weight = np.array([r['length_mm'] for r in records]); measured = np.array([r['normal_degrees']['p95'] for r in records])
    result = dict(source_fit=str(source.resolve()), source_mesh_dihedral_threshold_degrees=smooth_degrees,
        maximum_spatial_matching_distance_mm=maximum_distance, original_join_records=len(records),
        matched_join_length_fraction=float(weight[match].sum()/weight.sum()),
        source_smooth_join_length_fraction=float(weight[smooth].sum()/weight.sum()),
        smooth_join_per_edge_p95_degrees=weighted_stats(measured[smooth], weight[smooth]) if np.any(smooth) else None,
        smooth_join_excess_over_mesh_dihedral_degrees=weighted_stats(
            np.maximum(0, measured[smooth]-angles[nearest[smooth]]), weight[smooth]) if np.any(smooth) else None,
        scope='Nearest source boundary segment among 16 center candidates. Raw mesh dihedral is a proxy for feature intent, not exact CAD continuity. Unmatched and sharp joins remain in the original all-join report.')
    excess = np.maximum(0, measured-angles[nearest])
    candidates = np.flatnonzero(smooth)
    def describe(i):
        return dict(edge=records[i]['edge'], center=records[i]['center'], length_mm=float(weight[i]),
                    output_p95_degrees=float(measured[i]), source_dihedral_degrees=float(angles[nearest[i]]),
                    source_regions=data['face_instance_id'][owners[nearest[i]]].tolist(),
                    source_charts=charts[owners[nearest[i]]].tolist())
    result['largest_smooth_angles'] = [describe(i) for i in candidates[np.argsort(-measured[candidates])[:15]]]
    result['largest_length_times_excess'] = [describe(i) for i in candidates[np.argsort(-(weight*excess)[candidates])[:15]]]
    dump(root/'source_join_context.json', result); return result
