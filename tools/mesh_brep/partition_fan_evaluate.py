"""Independent mesh-distance and native-normal checks of the fitted supports.

All samples here are independent of fitting. Forward distances are distances
to actual points on the supports, initialized by a knot-aware tessellation;
they are upper bounds on sampled point-to-surface distance, not Hausdorff
certificates. Reverse samples are stratified in patch parameter space.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np

from .mesh import Locator, dump, read_stl, sample, stats
from .splines import evaluate, tensor


def value(rec, uv, du=0, dv=0):
    return tensor(uv, rec['tu'], rec['tv'], du, dv)@rec['poles'].reshape(-1, 3)


def area_stats(values, weights):
    values, weights = np.asarray(values), np.asarray(weights)
    order = np.argsort(values); cumulative = np.cumsum(weights[order])/np.sum(weights)
    return dict(rms=float(np.sqrt(np.sum(weights*values*values)/np.sum(weights))),
                p50=float(np.interp(.5, cumulative, values[order])),
                p95=float(np.interp(.95, cumulative, values[order])),
                max=float(np.max(values)), min=float(np.min(values)))


def project(rec, points, uv):
    """Bounded damped Gauss-Newton refinement of a tessellation seed."""
    for _ in range(12):
        xyz = value(rec, uv); residual = xyz-points
        a, b = value(rec, uv, 1, 0), value(rec, uv, 0, 1)
        aa = np.sum(a*a, axis=1); ab = np.sum(a*b, axis=1); bb = np.sum(b*b, axis=1)
        x = np.sum(a*residual, axis=1); y = np.sum(b*residual, axis=1)
        denominator = np.maximum(aa*bb-ab*ab, 1e-30)
        delta = np.c_[(bb*x-ab*y)/denominator, (aa*y-ab*x)/denominator]
        delta /= np.maximum(1., np.linalg.norm(delta, axis=1)/.2)[:, None]
        best = np.sum(residual*residual, axis=1); next_uv = uv.copy()
        for damping in (1., .5, .125):
            trial = np.clip(uv-damping*delta, 0., 1.)
            error = np.sum((value(rec, trial)-points)**2, axis=1)
            take = error < best; next_uv[take] = trial[take]; best[take] = error[take]
        if np.max(abs(next_uv-uv)) < 1e-9:
            break
        uv = next_uv
    return value(rec, uv), uv


def metrics(folder, forward_samples=30000, reverse_per_patch=64, seed=3093051):
    folder = Path(folder); started = time.perf_counter()
    run = json.loads((folder/'run.json').read_text(encoding='utf-8'))
    if run['status'] != 'DiagnosticOnly' or not (folder/'fitting.json').exists():
        raise ValueError('Only complete reconstructions can receive whole-model metrics')
    records = []
    for path in sorted((folder/'patches').glob('*.npz')):
        with np.load(path, allow_pickle=False) as a:
            records.append({key: a[key] for key in a.files})
    vertices, triangles = read_stl(run['stl']); source_locator = Locator(vertices, triangles)
    rng = np.random.default_rng(seed)
    mesh_points = []; mesh_triangles = []; mesh_uv = []; mesh_owner = []; mesh_normals = []
    reverse_points = []; reverse_normals = []; reverse_distance = []; normal_errors = []
    area_weights = []; curvature = []
    singular = 0; offset = 0
    for pi, rec in enumerate(records):
        # Sample every polynomial span plus a uniform grid for large spans.
        axes = [np.unique(np.r_[np.linspace(0., 1., 9), np.unique(rec[t]),
                                (np.unique(rec[t])[1:]+np.unique(rec[t])[:-1])/2]) for t in ('tu', 'tv')]
        u, v = axes; q = np.stack(np.meshgrid(u, v, indexing='ij'), axis=-1).reshape(-1, 2)
        xyz, normal, _ = evaluate(rec['poles'], rec['tu'], rec['tv'], q)
        normal *= int(rec['orientation'])
        index = np.arange(len(q)).reshape(len(u), len(v))
        a, b, c, d = index[:-1, :-1], index[1:, :-1], index[1:, 1:], index[:-1, 1:]
        tri = np.concatenate([np.stack([a, b, c], axis=-1).reshape(-1, 3),
                              np.stack([a, c, d], axis=-1).reshape(-1, 3)])
        if int(rec['orientation']) < 0:
            tri = tri[:, [0, 2, 1]]
        mesh_points.append(xyz); mesh_normals.append(normal); mesh_uv.append(q)
        mesh_triangles.append(tri+offset); offset += len(q); mesh_owner.append(np.full(len(tri), pi, dtype=np.int32))
        random_uv = rng.uniform(.0001, .9999, (reverse_per_patch, 2))
        rp, rn, jac = evaluate(rec['poles'], rec['tu'], rec['tv'], random_uv)
        rn *= int(rec['orientation']); singular += int(np.sum(jac < 1e-12))
        area_weights.extend(jac/reverse_per_patch)
        du, dv = value(rec, random_uv, 1, 0), value(rec, random_uv, 0, 1)
        e, ff, g = np.sum(du*du, axis=1), np.sum(du*dv, axis=1), np.sum(dv*dv, axis=1)
        ll = np.sum(value(rec, random_uv, 2, 0)*rn, axis=1)
        mm = np.sum(value(rec, random_uv, 1, 1)*rn, axis=1)
        nn = np.sum(value(rec, random_uv, 0, 2)*rn, axis=1)
        curvature.extend(abs((e*nn-2*ff*mm+g*ll)/(2*np.maximum(jac*jac, 1e-30))))
        _, distances, ids, _ = source_locator.closest(rp)
        source_tri = vertices[triangles[ids]]
        tn = np.cross(source_tri[:, 1]-source_tri[:, 0], source_tri[:, 2]-source_tri[:, 0])
        tn /= np.maximum(np.linalg.norm(tn, axis=1)[:, None], 1e-30)
        normal_errors.extend(np.degrees(np.arccos(np.clip(np.sum(tn*rn, axis=1), -1., 1.))))
        reverse_points.append(rp); reverse_normals.append(rn); reverse_distance.extend(distances)
    points, tri = np.concatenate(mesh_points), np.concatenate(mesh_triangles)
    uv, owner = np.concatenate(mesh_uv), np.concatenate(mesh_owner)
    proxy = Locator(points, tri)
    query, _, _, source_ids, _ = sample(vertices, triangles, forward_samples, seed+1)
    _, proxy_dist, ids, bary = proxy.closest(query)
    seed_uv = np.einsum('ij,ijk->ik', bary, uv[tri[ids]])
    fitted = np.empty_like(query); candidate = owner[ids]
    for pi in np.unique(candidate):
        selected = np.flatnonzero(candidate == pi)
        fitted[selected], _ = project(records[int(pi)], query[selected], seed_uv[selected])
    distance = np.linalg.norm(fitted-query, axis=1)
    result = dict(forward_samples=forward_samples, reverse_samples=len(reverse_distance),
                  reverse_per_patch=reverse_per_patch, seed=seed, training_samples_reused=False,
                  sampled_source_to_surface_upper_bound_mm=stats(distance),
                  source_to_tessellation_proxy_mm=stats(proxy_dist),
                  sampled_surface_to_source_mesh_mm=stats(reverse_distance),
                  area_weighted_surface_to_source_mesh_mm=area_stats(reverse_distance, area_weights),
                  surface_to_flat_source_normal_degrees=stats(normal_errors),
                  area_weighted_surface_to_flat_source_normal_degrees=area_stats(normal_errors, area_weights),
                  sampled_abs_mean_curvature_per_mm=stats(curvature),
                  reverse_estimated_area_mm2=float(np.sum(area_weights)),
                  reverse_samples_opposed_to_nearest_triangle=int(np.sum(np.asarray(normal_errors) > 90.)),
                  singular_reverse_samples=singular, tessellation_triangles=len(tri),
                  sampling_note=__doc__, seconds=time.perf_counter()-started)
    dump(folder/'quality.json', result)
    np.savez_compressed(folder/'quality_samples.npz', source=query, fitted=fitted, distance=distance,
                        source_triangle_ids=source_ids, reverse_points=np.concatenate(reverse_points),
                        reverse_distance=np.array(reverse_distance), reverse_normals=np.concatenate(reverse_normals))
    np.savez_compressed(folder/'display_mesh.npz', points=points, triangles=tri,
                        normals=np.concatenate(mesh_normals), patch=owner)
    print(json.dumps(result), flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True)
    parser.add_argument('--forward-samples', type=int, default=30000)
    parser.add_argument('--reverse-per-patch', type=int, default=64)
    args = parser.parse_args(); metrics(args.run, args.forward_samples, args.reverse_per_patch)
