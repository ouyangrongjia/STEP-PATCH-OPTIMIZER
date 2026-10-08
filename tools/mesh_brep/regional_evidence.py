"""Compare actual trimmed STEP normals at identical source-region samples."""
import argparse
from pathlib import Path
import numpy as np
from . import cad
from .mesh import Locator, dump, stats, sample
from .source_normals import crease_fan_normals
from .step_quality import tessellate, native


def compare(partition, region, models, output, count=12000):
    data = np.load(partition)
    v, f, labels = (data[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    selected = np.flatnonzero(labels == region)
    if not len(selected):
        raise ValueError('Empty supplied region')
    corners = crease_fan_normals(v, f)
    # Uniform area samples, fixed before inspecting any candidate. The raw
    # crease-separated source field is independent of the fitting filter.
    points, _, _, ids, bary = sample(v, f[selected], count, 810081)
    target = np.einsum('ij,ijk->ik', bary, corners[selected[ids]])
    target /= np.linalg.norm(target, axis=1)[:, None]
    results = []
    for path in models:
        shape = cad.read_generated(path)
        mesh, surfaces, signs = tessellate(shape, deflection=.001)
        locator = Locator(mesh['points'], mesh['triangles'])
        _, distance, tri_ids, weights = locator.closest(points)
        owners = mesh['owners'][tri_ids]
        uv = np.einsum('ij,ijk->ik', weights, mesh['uv'][mesh['triangles'][tri_ids]])
        normals = np.empty_like(points); fitted = np.empty_like(points)
        for fi in np.unique(owners):
            where = np.flatnonzero(owners == fi)
            fitted[where], normals[where], _, _ = native(surfaces[fi], uv[where], signs[fi])
        angles = np.degrees(np.arccos(np.clip(np.sum(normals*target, axis=1), -1, 1)))
        results.append(dict(model=str(Path(path).resolve()), faces=len(surfaces),
            source_to_native_sample_distance_mm=stats(np.linalg.norm(fitted-points, axis=1)),
            source_normal_difference_degrees=stats(angles),
            samples_above_15_degrees=int(np.sum(angles > 15)),
            samples_above_30_degrees=int(np.sum(angles > 30)),
            delabella_retried_faces=mesh['delabella_retried_faces'].tolist()))
    report = dict(region=int(region), samples=count, source_CAD_used=False, models=results,
        scope='Same area-uniform source samples and raw crease-fan normals. Native STEP D1 at UV interpolated from closest trimmed tessellation (0.001 mm); sampled evidence, not a continuous bound.')
    dump(output, report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--partition', required=True); p.add_argument('--region', type=int, required=True)
    p.add_argument('--models', nargs='+', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); print(compare(a.partition, a.region, a.models, a.output))
