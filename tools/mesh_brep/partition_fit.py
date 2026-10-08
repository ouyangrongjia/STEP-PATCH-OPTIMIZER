"""Fit fixed, indexed STL partitions. Source CAD is never an input here.

Each region retains its identity. A region may need several graph charts; only
its internal representation is subdivided. Trim curves use the one common mesh
edge network, including holes. Graph coordinates make the support regular by
construction, and reduce the local least-squares solve to one scalar height.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import time

import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve

from .mesh import audit, dump, read_stl, stats, vertex_normals
from .splines import knots, basis, tensor


@dataclass
class Config:
    distance: float = .03
    boundary: float = .001
    controls: tuple = (4, 7, 11, 17, 25)
    normal_weight: float = .0001
    fairness: float = 1e-9
    max_charts: int = 8000
    max_depth: int = 24
    shared_smooth_normals: bool = False
    feature_angle: float = 30.
    boundary_normal_weight: float = 0.
    interior_subdivisions: int = 0


def load_partition(path, stl):
    with np.load(path, allow_pickle=False) as a:
        v = a['mesh_vertex_original'].astype(float)
        f = a['mesh_triangle'].astype(np.int64)
        labels = a['face_instance_id'].copy()
    if (v.ndim != 2 or v.shape[1] != 3 or not np.isfinite(v).all()
            or f.ndim != 2 or f.shape[1] != 3 or np.any(f < 0) or np.any(f >= len(v))
            or labels.shape != (len(f),) or labels.dtype.kind not in 'iu' or np.any(labels < 0)):
        raise ValueError('Invalid indexed partition')
    sv, sf = read_stl(stl)
    if len(sf) != len(f):
        raise ValueError('Partition does not cover the supplied STL')
    for start in range(0, len(f), 50000):
        if not np.array_equal(v[f[start:start+50000]], sv[sf[start:start+50000]]):
            raise ValueError('Partition coordinates/order differ from the supplied STL')
    report = audit(v, f)
    if any(report[k] for k in ('boundary_edges', 'nonmanifold_edges', 'inconsistent_edges', 'degenerate_triangles')):
        raise ValueError(f'Input is not a closed oriented manifold: {report}')
    return v, f, labels.astype(np.int64), report


def incidence(f):
    oriented = f[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2)
    e, inv, count = np.unique(np.sort(oriented, axis=1), axis=0, return_inverse=True, return_counts=True)
    if np.any(count != 2):
        raise ValueError('Every complete-model edge must have two triangle uses')
    order = np.argsort(inv, kind='stable').reshape(-1, 2)
    return e, order//3


def boundary_edges(f):
    oriented = f[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2)
    _, first, count = np.unique(np.sort(oriented, axis=1), axis=0, return_index=True, return_counts=True)
    return oriented[first[count == 1]]


def loops(edges):
    """Oriented outer/hole loops; reject pinched boundaries before CAD work."""
    if not len(edges):
        raise ValueError('A graph chart must have a boundary')
    a, ca = np.unique(edges[:, 0], return_counts=True)
    b, cb = np.unique(edges[:, 1], return_counts=True)
    if not np.array_equal(a, b) or np.any(ca != 1) or np.any(cb != 1):
        raise ValueError('Pinched or branching chart boundary')
    nxt = dict(edges.tolist()); result = []
    while nxt:
        start = min(nxt); here = start; loop = []
        while True:
            loop.append(here); here = nxt.pop(here)
            if here == start:
                break
        result.append(np.array(loop, dtype=np.int64))
    return result


def projected_boundary_valid(uv, boundary):
    """Reject global UV overlaps even when every source triangle faces forward.

    A positive local Jacobian alone does not make a multiply covered graph
    injective. Non-adjacent boundary segments must not cross or touch.
    """
    a, b = uv[boundary[:, 0]], uv[boundary[:, 1]]
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    cross = lambda x, y: x[..., 0]*y[..., 1]-x[..., 1]*y[..., 0]
    eps = np.finfo(float).eps*64
    for i in range(len(boundary)):
        candidates = np.flatnonzero(np.all(lo[i] <= hi[i+1:]+eps, axis=1)
                                    & np.all(hi[i]+eps >= lo[i+1:], axis=1))+i+1
        candidates = candidates[~np.any(boundary[candidates, :, None] == boundary[i], axis=(1, 2))]
        if not len(candidates):
            continue
        c, d = a[candidates], b[candidates]
        s1, s2 = cross(b[i]-a[i], c-a[i]), cross(b[i]-a[i], d-a[i])
        s3, s4 = cross(d-c, a[i]-c), cross(d-c, b[i]-c)
        opposite = lambda x, y: ((x <= eps) & (y >= -eps)) | ((y <= eps) & (x >= -eps))
        if np.any(opposite(s1, s2) & opposite(s3, s4)):
            return False
    return True


def frame(v, f, normal_cone=None):
    origin = v.mean(axis=0)
    _, _, axes = np.linalg.svd(v-origin, full_matrices=False)
    cross = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    if cross.sum(axis=0)@axes[2] < 0:
        axes[2] *= -1
    if normal_cone is not None:
        # Center the complete source-normal cone. PCA on a narrow ribbon can
        # choose a nearly tangent height axis and leave its seam ill-conditioned.
        from scipy.optimize import minimize
        directions = np.r_[cross/np.linalg.norm(cross, axis=1)[:, None], normal_cone.reshape(-1, 3)]
        initial = axes[2]/max(.05, float(np.min(directions@axes[2])))
        solved = minimize(lambda x: .5*np.dot(x, x), initial, jac=lambda x: x,
            constraints={'type': 'ineq', 'fun': lambda x: directions@x-1., 'jac': lambda x: directions},
            method='SLSQP', options={'maxiter': 100, 'ftol': 1e-10})
        if solved.success and np.isfinite(solved.x).all() and np.min(directions@solved.x) >= 1-1e-7:
            candidate = solved.x/np.linalg.norm(solved.x)
            if np.min(directions@candidate) > np.min(directions@axes[2]):
                axes[2] = candidate
                tangent = axes[0]-np.dot(axes[0], candidate)*candidate
                if np.linalg.norm(tangent) < 1e-10:
                    tangent = axes[1]-np.dot(axes[1], candidate)*candidate
                axes[0] = tangent/np.linalg.norm(tangent)
    axes[1] = np.cross(axes[2], axes[0])
    points = (v-origin)@axes.T
    lo = points[:, :2].min(axis=0); size = np.ptp(points[:, :2], axis=0)
    if np.min(size) <= 1e-12:
        raise ValueError('Collapsed graph extent')
    uv = (points[:, :2]-lo)/size
    normal_dot = cross@axes[2]/np.linalg.norm(cross, axis=1)
    return origin, axes, lo, size, uv, points[:, 2], normal_dot


def parameters(points, rec):
    return ((points-rec['origin'])@rec['axes'][:2].T-rec['lo'])/rec['size']


def positions(uv, rec, du=0, dv=0):
    t = rec['knots']; h = tensor(uv, t, t, du, dv)@rec['height']
    if du or dv:
        base = np.zeros((len(uv), 3))
        if du == 1 and dv == 0:
            base += rec['size'][0]*rec['axes'][0]
        if dv == 1 and du == 0:
            base += rec['size'][1]*rec['axes'][1]
        return base+h[:, None]*rec['axes'][2]
    xy = rec['lo']+uv*rec['size']
    return rec['origin']+xy@rec['axes'][:2]+h[:, None]*rec['axes'][2]


def surface_poles(rec):
    t = rec['knots']; n = len(t)-4
    g = np.array([np.mean(t[i+1:i+4]) for i in range(n)])
    q = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
    xy = rec['lo']+q*rec['size']
    return (rec['origin']+xy@rec['axes'][:2]+rec['height'][:, None]*rec['axes'][2]).reshape(n, n, 3)


def polyline_graph_error(points, rec):
    """Maximize the scalar curve/support residual on every polynomial span.

    A bicubic height restricted to a straight UV segment has degree at most
    six. Split at all crossed knots, recover that polynomial on Chebyshev
    nodes, then inspect its endpoints and real derivative roots. This avoids
    underestimating individual edge tolerances with a fixed sample grid.
    """
    from numpy.polynomial.chebyshev import chebvander, chebder, chebroots, chebval
    points = np.asarray(points); uv = parameters(points, rec)
    intervals = []; knots_unique = np.unique(rec['knots'])[1:-1]
    for index, (a, b) in enumerate(zip(uv[:-1], uv[1:])):
        cuts = [0., 1.]
        for axis in range(2):
            delta = b[axis]-a[axis]
            if abs(delta) > 1e-15:
                values = (knots_unique-a[axis])/delta
                cuts.extend(values[(values > 0) & (values < 1)])
        cuts = np.unique(cuts)
        intervals.extend((index, start, end) for start, end in zip(cuts[:-1], cuts[1:]))
    nodes = np.cos(np.arange(7)*np.pi/6)
    ids = np.array([r[0] for r in intervals], dtype=int)
    ends = np.array([r[1:] for r in intervals])
    t = ends[:, :1]+(ends[:, 1:]-ends[:, :1])*(nodes+1)/2
    xyz = points[ids, None]*(1-t[..., None])+points[ids+1, None]*t[..., None]
    q = parameters(xyz.reshape(-1, 3), rec)
    residual = (tensor(q, rec['knots'], rec['knots'])@rec['height']-
                (xyz.reshape(-1, 3)-rec['origin'])@rec['axes'][2]).reshape(-1, 7)
    coefficients = np.linalg.solve(chebvander(nodes, 6), residual.T)
    derivatives = chebder(coefficients, axis=0); maximum = float(np.max(abs(residual)))
    for coeff, derivative in zip(coefficients.T, derivatives.T):
        roots = chebroots(derivative)
        roots = roots.real[(abs(roots.imag) < 1e-8) & (abs(roots.real) <= 1)]
        if len(roots):
            maximum = max(maximum, float(np.max(abs(chebval(roots, coeff)))))
    return maximum


def spline_graph_error(poles, curve_knots, rec, return_parameter=False):
    """Polynomial extrema of a cubic spatial spline against a bicubic graph.

    Split at curve knots and every crossed surface knot. On each resulting
    interval the scalar residual is a polynomial of degree at most eighteen.
    """
    from scipy.interpolate import BSpline, PPoly
    from numpy.polynomial.chebyshev import chebvander, chebder, chebroots, chebval
    degree = len(curve_knots)-len(poles)-1; polynomial_degree = 6*degree
    uv_poles = parameters(poles, rec); cuts = list(np.unique(curve_knots))
    for axis in range(2):
        polynomial = PPoly.from_spline((curve_knots, uv_poles[:, axis], degree))
        for knot in np.unique(rec['knots'])[1:-1]:
            roots = polynomial.solve(float(knot), extrapolate=False)
            cuts.extend(roots[np.isfinite(roots) & (roots > 0) & (roots < 1)])
    cuts = np.unique(cuts); cuts = cuts[np.r_[True, np.diff(cuts) > 1e-12]]
    nodes = np.cos(np.arange(polynomial_degree+1)*np.pi/polynomial_degree)
    t = cuts[:-1, None]+np.diff(cuts)[:, None]*(nodes+1)/2
    xyz = BSpline(curve_knots, poles, degree)(t.ravel()); q = parameters(xyz, rec)
    if np.any(q < -1e-9) or np.any(q > 1+1e-9):
        raise ValueError(f'Smoothed trim curve leaves its graph support: min={q.min(axis=0)}, max={q.max(axis=0)}')
    residual = (tensor(q, rec['knots'], rec['knots'])@rec['height']-
                (xyz-rec['origin'])@rec['axes'][2]).reshape(-1, polynomial_degree+1)
    coefficients = np.linalg.solve(chebvander(nodes, polynomial_degree), residual.T)
    peak = np.unravel_index(np.argmax(abs(residual)), residual.shape)
    maximum = float(abs(residual[peak])); where = float(t[peak])
    for interval, coeff in enumerate(coefficients.T):
        roots = chebroots(chebder(coeff))
        roots = roots.real[(abs(roots.imag) < 1e-8) & (abs(roots.real) <= 1)]
        if len(roots):
            values = abs(chebval(roots, coeff)); index = int(np.argmax(values))
            if values[index] > maximum:
                maximum = float(values[index])
                where = float(cuts[interval]+(roots[index]+1)*(cuts[interval+1]-cuts[interval])/2)
    return (maximum, where) if return_parameter else maximum


def solve_graph(v, f, normals, boundary, config, condition_frame=False):
    origin, axes, lo, size, uv, heights, dot = frame(v, f, normals if condition_frame else None)
    if np.min(dot) <= .05:
        return None, dict(reason='non_graph', minimum_normal_dot=float(np.min(dot)))
    if condition_frame and np.min(normals.reshape(-1, 3)@axes[2]) <= .1:
        return None, dict(reason='ill_conditioned_source_normal_cone')
    try:
        chart_loops = loops(boundary)
    except ValueError as error:
        return None, dict(reason=str(error))
    if not projected_boundary_valid(uv, boundary):
        return None, dict(reason='overlapping_projected_boundary')
    # All source vertices and all triangle centers contribute to the fit.
    centers = v[f].mean(axis=1)
    if config.interior_subdivisions > 1:
        subdivisions = config.interior_subdivisions
        weights = np.array([[i, j, subdivisions-i-j]
                            for i in range(subdivisions+1) for j in range(subdivisions+1-i)
                            if max(i, j, subdivisions-i-j) < subdivisions], dtype=float)/subdivisions
        interior = np.einsum('bc,tcd->tbd', weights, v[f]).reshape(-1, 3)
        points = np.r_[v, centers, interior]
    else:
        points = np.r_[v, centers]
    q = ((points-origin)@axes[:2].T-lo)/size
    z = (points-origin)@axes[2]
    # Curve constraints cover every segment, not just a small random sample.
    w = np.linspace(0., 1., 17)
    bp = (v[boundary[:, 0], None]*(1-w[None, :, None])+v[boundary[:, 1], None]*w[None, :, None]).reshape(-1, 3)
    bq = ((bp-origin)@axes[:2].T-lo)/size
    bz = (bp-origin)@axes[2]
    if normals.ndim == 3:
        oriented = f[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2)
        keys = oriented[:, 0]*len(v)+oriented[:, 1]; order = np.argsort(keys)
        use = order[np.searchsorted(keys[order], boundary[:, 0]*len(v)+boundary[:, 1])]
        boundary_nn = np.stack([normals[use//3, use%3], normals[use//3, (use%3+1)%3]], axis=1)
        normal_uv = uv[f].reshape(-1, 2); nn = normals.reshape(-1, 3)@axes.T
    else:
        boundary_nn = normals[boundary]; normal_uv = uv; nn = normals@axes.T
    bn = (boundary_nn[:, 0, None]*(1-w[None, :, None])+
          boundary_nn[:, 1, None]*w[None, :, None]).reshape(-1, 3)@axes.T
    bgood = bn[:, 2] > .1
    boundary_gradients = -bn[bgood, :2]/bn[bgood, 2, None]*size
    good = nn[:, 2] > .1
    # Gradient constraints use normals estimated within the original region,
    # so later chart splits do not introduce independently estimated tangents.
    gradients = -nn[good, :2]/nn[good, 2, None]*size
    history = []
    for n in config.controls:
        t = knots(n); a = tensor(q, t, t); b = tensor(bq, t, t)
        blocks = [a/np.sqrt(len(q)), b*np.sqrt(100./len(bq))]
        rhs = [z/np.sqrt(len(q)), bz*np.sqrt(100./len(bq))]
        if np.any(good) and config.normal_weight:
            for axis in range(2):
                op = tensor(normal_uv[good], t, t, int(axis == 0), int(axis == 1))
                # Normal equations must not dominate positional tolerances on
                # micron-sized charts: use their physical length scale.
                weight = np.sqrt(config.normal_weight/good.sum())*np.min(size)/size[axis]
                blocks.append(op*weight); rhs.append(gradients[:, axis]*weight)
        if np.any(bgood) and config.boundary_normal_weight:
            for axis in range(2):
                op = tensor(bq[bgood], t, t, int(axis == 0), int(axis == 1))
                weight = np.sqrt(config.boundary_normal_weight/bgood.sum())*np.min(size)/size[axis]
                blocks.append(op*weight); rhs.append(boundary_gradients[:, axis]*weight)
        grid = np.linspace(0., 1., max(5, n+1))
        fq = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        for du, dv in [(2, 0), (1, 1), (0, 2)]:
            op = tensor(fq, t, t, du, dv)
            weight = np.sqrt(config.fairness/len(fq))/(size[0]**du*size[1]**dv)
            blocks.append(op*weight); rhs.append(np.zeros(len(fq)))
        matrix = sparse.vstack(blocks, format='csr'); target = np.concatenate(rhs)
        h = (matrix.T@matrix).tocsc()
        regularizer = max(float(h.diagonal().max()), 1.)*1e-14
        coeff = spsolve(h+sparse.eye(n*n, format='csc')*regularizer, matrix.T@target)
        err = np.abs(a@coeff-z); be = np.abs(b@coeff-bz)
        item = dict(controls=n, distance_max=float(err.max()), boundary_max=float(be.max()))
        history.append(item)
        if np.isfinite(coeff).all() and err.max() <= config.distance and be.max() <= config.boundary*.55:
            rec = dict(origin=origin, axes=axes, lo=lo, size=size, knots=t, height=coeff,
                       loops=chart_loops, uv=uv)
            return rec, dict(reason='accepted', attempts=history, controls=n,
                             training_distance=stats(err), training_boundary=stats(be))
    # Neighbourhood normals can disagree with a genuinely planar source cell.
    # A soft normal term must not prevent its exact geometric representation.
    # This fallback is restricted to numerical coplanarity, not a relaxed fit.
    po, pa, pl, ps, puv, ph, pdot = frame(v, f)
    numerical = 64*np.finfo(float).eps*max(1., float(np.max(abs(v))))
    if (np.max(abs(ph)) <= min(numerical, config.distance, config.boundary*.55)
            and np.min(pdot) > .05 and projected_boundary_valid(puv, boundary)):
        rec = dict(origin=po, axes=pa, lo=pl, size=ps, knots=knots(4), height=np.zeros(16),
                   loops=chart_loops, uv=puv)
        return rec, dict(reason='exact_planar_geometry_fallback', attempts=history, controls=4,
                         source_planarity_max_mm=float(np.max(abs(ph))),
                         normal_target_relaxed=True)
    return None, dict(reason='fit_budget', attempts=history)


def split_faces(v, f, ids, graph):
    """Split representation inside one fixed input label, preserving all faces."""
    p = v[f[ids]].mean(axis=1); axis = np.argmax(np.ptp(p, axis=0))
    order = np.argsort(p[:, axis], kind='stable'); cut = len(ids)//2
    result = []
    for selected in (ids[order[:cut]], ids[order[cut:]]):
        if not len(selected):
            continue
        count, components = sparse.csgraph.connected_components(graph[selected][:, selected], directed=False)
        result.extend(selected[components == k] for k in range(count))
    return result


def fit_partition(v, f, labels, config, out):
    edges, owners = incidence(f)
    shared_normals = sharp_vertices = None
    if config.shared_smooth_normals:
        shared_normals = vertex_normals(v, f)
        normal = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
        normal /= np.linalg.norm(normal, axis=1)[:, None]
        sharp = np.sum(normal[owners[:, 0]]*normal[owners[:, 1]], axis=1) < np.cos(np.radians(config.feature_angle))
        sharp_vertices = np.unique(edges[sharp])
    graph = sparse.coo_matrix((np.ones(2*len(edges)),
                              (owners.ravel(), owners[:, ::-1].ravel())), shape=(len(f), len(f))).tocsr()
    records, reports, splits = [], [], []
    face_chart = np.full(len(f), -1, dtype=np.int64)
    out = Path(out); (out/'charts').mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    for region in np.unique(labels):
        region_faces = np.flatnonzero(labels == region)
        region_vertices = np.unique(f[region_faces])
        region_normals = vertex_normals(v[region_vertices], np.searchsorted(region_vertices, f[region_faces]))
        if shared_normals is not None:
            smooth = ~np.isin(region_vertices, sharp_vertices)
            region_normals[smooth] = shared_normals[region_vertices[smooth]]
        components, ci = sparse.csgraph.connected_components(graph[region_faces][:, region_faces], directed=False)
        queue = [(region_faces[ci == k], 0) for k in range(components)]
        while queue:
            ids, depth = queue.pop(0)
            used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids]); points = v[used]
            normal = region_normals[np.searchsorted(region_vertices, used)]
            boundary = boundary_edges(tri)
            rec, report = solve_graph(points, tri, normal, boundary, config)
            if rec is None:
                splits.append(dict(region=int(region), triangles=len(ids), depth=depth, **report))
                if depth >= config.max_depth or len(ids) <= 1:
                    dump(out/'fit_failure.json', dict(region=int(region), triangles=ids.tolist(), report=report))
                    raise ValueError(f'Cannot fit region {region}, {len(ids)} triangles: {report}')
                queue.extend((part, depth+1) for part in split_faces(v, f, ids, graph))
                continue
            chart = len(records)
            if chart >= config.max_charts:
                raise ValueError('Declared chart budget exceeded')
            rec['region'] = int(region); rec['triangle_ids'] = ids; rec['vertex_ids'] = used
            rec['loops'] = [used[loop] for loop in rec['loops']]
            face_chart[ids] = chart
            np.savez_compressed(out/'charts'/f'{chart:05d}.npz', **{k: value for k, value in rec.items() if k not in ('loops', 'uv')})
            report.update(chart=chart, region=int(region), triangles=len(ids), depth=depth)
            records.append(rec); reports.append(report)
        if int(region) % 50 == 0:
            print(json.dumps(dict(stage='fit', region=int(region), charts=len(records), seconds=time.perf_counter()-start)), flush=True)
    if np.any(face_chart < 0) or any(len(np.unique(labels[r['triangle_ids']])) != 1 for r in records):
        raise ValueError('Missing source coverage or input partition crossed')
    np.savez_compressed(out/'chart_index.npz', face_chart=face_chart, source_labels=labels)
    dump(out/'fitting.json', dict(charts=reports, splits=splits, input_regions=len(np.unique(labels)),
                                 fitted_charts=len(records), source_triangles=len(f), complete_coverage=True,
                                 partition_labels_unchanged=True, seconds=time.perf_counter()-start))
    return records, face_chart, edges, owners


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--partition', required=True); parser.add_argument('--stl', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--distance', type=float, default=.03)
    parser.add_argument('--boundary', type=float, default=.001)
    parser.add_argument('--controls', type=int, nargs='+', default=[4, 7, 11, 17, 25])
    parser.add_argument('--face-limit', type=int, default=8000)
    parser.add_argument('--normal-weight', type=float, default=.0001)
    parser.add_argument('--shared-smooth-normals', action='store_true')
    parser.add_argument('--boundary-normal-weight', type=float, default=0.)
    parser.add_argument('--interior-subdivisions', type=int, default=0)
    args = parser.parse_args(); out = Path(args.output); out.mkdir(parents=True, exist_ok=False)
    start = time.perf_counter(); config = Config(distance=args.distance, boundary=args.boundary,
        controls=tuple(args.controls), max_charts=args.face_limit, normal_weight=args.normal_weight,
        shared_smooth_normals=args.shared_smooth_normals, boundary_normal_weight=args.boundary_normal_weight,
        interior_subdivisions=args.interior_subdivisions)
    if config.distance <= 0 or config.boundary <= 0 or any(n < 4 for n in config.controls):
        raise ValueError('Positive tolerances and cubic control counts >= 4 are required')
    run = dict(status='running', config=asdict(config), partition=str(Path(args.partition).resolve()),
               stl=str(Path(args.stl).resolve()), source_CAD_read=False, units='mm',
               algorithm='fixed-partition-graph-v2-length-scaled-normals')
    dump(out/'run.json', run)
    try:
        v, f, labels, topo = load_partition(args.partition, args.stl)
        run['input_topology'] = topo; run['read_seconds'] = time.perf_counter()-start
        rec, face_chart, edges, owners = fit_partition(v, f, labels, config, out)
        from .partition_cad import assemble
        result = assemble(v, f, rec, face_chart, edges, owners, config.boundary, out)
        run.update(result); run['status'] = 'DiagnosticOnly'; run['can_apply'] = False
    except Exception as error:
        run.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        run['wall_seconds'] = time.perf_counter()-start; dump(out/'run.json', run)


if __name__ == '__main__':
    main()
