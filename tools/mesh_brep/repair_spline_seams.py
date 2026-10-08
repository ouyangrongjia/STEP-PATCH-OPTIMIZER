"""Reconcile paired rectangular spline boundaries by one common control curve.

This edits support geometry, never edge tolerance. Only open edges sharing both
endpoints are eligible; ambiguous pairs and excessive displacements fail closed.
"""
import argparse
from pathlib import Path
import time
import numpy as np
from OCP.BRep import BRep_Tool, BRep_Builder
from OCP.BRepTools import BRepTools
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace, BRepBuilderAPI_Sewing, BRepBuilderAPI_MakeSolid
from OCP.BRepLib import BRepLib
from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_SHELL, TopAbs_REVERSED
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS, TopoDS_Shape
from OCP.gp import gp_Pnt
from . import cad
from .mesh import dump
from .partition_cad import topology


def split_rectangle(surface, tolerance):
    """Exact diagonal trims expose a self seam as an ordinary two-face seam."""
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire
    from OCP.GCE2d import GCE2d_MakeSegment
    from OCP.gp import gp_Pnt2d
    result = []
    for uv in [[(0.,0.),(1.,0.),(1.,1.)], [(0.,0.),(1.,1.),(0.,1.)]]:
        # Separate surface handles retain the two different seam pcurves.
        support = surface.Copy()
        wire = BRepBuilderAPI_MakeWire()
        for a,b in zip(uv,uv[1:]+uv[:1]):
            curve = GCE2d_MakeSegment(gp_Pnt2d(*a),gp_Pnt2d(*b)).Value()
            edge = BRepBuilderAPI_MakeEdge(curve,support).Edge()
            if not BRepLib.BuildCurve3d_s(edge,tolerance*.1): raise ValueError('Diagonal trim curve construction failed')
            wire.Add(edge)
        result.append(BRepBuilderAPI_MakeFace(support,wire.Wire(),True).Face())
    return result


def read(path):
    if Path(path).suffix.lower() == '.brep':
        shape = TopoDS_Shape(); BRepTools.Read_s(shape, str(path), BRep_Builder()); return shape
    return cad.read_generated(path)


def side(edge, face):
    a, b = BRep_Tool.Range_s(edge); pc = BRep_Tool.CurveOnSurface_s(edge, face, a, b)
    p, q = np.array(pc.Value(a).Coord()), np.array(pc.Value(b).Coord())
    axis = int(np.argmax(np.abs(q-p))); fixed = 1-axis; value = float((p[fixed]+q[fixed])/2)
    if abs(p[fixed]-q[fixed]) > 1e-9 or min(abs(value), abs(value-1)) > 1e-9:
        raise ValueError('Repair requires a natural rectangular boundary')
    return axis, int(value > .5)


def knots(surface, axis, reverse):
    n = surface.NbUKnots() if axis == 0 else surface.NbVKnots()
    get = surface.UKnot if axis == 0 else surface.VKnot
    mult = surface.UMultiplicity if axis == 0 else surface.VMultiplicity
    return [(1-get(i) if reverse else get(i), mult(i)) for i in range(1, n+1)]


def boundary(surface, axis, high, reverse):
    nu, nv = surface.NbUPoles(), surface.NbVPoles()
    ids = [(i, nv if high else 1) for i in range(1, nu+1)] if axis == 0 else [(nu if high else 1, i) for i in range(1, nv+1)]
    if reverse: ids = ids[::-1]
    return ids, np.array([surface.Pole(i, j).Coord() for i, j in ids])


def require_natural_rectangle(face):
    """Never silently replace a trimmed face or hole by its whole support."""
    from OCP.Geom import Geom_BSplineSurface
    s=BRep_Tool.Surface_s(face)
    if not isinstance(s,Geom_BSplineSurface) or np.max(np.abs(np.array(s.Bounds())-[0,1,0,1]))>1e-10:
        raise ValueError('Repair requires normalized B-spline rectangles')
    covered={key:[] for key in [(0,0),(0,1),(1,0),(1,1)]}
    for edge in cad.shapes(face,TopAbs_EDGE):
        axis,high=side(edge,face);a,b=BRep_Tool.Range_s(edge)
        pc=BRep_Tool.CurveOnSurface_s(edge,face,a,b)
        q=np.array([pc.Value(float(t)).Coord() for t in np.linspace(a,b,7)])
        if np.max(np.abs(q[:,1-axis]-high))>1e-9:
            raise ValueError('Repair cannot replace a nonrectangular trim')
        covered[axis,high].append((float(q[:,axis].min()),float(q[:,axis].max())))
    for intervals in covered.values():
        intervals.sort()
        if not intervals or abs(intervals[0][0])>1e-9 or abs(intervals[-1][1]-1)>1e-9:
            raise ValueError('Repair requires all four natural rectangle sides')
        if any(abs(a[1]-b[0])>1e-9 for a,b in zip(intervals,intervals[1:])):
            raise ValueError('Repair found overlapping or incomplete rectangular trims')


def reconcile(source, output, max_gap=.1, tolerance=1e-6, preserve_ribbon=True):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    shape = read(source); faces = cad.shapes(shape, TopAbs_FACE)
    report = dict(source=str(Path(source).resolve()), max_allowed_gap_mm=max_gap, tolerance_mm=tolerance,
                  before=topology(shape), status='running', preserve_ribbon=preserve_ribbon)
    dump(out/'run.json', report)
    try:
        index = TopTools_IndexedMapOfShape(); uses = []
        for fi, face in enumerate(faces):
            require_natural_rectangle(face)
            for edge in cad.shapes(face, TopAbs_EDGE):
                if BRep_Tool.Degenerated_s(edge): continue
                j = index.Add(edge)-1
                if j == len(uses): uses.append([])
                uses[j].append(fi)
        free = []; surfaces = [BRep_Tool.Surface_s(f).Copy() for f in faces]
        for j, owner in enumerate(uses):
            if len(owner) != 1: continue
            edge = TopoDS.Edge_s(index.FindKey(j+1)); fi = owner[0]; s = surfaces[fi]
            if s.IsURational() or s.IsVRational() or np.max(np.abs(np.array(s.Bounds())-[0,1,0,1])) > 1e-10:
                raise ValueError('Repair requires non-rational normalized B-spline surfaces')
            axis, high = side(edge, faces[fi]); _, cp = boundary(s, axis, high, False)
            free.append(dict(edge=j, face=fi, axis=axis, high=high, endpoints=cp[[0,-1]]))
        paired = set(); records = []; self_seams = set()
        for i, a in enumerate(free):
            if i in paired: continue
            matches = []
            for j, b in enumerate(free):
                if j == i or j in paired: continue
                d = np.linalg.norm(a['endpoints']-b['endpoints'], axis=1).max()
                dr = np.linalg.norm(a['endpoints']-b['endpoints'][::-1], axis=1).max()
                if min(d, dr) <= tolerance: matches.append((j, dr < d))
            if len(matches) != 1: raise ValueError(f'Open edge {a["edge"]} has {len(matches)} endpoint matches')
            j, reverse = matches[0]; b = free[j]; pair = [(a, False), (b, reverse)]
            if a['face'] == b['face']: self_seams.add(a['face'])
            degree = max(surfaces[x['face']].UDegree() if x['axis'] == 0 else surfaces[x['face']].VDegree() for x, rev in pair)
            common = {}
            for x, rev in pair:
                s = surfaces[x['face']]
                s.IncreaseDegree(degree if x['axis'] == 0 else s.UDegree(), degree if x['axis'] == 1 else s.VDegree())
                for k, m in knots(s, x['axis'], rev): common[round(k, 12)] = max(common.get(round(k, 12), 0), m)
            for x, rev in pair:
                s = surfaces[x['face']]; insert = s.InsertUKnot if x['axis'] == 0 else s.InsertVKnot
                for k, m in sorted(common.items()):
                    if 1e-10 < k < 1-1e-10: insert(1-k if rev else k, m, 1e-10, False)
            rows = [boundary(surfaces[x['face']], x['axis'], x['high'], rev) for x, rev in pair]
            if rows[0][1].shape != rows[1][1].shape: raise ValueError('Common knot refinement failed')
            gap = float(np.linalg.norm(rows[0][1]-rows[1][1], axis=1).max())
            if gap > max_gap: raise ValueError(f'Control curve gap {gap} exceeds the declared bound')
            target = (rows[0][1]+rows[1][1])/2
            # Endpoints already belong to the surrounding edge network.
            target[[0,-1]] = rows[0][1][[0,-1]]
            for (x, rev), (ids, original) in zip(pair, rows):
                s = surfaces[x['face']]
                for k, ((u, v), p, previous) in enumerate(zip(ids, target, original)):
                    s.SetPole(u, v, gp_Pnt(*p))
                    if preserve_ribbon and 0 < k < len(ids)-1:
                        inner = (u, v+(-1 if x['high'] else 1)) if x['axis'] == 0 else (u+(-1 if x['high'] else 1), v)
                        # Other perimeter rows must retain their exact old curve.
                        if 1 < inner[0] < s.NbUPoles() and 1 < inner[1] < s.NbVPoles():
                            old = np.array(s.Pole(*inner).Coord()); s.SetPole(*inner, gp_Pnt(*(old+p-previous)))
            records.append(dict(edges=[a['edge'],b['edge']], faces=[a['face'],b['face']],
                                common_control_points=len(target), control_gap_mm=gap,
                                maximum_control_displacement_mm=gap/2))
            paired.update([i,j])
        report.update(repairs=records,self_seam_faces_split=sorted(self_seams)); dump(out/'run.json', report)
        sew = BRepBuilderAPI_Sewing(tolerance, True, True, True, False)
        sew.SetMinTolerance(tolerance); sew.SetMaxTolerance(tolerance)
        for fi, (old, surface) in enumerate(zip(faces, surfaces)):
            pieces = split_rectangle(surface,tolerance) if fi in self_seams else [BRepBuilderAPI_MakeFace(surface, tolerance).Face()]
            for face in pieces:
                if old.Orientation() == TopAbs_REVERSED: face = TopoDS.Face_s(face.Reversed())
                sew.Add(face)
        sew.Perform(); result = sew.SewedShape(); report['after_sewing'] = topology(result)
        if report['after_sewing']['free_edges'] == 0 and report['after_sewing']['shells'] == 1:
            result = BRepBuilderAPI_MakeSolid(cad.shapes(result, TopAbs_SHELL)[0]).Solid(); BRepLib.OrientClosedSolid_s(result)
        report['after'] = topology(result); BRepTools.Write_s(result, str(out/'model.brep'))
        cad.write_step(out/'model.step', result); report['reread'] = topology(cad.read_generated(out/'model.step'))
        report.update(status='DiagnosticOnly', can_apply=False)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--source', required=True); p.add_argument('--output', required=True)
    p.add_argument('--max-gap', type=float, default=.1); p.add_argument('--no-ribbon', action='store_true')
    a = p.parse_args(); reconcile(a.source, a.output, a.max_gap, preserve_ribbon=not a.no_ribbon)
