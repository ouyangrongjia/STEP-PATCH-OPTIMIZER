"""Independent interior probes between the original fitting samples.

Evaluate inside the supplied triangle domain, using its piecewise-linear
height as the target. Knot-span probes expose narrow spline excursions which
can be absent at every source vertex and triangle centroid.
"""
import numpy as np
from .mesh import Locator
from .partition_fit import parameters
from .splines import tensor

VERSION = 1


def probes(record, vertices, triangles):
    uv = parameters(vertices, record)
    heights = (vertices-record['origin'])@record['axes'][2]
    spans = np.unique(record['knots'])
    nodes = np.polynomial.legendre.leggauss(4)[0]
    grid = (spans[:-1, None]+np.diff(spans)[:, None]*(nodes+1)/2).ravel()
    points = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    # Include edge midpoints so very narrow source strips remain represented.
    edges = np.unique(np.sort(np.concatenate([triangles[:, [0, 1]], triangles[:, [1, 2]],
                                               triangles[:, [2, 0]]]), axis=1), axis=0)
    points = np.unique(np.r_[points, uv[edges].mean(axis=1)], axis=0)
    locator = Locator(np.c_[uv, np.zeros(len(uv))], triangles)
    _, distance, ids, bary = locator.closest(np.c_[points, np.zeros(len(points))])
    inside = distance < 1e-10
    target = np.einsum('ij,ij->i', bary[inside], heights[triangles[ids[inside]]])
    return points[inside], target


def check(task):
    index, record, vertices, triangles = task
    q, z = probes(record, vertices, triangles)
    error = abs(tensor(q, record['knots'], record['knots'])@record['height']-z)
    return index, dict(interior_probe_version=VERSION, interior_probe_count=len(q),
                       interior_probe_max_mm=float(error.max(initial=0.)))
