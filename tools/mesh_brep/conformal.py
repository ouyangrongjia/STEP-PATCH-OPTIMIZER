"""Given disk region -> free-boundary mesh LSCM -> one trimmed XYZ spline.

This is a deterministic merge of the supplied smooth region, not partition search.
The chart is intrinsic to mesh triangles and does not assume a 3D height field.
"""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from .mesh import read_stl, audit, distortion, sample, corner_chains, chain_parameters, dump, stats, vertex_normals
from .splines import knots, tensor, joint_fit
from .trimmed import quadratic_normals, pcurve, project
from . import cad


def lscm(vertices, triangles, anchors):
    p = vertices[triangles]; a, b = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]
    length = np.linalg.norm(a, axis=1); x = np.sum(a*b, axis=1)/length
    y = np.linalg.norm(np.cross(a, b), axis=1)/length
    gx = np.c_[-1/length, 1/length, length*0]
    gy = np.c_[(x-length)/(length*y), -x/(length*y), 1/y]
    weight = np.sqrt(length*y/2)
    rows = np.repeat(np.arange(len(triangles)), 3); cols = triangles.ravel()
    shape = (len(triangles), len(vertices))
    dx = sparse.coo_matrix(((gx*weight[:, None]).ravel(), (rows, cols)), shape=shape).tocsr()
    dy = sparse.coo_matrix(((gy*weight[:, None]).ravel(), (rows, cols)), shape=shape).tocsr()
    matrix = sparse.bmat([[dx, -dy], [dy, dx]], format='csr')
    fixed = np.r_[anchors, np.asarray(anchors)+len(vertices)]
    values = np.array([0., 1., 0., 0.])
    free = np.setdiff1d(np.arange(2*len(vertices)), fixed)
    lhs = matrix[:, free]; rhs = -matrix[:, fixed]@values
    solution = np.zeros(2*len(vertices)); solution[fixed] = values
    solution[free] = spsolve((lhs.T@lhs).tocsc(), lhs.T@rhs)
    uv = solution.reshape(2, -1).T
    # Rotate only the planar chart, then reserve an extrapolation strip.
    center = uv.mean(axis=0); _, axes = np.linalg.eigh((uv-center).T@(uv-center))
    if np.linalg.det(axes) < 0:
        axes[:, 0] *= -1
    uv = (uv-center)@axes
    lo, hi = uv.min(axis=0), uv.max(axis=0)
    uv = .04+.92*(uv-lo)/(hi-lo)
    quality = distortion(vertices, triangles, uv)
    if quality['flipped'] or quality['zero']:
        raise ValueError(f'Free-boundary chart is not usable: {quality}')
    return uv, dict(distortion=quality, energy=float(np.sum((matrix@solution)**2)),
                    linear_residual=float(np.linalg.norm(lhs.T@(lhs@solution[free]-rhs))), anchors=list(anchors))


def export(poles, tu, tv, uv, chains, parameters, output):
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeWire
    from OCP.BRepLib import BRepLib
    from OCP.TopoDS import TopoDS
    surf = cad.surface(poles, tu, tv)
    vertices = {ch[0]: BRepBuilderAPI_MakeVertex(surf.Value(*uv[ch[0]])).Vertex() for ch in chains}
    wire = BRepBuilderAPI_MakeWire()
    for chain, t in zip(chains, parameters):
        curve = pcurve(uv[chain], t)
        edge = BRepBuilderAPI_MakeEdge(curve, surf, vertices[chain[0]], vertices[chain[-1]], 0., 1.).Edge()
        if not BRepLib.BuildCurve3d_s(edge, 1e-8):
            raise RuntimeError('New outer curve construction failed')
        wire.Add(edge)
    if not wire.IsDone():
        raise RuntimeError('Outer trim wire is disconnected')
    face = BRepBuilderAPI_MakeFace(surf, wire.Wire(), True).Face()
    # Chain order is given explicitly, independent of surface orientation.
    from .mesh import vertex_normals
    BRepLib.SameParameter_s(face, 1e-7, True)
    cad.write_step(output, face)
    return face


def reconstruct(stl, layout, out, controls=41, samples=36000, seed=2910100):
    start = time.perf_counter(); out = Path(out); out.mkdir(parents=True, exist_ok=False)
    vertices, triangles = read_stl(stl); meta = json.loads(Path(layout).read_text(encoding='utf-8'))
    allowed = {'schema', 'units', 'triangle_labels', 'patches', 'smooth_seams', 'input_kind',
               'tolerance_mm', 'seam_tolerance_mm', 'construction_tolerance_mm'}
    if set(meta)-allowed or meta['units'] != 'mm' or not meta['smooth_seams']:
        raise ValueError('Only declared smooth mesh partitions in mm are accepted')
    for patch in meta['patches']:
        if set(patch) != {'corners', 'chains'}:
            raise ValueError('Only discrete corners and chains may be provided')
    topo = audit(vertices, triangles)
    if topo['euler'] != 1 or topo['components'] != 1 or any(topo[k] for k in ['degenerate_triangles', 'nonmanifold_edges', 'inconsistent_edges']):
        raise ValueError('The supplied region must be an oriented disk')
    if len(meta['patches']) != 5:
        raise ValueError('This experiment merges the supplied five-chart region')
    corners = [patch['corners'][0] for patch in meta['patches'][:4]]
    chains = corner_chains(triangles, corners)
    read_seconds = time.perf_counter()-start
    tick = time.perf_counter(); uv, parameterization = lscm(vertices, triangles, [corners[0], corners[2]])
    uv_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); normals = vertex_normals(vertices, triangles)
    normal_seconds = time.perf_counter()-tick
    tick = time.perf_counter()
    points, nn, q, _, _ = sample(vertices, triangles, samples, seed, uv, normals)
    boundary = np.concatenate([ch[:-1] for ch in chains]); t = knots(controls)
    area_length = np.sqrt(topo['area_mm2'])
    spec = dict(uv=q, points=points, normals=nn, tu=t, tv=t, lengths=[area_length, area_length],
                boundary_uv=uv[boundary], boundary_points=vertices[boundary], boundary_weight=100.)
    mapping = sparse.eye(controls*controls, format='csr'); offset = np.zeros((controls*controls, 3))
    correspondence_seconds = time.perf_counter()-tick
    poles, solver = joint_fit([spec], [mapping], [offset], [], normal_weight=.003, fairness=1e-6, quadrature_order=4,
                              hard_positions=[(0, uv[corners], vertices[corners])])
    p = poles[0]; boundary_error = np.linalg.norm(tensor(uv[boundary], t, t)@p.reshape(-1, 3)-vertices[boundary], axis=1)
    print(json.dumps(dict(controls=controls, boundary_mm=stats(boundary_error), uv=parameterization)), flush=True)
    tick = time.perf_counter(); export(p, t, t, uv, chains, [chain_parameters(vertices, ch) for ch in chains], out/'network.step')
    write_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); shape = cad.read_generated(out/'network.step')
    topology = cad.topology(shape); checks = cad.edge_checks(shape); expression = cad.expression(shape)
    check_seconds = time.perf_counter()-tick
    np.savez_compressed(out/'patch_0.npz', poles=p, tu=t, tv=t, vertex_uv=uv, triangle_ids=np.arange(len(triangles)))
    report = dict(status='DiagnosticOnly', can_apply=False, mode='given smooth disk region; mesh-only LSCM and trimmed XYZ spline',
                  inputs=[str(Path(stl).resolve()), str(Path(layout).resolve())], supplied_patches=5, reconstructed_patches=1,
                  controls=controls, samples=samples, seed=seed, parameterization=parameterization, solver=solver,
                  normal_source='Area-weighted oriented triangle one-ring',
                  boundary_sample_error_mm=stats(boundary_error), topology=topology, cad_checks=checks, expression=expression,
                  timing_seconds=dict(read_index=read_seconds, uv=uv_seconds, mesh_normals=normal_seconds,
                                      correspondence=correspondence_seconds, matrix_assembly=solver['matrix_assembly_seconds'],
                                      solve=solver['solve_seconds'], brep_write=write_seconds, reread_checks=check_seconds),
                  wall_seconds=time.perf_counter()-start)
    dump(out/'reconstruction.json', report)
    return report


def main():
    parser = argparse.ArgumentParser()
    for name in ['stl', 'layout', 'output']:
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--controls', type=int, default=41)
    a = parser.parse_args(); r = reconstruct(a.stl, a.layout, a.output, a.controls)
    print(json.dumps({k: r[k] for k in ['topology', 'expression', 'wall_seconds']}), flush=True)


if __name__ == '__main__':
    main()
