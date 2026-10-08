"""Regional variational surface experiments with a common smooth curve network."""
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve

from .mesh import chain_parameters, sample
from .splines import knots, basis
from .compact import curve


def smooth_curve(points, tolerance):
    length = np.r_[0., np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
    t = length/length[-1]
    probe = np.unique(np.r_[t, (t[:-1]+t[1:])/2])
    target = np.stack([np.interp(probe, t, points[:, k]) for k in range(3)], axis=1)
    origin = points[0]
    for n in (4, 6, 9, 13, 19, 28, 42, 64, 96, 144, 216, 324):
        k = knots(n); b = basis(k, probe)
        rhs = target-origin-b[:, -1, None]*(points[-1]-origin)
        # Penalize physical bending, not distance from the global XYZ origin.
        d = basis(k, np.linspace(0, 1, max(25, n*2)), 2)
        a = np.r_[b[:, 1:-1]/np.sqrt(len(probe)), d[:, 1:-1]*1e-5/np.sqrt(len(d))]
        y = np.r_[rhs/np.sqrt(len(probe)), -d[:, -1, None]*(points[-1]-origin)*1e-5/np.sqrt(len(d))]
        cp = np.r_[np.zeros((1, 3)), np.linalg.lstsq(a, y, rcond=1e-12)[0], (points[-1]-origin)[None]]+origin
        error = np.linalg.norm(b@cp-target, axis=1)
        if error.max() <= tolerance:
            return curve(cp, k), dict(controls=n, maximum_polyline_distance_mm=float(error.max()))
    from .partition_fan import fit_curve
    c, error = fit_curve(points, t, tolerance)
    return c, dict(controls=c.NbPoles(), maximum_polyline_distance_mm=error, fallback=True)


def fit_plate(v, tri, curves, tolerance=.001, points=80):
    from OCP.GeomPlate import (GeomPlate_BuildPlateSurface, GeomPlate_CurveConstraint,
                               GeomPlate_PointConstraint, GeomPlate_MakeApprox)
    from OCP.GeomAdaptor import GeomAdaptor_Curve
    from OCP.GeomAbs import GeomAbs_C2
    from OCP.gp import gp_Pnt
    p = GeomPlate_BuildPlateSurface(3, 15, 3, 1e-7, tolerance*.1)
    for c in curves:
        p.Add(GeomPlate_CurveConstraint(GeomAdaptor_Curve(c), 0, 15, tolerance*.1))
    p.SetNbBounds(len(curves))
    xyz, _, _, _, _ = sample(v, tri, points, 9302026)
    for point in xyz:
        p.Add(GeomPlate_PointConstraint(gp_Pnt(*point), 0, tolerance*.1))
    p.Perform()
    if not p.IsDone():
        raise ValueError('Variational plate construction failed')
    approx = GeomPlate_MakeApprox(p.Surface(), tolerance*.1, 64, 8, tolerance*.1, 0, GeomAbs_C2)
    return approx.Surface(), dict(plate_error_mm=p.G0Error(), approximation_error_mm=approx.ApproxError())
