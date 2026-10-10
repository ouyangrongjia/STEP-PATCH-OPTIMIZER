"""Thin-plate energy restricted to a graph chart's source polygon."""
import numpy as np
from scipy import sparse
from .splines import tensor


def source_polygon(uv, triangles):
    from shapely.geometry import Polygon
    from .partition_fit import boundary_edges, loops
    rings = [uv[ids] for ids in loops(boundary_edges(triangles))]
    areas = [abs(np.sum(r[:, 0]*np.roll(r[:, 1], -1)-r[:, 1]*np.roll(r[:, 0], -1)))/2 for r in rings]
    outer = int(np.argmax(areas))
    polygon = Polygon(rings[outer], [r for i, r in enumerate(rings) if i != outer])
    ab = uv[triangles[:, 1]]-uv[triangles[:, 0]]
    ac = uv[triangles[:, 2]]-uv[triangles[:, 0]]
    area = abs(np.sum(ab[:, 0]*ac[:, 1]-ab[:, 1]*ac[:, 0]))/2
    if not polygon.is_valid or not np.isclose(polygon.area, area, rtol=1e-8, atol=1e-12):
        raise ValueError('Source graph domain is not a valid, singly covered polygon')
    return polygon


def quadrature(polygon, knot):
    """Degree-eight triangle quadrature, split at every tensor knot line."""
    import shapely
    spans = np.unique(knot)
    i, j = np.meshgrid(np.arange(len(spans)-1), np.arange(len(spans)-1), indexing='ij')
    boxes = shapely.box(spans[i.ravel()], spans[j.ravel()], spans[i.ravel()+1], spans[j.ravel()+1])
    parts = shapely.get_parts(shapely.intersection(boxes, polygon))
    parts = parts[shapely.area(parts) > 1e-16]
    triangles = shapely.get_parts(shapely.constrained_delaunay_triangles(parts))
    points = np.asarray([np.asarray(t.exterior.coords)[:3] for t in triangles])
    a, ab, ac = points[:, 0], points[:, 1]-points[:, 0], points[:, 2]-points[:, 0]
    determinant = abs(ab[:, 0]*ac[:, 1]-ab[:, 1]*ac[:, 0])
    nodes, weights = np.polynomial.legendre.leggauss(5)
    r, s = np.meshgrid((nodes+1)/2, (nodes+1)/2, indexing='ij')
    r, s = r.ravel(), s.ravel()
    w = (np.outer(weights, weights)/4).ravel()*(1-r)
    q = a[:, None]+r[None, :, None]*ab[:, None]+((1-r)*s)[None, :, None]*ac[:, None]
    w = (determinant[:, None]*w).ravel()
    if not np.isclose(w.sum(), polygon.area, rtol=1e-8, atol=1e-12):
        raise ValueError('Trimmed fairness quadrature does not cover the source polygon')
    return q.reshape(-1, 2), w/polygon.area


def bending_matrix(polygon, knot, size):
    q, weights = quadrature(polygon, knot)
    count = len(knot)-4
    result = sparse.csc_matrix((count*count, count*count))
    for du, dv, factor in ((2, 0, 1), (1, 1, 2), (0, 2, 1)):
        b = tensor(q, knot, knot, du, dv)
        result += (b.T@b.multiply(weights[:, None]))*(factor/size[0]**(2*du)/size[1]**(2*dv))
    return result.tocsc()
