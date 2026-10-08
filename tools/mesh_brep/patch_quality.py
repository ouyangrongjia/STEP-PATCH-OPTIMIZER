"""Bounded-cost diagnostics on saved native spline supports, before STEP audit.

Uniform parameter sampling can miss narrow knot spans. These results are useful
for rejecting bad candidates, not for certifying geometry or CAD import.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from .mesh import Locator, dump, vertex_normals
from .splines import tensor
from .regional_quality import weighted_stats


def audit(run, output, resolution=25, samples=256, render=False):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); metadata = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(metadata['partition'])
    v, f = source['mesh_vertex_original'], source['mesh_triangle']
    locator = Locator(v, f); reference_normals = vertex_normals(v, f)
    grid = np.linspace(0, 1, resolution)
    display_uv = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    cells = []
    for i in range(resolution-1):
        for j in range(resolution-1):
            a = i*resolution+j; b = a+resolution
            cells.extend([[a, b, b+1], [a, b+1, a+1]])
    cells = np.asarray(cells); rng = np.random.default_rng(930711)
    points, normals, triangles, records = [], [], [], []
    distances, angles, weights = [], [], []
    for index, path in enumerate(sorted((root/'patches').glob('*.npz'))):
        z = np.load(path); cp = z['poles'].reshape(-1, 3); tu, tv = z['tu'], z['tv']
        sign = int(z['orientation']); q = rng.uniform(1e-8, 1-1e-8, (samples, 2))
        def evaluate(uv):
            xyz = tensor(uv, tu, tv)@cp
            cross = np.cross(tensor(uv, tu, tv, 1, 0)@cp, tensor(uv, tu, tv, 0, 1)@cp)
            jac = np.linalg.norm(cross, axis=1)
            return xyz, sign*cross/np.maximum(jac[:, None], 1e-30), jac
        p, n, jac = evaluate(q); _, distance, ids, bary = locator.closest(p)
        target = np.einsum('ij,ijk->ik', bary, reference_normals[f[ids]])
        target /= np.maximum(np.linalg.norm(target, axis=1)[:, None], 1e-30)
        angle = np.degrees(np.arccos(np.clip(np.sum(n*target, axis=1), -1, 1)))
        record = dict(patch=index, domain=int(z['domain']), region=int(z['region']),
            distance_mm=weighted_stats(distance, jac), normal_degrees=weighted_stats(angle, jac),
            opposed_area_fraction=float(jac[angle > 90].sum()/max(jac.sum(), 1e-30)),
            estimated_area_mm2=float(jac.mean()))
        records.append(record); distances.extend(distance); angles.extend(angle); weights.extend(jac)
        xyz, nn, _ = evaluate(display_uv)
        _, nn, _ = evaluate(np.clip(display_uv, 1e-7, 1-1e-7))
        points.append(xyz); normals.append(nn)
        triangles.append((cells if sign > 0 else cells[:, [0, 2, 1]])+index*len(display_uv))
    angle = np.array(angles); weight = np.array(weights)
    report = dict(status='DiagnosticOnly', can_apply=False, partition=metadata['partition'],
        fitted_run=str(root.resolve()), reverse_distance_mm=weighted_stats(distances, weights),
        source_normal_difference_degrees=weighted_stats(angles, weights),
        opposed_area_fraction=float(weight[angle > 90].sum()/weight.sum()),
        estimated_area_mm2=float(weight.sum()/samples), patches=len(records),
        resolution=resolution, samples_per_patch=samples, seconds=time.perf_counter()-started,
        scope='Independent finite native-support samples; display is a uniform parameter grid, not STEP tessellation. Not a Hausdorff, intersection or CAD certification.')
    dump(out/'run.json', report); dump(out/'patches.json', records)
    np.savez_compressed(out/'display_mesh.npz', points=np.concatenate(points),
                        normals=np.concatenate(normals), triangles=np.concatenate(triangles))
    if render:
        from .regional_quality import render as draw
        draw(out, title='原生曲面有限网格（诊断）')
    print(json.dumps(report), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True)
    parser.add_argument('--output', required=True); parser.add_argument('--render', action='store_true')
    args = parser.parse_args(); audit(args.run, args.output, render=args.render)
