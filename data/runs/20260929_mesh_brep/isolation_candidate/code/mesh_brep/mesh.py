"""Mesh-only I/O, topology, mean-value charts and piecewise-linear geometry.

No CAD imports. STL indexing merges bit-identical float32 coordinates only;
proximity welding is deliberately not used (thin walls are not weld targets).
"""
import json
import struct
from pathlib import Path

import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve


def dump(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')


def stats(x):
    x = np.asarray(x, dtype=float)
    if not len(x):
        return None
    return dict(rms=float(np.sqrt(np.mean(x*x))), p50=float(np.median(x)),
                p95=float(np.quantile(x, .95)), max=float(np.max(x)), min=float(np.min(x)))


def read_stl(path):
    data = Path(path).read_bytes()
    if len(data) < 84:
        raise ValueError('Truncated binary STL')
    n = struct.unpack_from('<I', data, 80)[0]
    if len(data) != 84+50*n:
        raise ValueError('This entry accepts binary STL; convert ASCII explicitly')
    dtype = np.dtype([('normal', '<f4', 3), ('vertices', '<f4', (3, 3)), ('attribute', '<u2')])
    raw = np.frombuffer(data, dtype, n, 84)['vertices'].copy()
    if not np.isfinite(raw).all():
        raise ValueError('Nonfinite STL coordinates')
    vertices, inverse = np.unique(raw.reshape(-1, 3), axis=0, return_inverse=True)
    return vertices.astype(float), inverse.reshape(-1, 3)


def write_stl(path, vertices, triangles):
    p = np.asarray(vertices, dtype='<f4')[triangles]
    cross = np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0])
    norm = np.linalg.norm(cross, axis=1)
    if np.any(norm == 0):
        raise ValueError('Triangle collapsed at STL float32 precision')
    dtype = np.dtype([('normal', '<f4', 3), ('vertices', '<f4', (3, 3)), ('attribute', '<u2')])
    rows = np.zeros(len(p), dtype=dtype)
    rows['vertices'] = p
    rows['normal'] = cross/norm[:, None]
    Path(path).write_bytes(b'Mesh BRep experiment; units in sidecar'.ljust(80, b' ')+
                          struct.pack('<I', len(p))+rows.tobytes())


def edges(triangles):
    oriented = np.concatenate([triangles[:, [0, 1]], triangles[:, [1, 2]], triangles[:, [2, 0]]])
    unique, inverse, counts = np.unique(np.sort(oriented, axis=1), axis=0,
                                        return_inverse=True, return_counts=True)
    signs = np.where(oriented[:, 0] < oriented[:, 1], 1, -1)
    direction_sum = np.bincount(inverse, weights=signs, minlength=len(unique))
    return unique, counts, direction_sum


def audit(vertices, triangles):
    e, counts, direction = edges(triangles)
    cross = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                     vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
    areas = np.linalg.norm(cross, axis=1)/2
    used = np.unique(triangles)
    graph = sparse.coo_matrix((np.ones(len(e)*2), (e.ravel(), e[:, ::-1].ravel())),
                              shape=(len(vertices), len(vertices))).tocsr()
    components = sparse.csgraph.connected_components(graph[used][:, used], directed=False)[0]
    return dict(vertices=len(used), triangles=len(triangles), edges=len(e),
                euler=int(len(used)-len(e)+len(triangles)), components=int(components),
                boundary_edges=int(np.sum(counts == 1)), nonmanifold_edges=int(np.sum(counts > 2)),
                inconsistent_edges=int(np.sum((counts == 2) & (direction != 0))),
                degenerate_triangles=int(np.sum(areas < 1e-15)), area_mm2=float(areas.sum()))


def boundary_loop(triangles):
    e, counts, _ = edges(triangles)
    e = e[counts == 1]
    neighbors = {}
    for a, b in e:
        neighbors.setdefault(int(a), []).append(int(b))
        neighbors.setdefault(int(b), []).append(int(a))
    if not neighbors or any(len(v) != 2 for v in neighbors.values()):
        raise ValueError('Boundary is missing or branched')
    path = [min(neighbors)]
    previous = -1
    while True:
        nxt = next(x for x in neighbors[path[-1]] if x != previous)
        if nxt == path[0]:
            break
        previous = path[-1]
        path.append(nxt)
        if len(path) > len(neighbors):
            raise ValueError('Boundary traversal failed')
    if len(path) != len(neighbors):
        raise ValueError('Multiple boundary components; supply a cut')
    return np.asarray(path)


def corner_chains(triangles, corners):
    loop = boundary_loop(triangles)
    positions = [int(np.flatnonzero(loop == x)[0]) for x in corners]
    shifted = (np.asarray(positions)-positions[0]) % len(loop)
    if not np.all(np.diff(shifted) > 0):
        loop = loop[::-1]
        positions = [int(np.flatnonzero(loop == x)[0]) for x in corners]
        shifted = (np.asarray(positions)-positions[0]) % len(loop)
    if not np.all(np.diff(shifted) > 0):
        raise ValueError('Corners not in cyclic boundary order')
    return [loop[(np.arange((positions[(i+1) % 4]-positions[i]) % len(loop)+1)+positions[i]) % len(loop)].tolist()
            for i in range(4)]


def chain_parameters(vertices, chain):
    distance = np.r_[0., np.cumsum(np.linalg.norm(np.diff(vertices[chain], axis=0), axis=1))]
    if distance[-1] <= 0 or np.any(np.diff(distance) <= 0):
        raise ValueError('Zero length or duplicate chain vertex')
    return distance/distance[-1]


def side_uv(side, t):
    t = np.asarray(t)
    return [lambda: np.c_[t, t*0], lambda: np.c_[t*0+1, t],
            lambda: np.c_[1-t, t*0+1], lambda: np.c_[t*0, 1-t]][side]()


def mean_value(vertices, triangles, fixed_ids, fixed_uv):
    """Positive Floater weights, arbitrary prescribed convex boundary.

    Independently implemented from the formula, not copied CGAL source.
    A square has collinear boundary vertices: actual flips/zeros are checked.
    """
    rows, cols, values = [], [], []
    for k in range(3):
        a, b, c = triangles[:, k], triangles[:, (k+1) % 3], triangles[:, (k+2) % 3]
        x, y = vertices[b]-vertices[a], vertices[c]-vertices[a]
        lx, ly = np.linalg.norm(x, axis=1), np.linalg.norm(y, axis=1)
        denom = lx*ly+np.einsum('ij,ij->i', x, y)
        half = np.linalg.norm(np.cross(x, y), axis=1)/np.maximum(denom, 1e-30)
        rows.extend([a, a]); cols.extend([b, c]); values.extend([half/lx, half/ly])
    w = sparse.coo_matrix((np.concatenate(values), (np.concatenate(rows), np.concatenate(cols))),
                           shape=(len(vertices), len(vertices))).tocsr()
    lap = sparse.diags(np.asarray(w.sum(axis=1)).ravel())-w
    used = np.unique(triangles)
    free = np.setdiff1d(used, fixed_ids)
    uv = np.zeros((len(vertices), 2)); uv[fixed_ids] = fixed_uv
    a = lap[free][:, free].tocsc()
    rhs = -lap[free][:, fixed_ids]@fixed_uv
    uv[free] = spsolve(a, rhs)
    residual = np.max(np.abs(a@uv[free]-rhs)) if len(free) else 0.
    return uv, float(residual)


def distortion(vertices, triangles, uv):
    p, q = vertices[triangles], uv[triangles]
    a, b = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]
    length = np.linalg.norm(a, axis=1)
    bx = np.einsum('ij,ij->i', a, b)/length
    by = np.linalg.norm(np.cross(a, b), axis=1)/length
    physical = np.zeros((len(p), 2, 2))
    physical[:, 0, 0] = length; physical[:, 0, 1] = bx; physical[:, 1, 1] = by
    planar = np.stack([q[:, 1]-q[:, 0], q[:, 2]-q[:, 0]], axis=2)
    jac = planar@np.linalg.inv(physical)
    det = np.linalg.det(jac)
    orientation = 1 if np.median(det) > 0 else -1
    singular = np.linalg.svd(jac, compute_uv=False)
    return dict(flipped=int(np.sum(det*orientation < -1e-14)), zero=int(np.sum(abs(det) <= 1e-14)),
                orientation=orientation, stretch_ratio=stats(singular[:, 0]/np.maximum(singular[:, 1], 1e-30)),
                area_scale=stats(abs(det)))


def charts(vertices, triangles, metadata):
    labels = np.asarray(metadata['triangle_labels'])
    if len(labels) != len(triangles):
        raise ValueError('Sidecar triangle count differs from STL')
    result = []
    # Declared smooth chart cuts are not geometric features. Accumulate their
    # normals from the real global one-ring, including all incident charts at
    # junctions. A per-chart one-ring introduces a systematic boundary bias.
    global_normals = vertex_normals(vertices, triangles) if metadata.get('smooth_seams') else None
    for i, patch in enumerate(metadata['patches']):
        tri_ids = np.flatnonzero(labels == i); tri = triangles[tri_ids]
        report = audit(vertices, tri)
        if any(report[k] for k in ['nonmanifold_edges', 'inconsistent_edges', 'degenerate_triangles']) or report['euler'] != 1 or report['components'] != 1:
            raise ValueError(f'Patch {i} is not an oriented disk: {report}')
        loop = boundary_loop(tri)
        chains = patch['chains']
        if len(chains) != 4 or set(loop) != set(sum(chains, [])):
            raise ValueError('Four chains must cover the actual boundary exactly')
        if any(chains[k][-1] != chains[(k+1) % 4][0] for k in range(4)):
            raise ValueError('Disconnected chart sides')
        ids = np.concatenate([np.asarray(c[:-1]) for c in chains])
        ts = [chain_parameters(vertices, c) for c in chains]
        fixed = np.concatenate([side_uv(k, t[:-1]) for k, t in enumerate(ts)])
        uv, residual = mean_value(vertices, tri, ids, fixed)
        distort = distortion(vertices, tri, uv)
        if distort['flipped'] or distort['zero']:
            raise ValueError(f'MVC chart {i} flipped or singular: {distort}')
        normals = global_normals if global_normals is not None else vertex_normals(vertices, tri)
        result.append(dict(triangles=tri, triangle_ids=tri_ids, uv=uv, chains=chains,
                           chain_t=ts, normals=normals, audit=report, distortion=distort, residual=residual))
    return result


def vertex_normals(vertices, triangles):
    normal = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                      vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
    out = np.zeros_like(vertices)
    for k in range(3):
        np.add.at(out, triangles[:, k], normal)
    out /= np.maximum(np.linalg.norm(out, axis=1)[:, None], 1e-30)
    return out


def sample(vertices, triangles, count, seed, uv=None, normals=None):
    rng = np.random.default_rng(seed)
    p = vertices[triangles]
    crosses = np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0])
    area2 = np.linalg.norm(crosses, axis=1)
    selected = rng.choice(len(triangles), count, p=area2/area2.sum())
    root, split = np.sqrt(rng.random(count)), rng.random(count)
    bary = np.c_[1-root, root*(1-split), root*split]
    chosen = triangles[selected]
    points = np.einsum('ij,ijk->ik', bary, vertices[chosen])
    n = crosses[selected]/area2[selected, None] if normals is None else np.einsum('ij,ijk->ik', bary, normals[chosen])
    n /= np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-30)
    params = None if uv is None else np.einsum('ij,ijk->ik', bary, uv[chosen])
    return points, n, params, selected, bary


class Locator:
    """Exact closest point on the discrete triangles, using VTK's cell locator."""
    def __init__(self, vertices, triangles):
        import vtk
        from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray
        self.vtk = vtk; self.vertices = vertices; self.triangles = triangles
        points = vtk.vtkPoints(); points.SetData(numpy_to_vtk(vertices, deep=True))
        cells = vtk.vtkCellArray()
        cells.SetCells(len(triangles), numpy_to_vtkIdTypeArray(np.c_[np.full(len(triangles), 3), triangles].astype(np.int64).ravel(), deep=True))
        self.poly = vtk.vtkPolyData(); self.poly.SetPoints(points); self.poly.SetPolys(cells)
        self.locator = vtk.vtkStaticCellLocator(); self.locator.SetDataSet(self.poly); self.locator.BuildLocator()

    def closest(self, points):
        v = self.vtk
        point = [0., 0., 0.]; cell, sub, dist = v.mutable(0), v.mutable(0), v.mutable(0.)
        xyz = np.empty_like(points); ids = np.empty(len(points), dtype=int); d = np.empty(len(points))
        for i, p in enumerate(points):
            self.locator.FindClosestPoint(p, point, cell, sub, dist)
            xyz[i] = point; ids[i] = int(cell); d[i] = np.sqrt(float(dist))
        p = self.vertices[self.triangles[ids]]
        a, b, c = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0], xyz-p[:, 0]
        aa = np.einsum('ij,ij->i', a, a); ab = np.einsum('ij,ij->i', a, b); bb = np.einsum('ij,ij->i', b, b)
        ac = np.einsum('ij,ij->i', a, c); bc = np.einsum('ij,ij->i', b, c)
        den = aa*bb-ab*ab
        x, y = (bb*ac-ab*bc)/den, (aa*bc-ab*ac)/den
        bary = np.c_[1-x-y, x, y]
        return xyz, d, ids, bary
