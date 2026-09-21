"""Extract original zero-based FaceIds and independent exact-surface samples."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from geometry import *
from OCP.TopoDS import TopoDS_Shell


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--train-count', type=int, default=12000)
    parser.add_argument('--test-count', type=int, default=18000)
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    baseline_dir = Path(args.baseline)
    historical = json.loads((baseline_dir/'validation_summary.json').read_text(encoding='utf-8-sig'))
    face_ids = [int(i) for i in historical['performance']['selected_faces']]
    shape = read_step(args.source)
    all_faces = shapes(shape, TopAbs_FACE)
    faces = [all_faces[i] for i in face_ids]
    region=TopoDS_Shell();builder=BRep_Builder();builder.MakeShell(region)
    for face in faces:builder.Add(region,face)
    write_step(out/'original_region.step', region)
    points, triangles, mesh_uv, tri_face = tessellate(region)
    area = np.linalg.norm(np.cross(points[triangles[:, 1]]-points[triangles[:, 0]],
                                   points[triangles[:, 2]]-points[triangles[:, 0]]), axis=1)/2
    data = {}
    for label, count, seed in [('train', args.train_count, 179), ('test', args.test_count, 180)]:
        rng = np.random.default_rng(seed)
        choices = rng.choice(len(triangles), count, p=area/area.sum())
        root = np.sqrt(rng.random(count))
        fraction = rng.random(count)
        weights = np.c_[1-root, root*(1-fraction), root*fraction]
        uv = np.einsum('ij,ijk->ik', weights, mesh_uv[triangles[choices]])
        sample_faces = tri_face[choices]
        p, n = np.empty((count, 3)), np.empty((count, 3))
        for fi, face in enumerate(faces):
            mask = sample_faces == fi
            p[mask], n[mask] = evaluate(BRep_Tool.Surface_s(face), uv[mask])
            if face.Orientation() == TopAbs_REVERSED:
                n[mask] *= -1
        data.update({f'{label}_points': p, f'{label}_normals': n,
                     f'{label}_uv': uv, f'{label}_faces': np.array(face_ids)[sample_faces]})
    boundary, boundary_ids, boundary_t = [], [], []
    boundary_edges = [(e, use) for e, use in edge_uses(faces) if len(use) == 1]
    for i, (edge, use) in enumerate(boundary_edges):
        ts, ps = edge_samples(edge, 193)
        boundary.extend(ps)
        boundary_ids.extend([i]*len(ps))
        boundary_t.extend(ts)
    data.update(boundary_points=np.asarray(boundary), boundary_ids=np.asarray(boundary_ids),
                boundary_t=np.asarray(boundary_t), mesh_points=points, mesh_triangles=triangles,
                mesh_uv=mesh_uv, mesh_triangle_face=tri_face)
    center = data['train_points'].mean(axis=0)
    _, singular, axes = np.linalg.svd(data['train_points']-center, full_matrices=False)
    if np.linalg.det(axes) < 0:
        axes[2] *= -1
    normal_dot = data['test_normals']@axes[2]
    if np.median(normal_dot) < 0:
        axes[1:] *= -1
        normal_dot *= -1
    data.update(center=center, axes=axes)
    np.savez_compressed(out/'reference.npz', **data)
    baseline = read_step(baseline_dir/'trimmed_patch_faces.step')
    bp, bt, buv, bf = tessellate(baseline)
    np.savez_compressed(out/'baseline_mesh.npz', points=bp, triangles=bt, uv=buv, face=bf)
    result = dict(source=str(Path(args.source).resolve()), source_bytes=Path(args.source).stat().st_size,
                  selected_face_ids=face_ids, face_id_convention='TopExp.MapShapes zero-based, same as TopologyGraph.cpp',
                  input_faces=len(all_faces), original_region=topology(region),original_region_step_readback=topology(read_step(out/'original_region.step')),
                  boundary_edges=len(boundary_edges), bounding_box=[points.min(axis=0).tolist(),points.max(axis=0).tolist()],
                  tessellation=dict(deflection_mm=0.003, angular_deflection_rad=0.1, triangles=len(triangles), area_mm2=float(area.sum())),
                  sampling=dict(train_count=args.train_count, train_seed=179, test_count=args.test_count, test_seed=180,
                                method='area-weighted random triangle choice; UV barycentric then exact original surface evaluation',
                                boundary_samples_per_edge=193),
                  pca_singular_values=singular.tolist(), projection_normal_dot_quantiles=np.quantile(normal_dot,[0,.01,.5,1]).tolist(),
                  baseline=topology(baseline), baseline_surfaces=[surface_info(BRep_Tool.Surface_s(f)) for f in shapes(baseline,TopAbs_FACE)],
                  elapsed_seconds=time.perf_counter()-start)
    dump(out/'input_audit.json', result)
    print(json.dumps(result,indent=2),flush=True)


if __name__ == '__main__':
    main()
