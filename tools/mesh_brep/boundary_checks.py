"""Continuous planar curve intersections for graph-chart trim wires."""
import numpy as np
from scipy.interpolate import BSpline
from scipy.optimize import least_squares
from OCP.Geom2dAPI import Geom2dAPI_InterCurveCurve
from OCP.Geom2dAdaptor import Geom2dAdaptor_Curve
from OCP.Geom2dInt import Geom2dInt_GInter
from .compact import curve
from .partition_cad import projected_curve


def distinct_intersection(spline, first, second):
    """Refine a kernel candidate without converging to the trivial s == t root.

    The kernel's overlap segments can include distinct parameters whose actual
    positions differ substantially. A divided difference removes the diagonal
    root before testing for a genuine second branch.
    """
    if abs(first-second) <= 1e-9:
        return False
    def residual(q):
        a, b = q
        if abs(a-b) <= 1e-10:
            return spline((a+b)/2, 1)
        return (spline(a)-spline(b))/(a-b)
    result = least_squares(residual, np.clip([first, second], 0., 1.), bounds=(0., 1.),
                           ftol=1e-13, xtol=1e-13, gtol=1e-13, max_nfev=80)
    a, b = result.x
    # Near a stationary point, distinct floating-point parameters can still
    # describe one unresolved position. Require a spatially resolved branch.
    excursion = np.linalg.norm(spline(np.linspace(a, b, 9))-spline(a), axis=1).max()
    return (abs(a-b) > 1e-8 and np.linalg.norm(residual(result.x)) < 1e-8
            and np.linalg.norm(spline(a)-spline(b)) <= 1e-10 and excursion > 1e-9)


def intersections(records, incident, frame, native_curves=None, details=None):
    ids = sorted(map(int, incident)); projected, boxes, splines = {}, {}, {}
    for ci in ids:
        c = records[ci]
        native = curve(c['poles'], c['knots']) if native_curves is None else native_curves[ci]
        pc = projected_curve(native, frame); projected[ci] = pc
        xy = ((c['poles']-frame['origin'])@frame['axes'][:2].T-frame['lo'])/frame['size']
        boxes[ci] = xy.min(axis=0), xy.max(axis=0)
        splines[ci] = BSpline(c['knots'], xy, 3)
    invalid = set()
    for i, ci in enumerate(ids):
        adaptor = Geom2dAdaptor_Curve(projected[ci])
        self_check = Geom2dInt_GInter(adaptor, 1e-10, 1e-10)
        if not self_check.IsDone():
            raise ValueError('Continuous trim self-intersection check did not complete')
        distinct = lambda p: distinct_intersection(splines[ci], p.ParamOnFirst(), p.ParamOnSecond())
        self_crossing = any(distinct(self_check.Point(k)) for k in range(1, self_check.NbPoints()+1))
        for k in range(1, self_check.NbSegments()+1):
            segment = self_check.Segment(k)
            # The kernel also reports pieces of the trivial diagonal t == t.
            # They are the same point on the same curve, not a second branch.
            self_crossing |= (not segment.HasFirstPoint() or not segment.HasLastPoint()
                              or distinct(segment.FirstPoint()) or distinct(segment.LastPoint()))
        if self_crossing:
            invalid.add(ci)
            if details is not None:
                details.append(dict(kind='self', curves=[ci], points=self_check.NbPoints(), segments=self_check.NbSegments()))
        for cj in ids[i+1:]:
            if np.any(boxes[ci][1] < boxes[cj][0]-1e-10) or np.any(boxes[cj][1] < boxes[ci][0]-1e-10):
                continue
            check = Geom2dAPI_InterCurveCurve(projected[ci], projected[cj], 1e-10)
            if check.NbSegments():
                invalid.update((ci, cj))
                if details is not None:
                    details.append(dict(kind='overlap', curves=[ci, cj], segments=check.NbSegments()))
                continue
            first, second = records[ci]['chain'], records[cj]['chain']
            common = set(map(int, first[[0, -1]])) & set(map(int, second[[0, -1]]))
            allowed = [projected[ci].Value(float(t)) for vi, t in zip(first[[0, -1]], [0., 1.]) if int(vi) in common]
            for k in range(1, check.NbPoints()+1):
                point = check.Point(k)
                if not any(point.Distance(endpoint) <= 1e-8 for endpoint in allowed):
                    invalid.update((ci, cj))
                    if details is not None:
                        details.append(dict(kind='crossing', curves=[ci, cj],
                            distance_to_shared_endpoint=min((point.Distance(p) for p in allowed), default=None)))
    return invalid
