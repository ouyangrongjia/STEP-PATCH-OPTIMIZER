"""Input generation only. CAD is permitted here, never in reconstruct.py."""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
from .mesh import (audit, corner_chains, dump, mean_value, chain_parameters,
                   side_uv, write_stl, read_stl, distortion)


def emit(out, vertices, triangles, labels, corners, source):
    out.mkdir(parents=True, exist_ok=False)
    vertices = np.asarray(vertices); triangles = np.asarray(triangles); labels = np.asarray(labels)
    quantized, remap = np.unique(vertices.astype(np.float32), axis=0, return_inverse=True)
    tri = remap[triangles]
    area2 = np.linalg.norm(np.cross(quantized[tri[:, 1]]-quantized[tri[:, 0]],
                                    quantized[tri[:, 2]]-quantized[tri[:, 0]]), axis=1)
    keep = area2 > 1e-14
    tri, labels = tri[keep], labels[keep]
    corners = [remap[c].tolist() for c in corners]
    # Normalize indexing exactly as the STL importer will do (unused vertices removed).
    used = np.unique(tri); compact = np.full(len(quantized), -1, dtype=int); compact[used] = np.arange(len(used))
    quantized = quantized[used]; tri = compact[tri]; corners = [compact[c].tolist() for c in corners]
    write_stl(out/'input.stl', quantized, tri)
    reread, rt = read_stl(out/'input.stl')
    if not np.array_equal(rt, tri) or not np.array_equal(reread, quantized):
        raise ValueError('STL ordering changed unexpectedly')
    patches = []
    for i, c in enumerate(corners):
        patches.append(dict(corners=c, chains=corner_chains(tri[labels == i], c)))
    metadata = dict(schema=1, units='mm', triangle_labels=labels.tolist(), patches=patches,
                    smooth_seams=True, input_kind='given-partition mesh',
                    tolerance_mm=.03, seam_tolerance_mm=.001, construction_tolerance_mm=1e-7)
    dump(out/'layout.json', metadata)
    report = dict(source=source, input_files=['input.stl', 'layout.json'], topology=audit(reread, rt),
                  quantization_max_mm=float(np.linalg.norm(vertices-vertices.astype(np.float32), axis=1).max()),
                  collapsed_triangles_removed=int((~keep).sum()),
                  indexing='Exact binary STL float32 equality; no proximity weld',
                  prohibited_fields='No UV, exact normals, CAD points beyond mesh vertices, curves, control points or archived map')
    dump(out/'generation.json', report)
    return report


def clip_triangle(uv, xyz, polygon):
    poly = [(x.copy(), p.copy()) for x, p in zip(uv, xyz)]
    for a, b in zip(polygon, np.roll(polygon, -1, axis=0)):
        edge = b-a
        def signed(q):
            return edge[0]*(q[1]-a[1])-edge[1]*(q[0]-a[0])
        new = []
        for (q, p), (r, s) in zip(poly, poly[1:]+poly[:1]):
            dq, dr = signed(q), signed(r)
            if dq >= -1e-13:
                new.append((q, p))
            if (dq < -1e-13 and dr > 1e-13) or (dr < -1e-13 and dq > 1e-13):
                t = dq/(dq-dr); new.append((q+t*(r-q), p+t*(s-p)))
        poly = new
        if len(poly) < 3:
            return []
    clean = []
    for q, p in poly:
        if not clean or np.linalg.norm(q-clean[-1][0]) > 1e-11:
            clean.append((q, p))
    if len(clean) > 1 and np.linalg.norm(clean[0][0]-clean[-1][0]) < 1e-11:
        clean.pop()
    return clean


def partition(vertices, triangles, uv, polygons):
    newp, newuv, newtri, labels, lookup = [], [], [], [], {}
    for tri in triangles:
        q, p = uv[tri], vertices[tri]
        for label, poly in enumerate(polygons):
            if np.any(q.max(axis=0) < poly.min(axis=0)) or np.any(q.min(axis=0) > poly.max(axis=0)):
                continue
            clipped = clip_triangle(q, p, poly)
            indices = []
            for qi, pi in clipped:
                key = tuple(np.round(qi, 11))
                if key not in lookup:
                    lookup[key] = len(newp); newp.append(pi); newuv.append(qi)
                indices.append(lookup[key])
            for i in range(1, len(indices)-1):
                itri = [indices[0], indices[i], indices[i+1]]
                coords = np.asarray([newuv[k] for k in itri]); a, b = coords[1:]-coords[0]
                if abs(a[0]*b[1]-a[1]*b[0]) > 1e-15:
                    newtri.append(itri); labels.append(label)
    newuv = np.asarray(newuv); newtri = np.asarray(newtri); labels = np.asarray(labels)
    corners = []
    for label, poly in enumerate(polygons):
        ids = np.unique(newtri[labels == label])
        corners.append([int(ids[np.argmin(np.linalg.norm(newuv[ids]-x, axis=1))]) for x in poly])
    return np.asarray(newp), newtri, labels, corners


def cad_region(source, out, deflection):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
    from geometry import read_step, tessellate, xyz
    from designer_layout import original_loop
    from OCP.BRepTools import BRepTools
    start = time.perf_counter()
    region = read_step(source); read_seconds = time.perf_counter()-start
    BRepTools.Clean_s(region)
    start = time.perf_counter(); p, tri, _, _ = tessellate(region, deflection)
    # Generator-side rounding bridges duplicated CAD-face triangulation nodes.
    # It is far below reconstruction and sewing tolerance and is audited below.
    _, first, remap = np.unique(np.round(p, 8), axis=0, return_index=True, return_inverse=True)
    vertices, triangles = p[first], remap[tri]
    topology = audit(vertices, triangles)
    if any(topology[k] for k in ['nonmanifold_edges', 'inconsistent_edges', 'degenerate_triangles']) or topology['euler'] != 1:
        raise ValueError(f'CAD mesh adjacency failed before layout: {topology}')
    _, curves, _ = original_loop(region)
    corner_points = np.asarray([xyz(curves[i].StartPoint()) for i in [0, 6, 10, 18]])
    corners = [int(np.argmin(np.linalg.norm(vertices-q, axis=1))) for q in corner_points]
    chains = corner_chains(triangles, corners)
    ids = np.concatenate([np.array(c[:-1]) for c in chains])
    boundary = np.concatenate([side_uv(k, chain_parameters(vertices, c)[:-1]) for k, c in enumerate(chains)])
    uv, residual = mean_value(vertices, triangles, ids, boundary)
    quality = distortion(vertices, triangles, uv)
    if quality['flipped'] or quality['zero']:
        raise ValueError(f'Global layout chart fails: {quality}')
    o = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=float)
    inner = .5+(o-.5)*.45
    polygons = [np.array([o[i], o[(i+1) % 4], inner[(i+1) % 4], inner[i]]) for i in range(4)]+[inner]
    newp, newtri, labels, corner_ids = partition(vertices, triangles, uv, polygons)
    report = emit(out, newp, newtri, labels, corner_ids,
                  dict(kind='original CAD region triangulation', region=str(Path(source).resolve()),
                       deflection_mm=deflection, angular_deflection_rad=.1,
                       layout='Global mesh MVC rectangle with supplied original boundary corner indices; five fixed connected patches; no old candidate geometry',
                       boundary_corner_edge_indices=[0, 6, 10, 18], inner_scale=.45))
    report.update(step_read_seconds=read_seconds, mesh_and_layout_seconds=time.perf_counter()-start,
                  original_mesh_topology=topology, generator_weld_max_mm=float(np.linalg.norm(p-vertices[remap], axis=1).max()),
                  generator_uv=quality, generator_uv_residual=residual)
    dump(out/'generation.json', report)
    return report


def fixture(out, intervals, kind):
    start = time.perf_counter(); n = intervals+1
    q = np.linspace(-1, 1, n); u, v = np.meshgrid(q, q, indexing='ij')
    points, triangles, labels, corners = [], [], [], []
    grids = []
    if kind == 'closed':
        # Six oriented cube charts on one smooth non-polynomial radial surface.
        for axis in range(3):
            for sign in [-1, 1]:
                p = np.zeros((n, n, 3)); p[:, :, axis] = sign
                p[:, :, (axis+1) % 3] = u; p[:, :, (axis+2) % 3] = v*sign
                p /= np.linalg.norm(p, axis=2)[:, :, None]
                radius = 1+.07*np.sin(3*p[:, :, 0])*np.cos(2*p[:, :, 1])+.04*p[:, :, 2]**2
                grids.append(p*radius[:, :, None]*np.array([2., 1.5, 1.7]))
    else:
        angle = u*(2*np.pi/3); z = v*1.5
        radius = 2+.12*np.sin(2*angle)*np.cos(np.pi*z/3)
        grids.append(np.stack([radius*np.cos(angle), radius*np.sin(angle), z], axis=-1))
    for label, p in enumerate(grids):
        base = len(points); points.extend(p.reshape(-1, 3))
        idx = np.arange(n*n).reshape(n, n)+base
        for i in range(intervals):
            for j in range(intervals):
                a, b, c, d = idx[i, j], idx[i+1, j], idx[i+1, j+1], idx[i, j+1]
                triangles.extend([[a, b, c], [a, c, d]]); labels.extend([label, label])
        corners.append([int(idx[0, 0]), int(idx[-1, 0]), int(idx[-1, -1]), int(idx[0, -1])])
    result = emit(out, points, triangles, labels, corners,
                  dict(kind=kind, intervals=intervals, provenance='Independent mesh fixture; formulas used only by generation; no CAD caps'))
    result['generation_seconds'] = time.perf_counter()-start; dump(out/'generation.json', result)
    return result


def main():
    p = argparse.ArgumentParser(); p.add_argument('--kind', choices=['cad', 'closed', 'wrapped'], required=True)
    p.add_argument('--source'); p.add_argument('--output', required=True); p.add_argument('--deflection', type=float, default=.003)
    p.add_argument('--intervals', type=int, default=32); args = p.parse_args()
    out = Path(args.output)
    result = cad_region(args.source, out, args.deflection) if args.kind == 'cad' else fixture(out, args.intervals, args.kind)
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
