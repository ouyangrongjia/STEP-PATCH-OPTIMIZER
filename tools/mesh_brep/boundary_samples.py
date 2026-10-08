"""One source-derived C2 curve sampled identically by its two incident charts."""
import time
import numpy as np
from scipy.interpolate import BSpline
from .partition_cad import shared_chains
from .regional_rectangle import compact_curve


def smooth_samples(vertices, edges, owners, assignment, selected, tolerance, samples=17):
    started = time.perf_counter()
    pair = assignment[owners]
    keep = (pair[:, 0] != pair[:, 1]) & np.any(np.isin(pair, selected), axis=1)
    source_edges = edges[keep]
    keys = source_edges[:, 0]*len(vertices)+source_edges[:, 1]
    points = np.empty((len(keys), samples, 3)); tangents = np.empty_like(points)
    filled = np.zeros(len(keys), bool); reports = []
    chains, adjacent = shared_chains(edges, owners, assignment)
    fraction = np.linspace(0., 1., samples)
    for chain, sides in zip(chains, adjacent):
        if not np.any(np.isin(sides, selected)):
            continue
        p = vertices[chain]
        t = np.r_[0., np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]; t /= t[-1]
        curve, error = compact_curve(p, t, tolerance, allow_rounded=True)
        poles = np.array([curve.Pole(i).Coord() for i in range(1, curve.NbPoles()+1)])
        knots = np.repeat([curve.Knot(i) for i in range(1, curve.NbKnots()+1)],
                          [curve.Multiplicity(i) for i in range(1, curve.NbKnots()+1)])
        spline = BSpline(knots, poles, curve.Degree())
        u = t[:-1, None]*(1-fraction)+t[1:, None]*fraction
        xyz, tangent = spline(u), spline(u, 1)
        length = np.linalg.norm(tangent, axis=2)
        if np.any(length <= 1e-12):
            raise ValueError('A smoothed boundary has a singular sampled tangent')
        tangent /= length[..., None]
        boundary = np.sort(np.c_[chain[:-1], chain[1:]], axis=1)
        index = np.searchsorted(keys, boundary[:, 0]*len(vertices)+boundary[:, 1])
        reverse = chain[:-1] > chain[1:]
        xyz[reverse] = xyz[reverse, ::-1]; tangent[reverse] = -tangent[reverse, ::-1]
        if np.any(filled[index]):
            raise ValueError('A shared source edge occurs in multiple chains')
        points[index] = xyz; tangents[index] = tangent; filled[index] = True
        reports.append(dict(owners=sides.tolist(), source_segments=len(chain)-1,
                            source_polyline_deviation_mm=float(error)))
    if not np.all(filled):
        raise ValueError('Smoothed boundary samples lost source edge coverage')
    return keys, points, tangents, dict(curves=reports, seconds=time.perf_counter()-started,
        source_edges=len(keys), samples_per_edge=samples, smoothing_budget_mm=tolerance,
        scope='Canonical C2 source curves, before regional fitting; no reference CAD geometry.')
