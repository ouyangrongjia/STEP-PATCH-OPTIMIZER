"""Fresh mesh-based geometry/continuity checks for the fixed-partition fitter."""
import argparse
import json
from pathlib import Path
import time
import numpy as np

from .mesh import Locator, dump, sample, stats, vertex_normals
from .partition_fit import incidence, load_partition, parameters, positions


def load_records(folder):
    result = []
    for file in sorted((Path(folder)/'charts').glob('*.npz')):
        with np.load(file, allow_pickle=False) as a:
            rec = {k: a[k] for k in a.files}
        rec['region'] = int(rec['region']); result.append(rec)
    return result


def evaluate_uv(uv, rec):
    p = positions(uv, rec)
    du = positions(uv, rec, 1, 0); dv = positions(uv, rec, 0, 1)
    cross = np.cross(du, dv); length = np.linalg.norm(cross, axis=1)
    return p, cross/np.maximum(length[:, None], 1e-30), length


def angle(a, b):
    return np.degrees(np.arccos(np.clip(np.einsum('ij,ij->i', a, b), -1., 1.)))


def metrics(folder, count=100000, seed=3093001):
    folder = Path(folder); start = time.perf_counter()
    run = json.loads((folder/'run.json').read_text(encoding='utf-8'))
    v, f, labels, _ = load_partition(run['partition'], run['stl'])
    records = load_records(folder)
    with np.load(folder/'chart_index.npz') as a:
        chart = a['face_chart']; stored_labels = a['source_labels']
    if not np.array_equal(stored_labels, labels):
        raise ValueError('Fixed partition labels changed')
    points, raw_normals, _, tri_ids, bary = sample(v, f, count, seed)
    region_normals = {}
    for region in np.unique(labels):
        tri = f[labels == region]; used = np.unique(tri)
        region_normals[int(region)] = (used, vertex_normals(v[used], np.searchsorted(used, tri)))
    predicted = np.empty_like(points); normals = np.empty_like(points); source_normals = np.empty_like(points)
    curvature = np.empty(count); jac = np.empty(count)
    for ci, rec in enumerate(records):
        ids = np.flatnonzero(chart[tri_ids] == ci)
        if not len(ids):
            continue
        q = parameters(points[ids], rec)
        predicted[ids], normals[ids], jac[ids] = evaluate_uv(q, rec)
        used, nn = region_normals[rec['region']]
        interpolated = np.einsum('ij,ijk->ik', bary[ids], nn[np.searchsorted(used, f[tri_ids[ids]])])
        interpolated /= np.maximum(np.linalg.norm(interpolated, axis=1)[:, None], 1e-30)
        source_normals[ids] = interpolated
        du = positions(q, rec, 1, 0); dv = positions(q, rec, 0, 1)
        e = np.sum(du*du, axis=1); ff = np.sum(du*dv, axis=1); g = np.sum(dv*dv, axis=1)
        l = np.sum(positions(q, rec, 2, 0)*normals[ids], axis=1)
        m = np.sum(positions(q, rec, 1, 1)*normals[ids], axis=1)
        n = np.sum(positions(q, rec, 0, 2)*normals[ids], axis=1)
        curvature[ids] = np.abs((e*n-2*ff*m+g*l)/(2*np.maximum(e*g-ff*ff, 1e-30)))
    distance = np.linalg.norm(predicted-points, axis=1)
    normal_error = angle(normals, source_normals)
    locator = Locator(v, f)
    _, reverse_distance, _, _ = locator.closest(predicted)
    edges, owners = incidence(f); adjacent = chart[owners]
    seam_ids = np.flatnonzero(adjacent[:, 0] != adjacent[:, 1])
    # Both sides evaluated at identical source positions along every seam.
    t = np.array([0., .125, .25, .5, .75, .875, 1.])
    ep = (v[edges[seam_ids, 0], None]*(1-t[None, :, None])+
          v[edges[seam_ids, 1], None]*t[None, :, None]).reshape(-1, 3)
    seam_normals = [np.empty_like(ep), np.empty_like(ep)]
    seam_positions = [np.empty_like(ep), np.empty_like(ep)]
    for side in range(2):
        use = np.repeat(adjacent[seam_ids, side], len(t))
        for ci in np.unique(use):
            ids = np.flatnonzero(use == ci); rec = records[int(ci)]
            seam_positions[side][ids], seam_normals[side][ids], _ = evaluate_uv(parameters(ep[ids], rec), rec)
    seam_angle = angle(*seam_normals)
    seam_gap = np.linalg.norm(seam_positions[0]-seam_positions[1], axis=1)
    internal = np.repeat(labels[owners[seam_ids, 0]] == labels[owners[seam_ids, 1]], len(t))
    by_region = []
    for region in np.unique(labels):
        mask = labels[tri_ids] == region
        by_region.append(dict(region=int(region), samples=int(mask.sum()), distance_mm=stats(distance[mask]),
                              normal_degrees=stats(normal_error[mask])))
    result = dict(samples=count, seed=seed, fitting_samples_reused=False,
                  source_to_fit_correspondence_mm=stats(distance), fitted_to_source_mesh_mm=stats(reverse_distance),
                  normal_to_region_mesh_degrees=stats(normal_error), normal_to_flat_triangles_degrees=stats(angle(normals, raw_normals)),
                  within_region_seam_degrees=stats(seam_angle[internal]), region_interface_degrees=stats(seam_angle[~internal]),
                  seam_surface_gap_mm=stats(seam_gap), sampled_abs_mean_curvature_per_mm=stats(curvature),
                  singular_samples=int(np.sum(jac < 1e-12)), regions=by_region,
                  correspondence_note='Forward distance is a conservative graph correspondence, not a nearest-surface Hausdorff claim. Reverse distance uses the exact discrete-triangle locator at independently drawn domain points.',
                  trim_note='Shared trims retain original piecewise-linear mesh boundaries; smoothness of region interfaces is not forced.',
                  seconds=time.perf_counter()-start)
    dump(folder/'quality.json', result)
    np.savez_compressed(folder/'quality_samples.npz', source=points, fitted=predicted,
                        distance=distance, reverse_distance=reverse_distance, normal_degrees=normal_error,
                        triangle_ids=tri_ids, seed=seed)
    print(json.dumps({k: value for k, value in result.items() if k != 'regions'}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True)
    parser.add_argument('--samples', type=int, default=100000); parser.add_argument('--seed', type=int, default=3093001)
    args = parser.parse_args(); metrics(args.run, args.samples, args.seed)


if __name__ == '__main__':
    main()
