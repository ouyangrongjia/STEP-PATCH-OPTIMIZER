"""OCCT construction and checks of newly generated surfaces only."""
import numpy as np
from OCP.BRep import BRep_Tool
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace, BRepBuilderAPI_Sewing, BRepBuilderAPI_MakeSolid
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.BRepLib import BRepLib
from OCP.Geom import Geom_BSplineSurface
from OCP.TColgp import TColgp_Array2OfPnt
from OCP.TColStd import TColStd_Array1OfReal, TColStd_Array1OfInteger
from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_SHELL, TopAbs_SOLID
from OCP.TopExp import TopExp
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS
from OCP.gp import gp_Pnt
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.IFSelect import IFSelect_RetDone


def shapes(shape, kind):
    m = TopTools_IndexedMapOfShape(); TopExp.MapShapes_s(shape, kind, m)
    cast = {TopAbs_FACE: TopoDS.Face_s, TopAbs_EDGE: TopoDS.Edge_s, TopAbs_SHELL: TopoDS.Shell_s,
            TopAbs_SOLID: TopoDS.Solid_s}.get(kind, lambda x: x)
    return [cast(m.FindKey(i)) for i in range(1, m.Extent()+1)]


def surface(p, tu, tv):
    points = TColgp_Array2OfPnt(1, len(p), 1, p.shape[1])
    for i in range(len(p)):
        for j in range(p.shape[1]):
            points.SetValue(i+1, j+1, gp_Pnt(*p[i, j]))
    arrays = []
    for t in [tu, tv]:
        k, m = np.unique(t, return_counts=True)
        knots = TColStd_Array1OfReal(1, len(k)); mult = TColStd_Array1OfInteger(1, len(k))
        for i in range(len(k)):
            knots.SetValue(i+1, float(k[i])); mult.SetValue(i+1, int(m[i]))
        arrays.append((knots, mult))
    return Geom_BSplineSurface(points, arrays[0][0], arrays[1][0], arrays[0][1], arrays[1][1], 3, 3)


def topology(shape):
    faces = shapes(shape, TopAbs_FACE); edges = shapes(shape, TopAbs_EDGE)
    counts = [sum(any(e.IsSame(x) for x in shapes(f, TopAbs_EDGE)) for f in faces) for e in edges]
    return dict(faces=len(faces), edges=len(edges), solids=len(shapes(shape, TopAbs_SOLID)),
                free_edges=sum(x == 1 for x in counts), multiple_edges=sum(x > 2 for x in counts),
                brep_valid=BRepCheck_Analyzer(shape, True).IsValid(),
                maximum_edge_tolerance_mm=max([BRep_Tool.Tolerance_s(e) for e in edges], default=0.))


def construct(poles, knots, orientations, closed, tolerance=1e-7):
    sew = BRepBuilderAPI_Sewing(tolerance, True, True, True, False)
    sew.SetMinTolerance(tolerance); sew.SetMaxTolerance(tolerance)
    for p, (tu, tv), sign in zip(poles, knots, orientations):
        face = BRepBuilderAPI_MakeFace(surface(p, tu, tv), tolerance).Face()
        if sign < 0:
            face = TopoDS.Face_s(face.Reversed())
        sew.Add(face)
    sew.Perform(); shape = sew.SewedShape(); report = topology(shape)
    if closed and report['free_edges'] == 0:
        shells = shapes(shape, TopAbs_SHELL)
        if len(shells) != 1:
            raise ValueError('Closed input produced multiple shells')
        solid = BRepBuilderAPI_MakeSolid(shells[0]).Solid()
        BRepLib.OrientClosedSolid_s(solid); shape = solid
    return shape


def write_step(path, shape):
    writer = STEPControl_Writer(); writer.Transfer(shape, STEPControl_AsIs)
    if writer.Write(str(path)) != IFSelect_RetDone:
        raise RuntimeError('STEP write failed')


def read_generated(path):
    # Kept separate so the isolation launcher can guard this native read.
    from OCP.STEPControl import STEPControl_Reader
    reader = STEPControl_Reader()
    if reader.ReadFile(str(path)) != IFSelect_RetDone:
        raise RuntimeError('Generated STEP reread failed')
    reader.TransferRoots(); return reader.OneShape()


def edge_checks(shape, count=385):
    worst = 0.; failures = 0; records = []
    for edge in shapes(shape, TopAbs_EDGE):
        a, b = BRep_Tool.Range_s(edge); curve = BRep_Tool.Curve_s(edge, a, b)
        owners = [f for f in shapes(shape, TopAbs_FACE) if any(edge.IsSame(x) for x in shapes(f, TopAbs_EDGE))]
        deviations = []
        for face in owners:
            pc = BRep_Tool.CurveOnSurface_s(edge, face, a, b); surf = BRep_Tool.Surface_s(face)
            if pc is None:
                raise ValueError('Missing pcurve')
            error = 0.
            for t in np.linspace(a, b, count):
                uv = pc.Value(float(t)); p = surf.Value(uv.X(), uv.Y()); q = curve.Value(float(t))
                error = max(error, p.Distance(q))
            deviations.append(error); worst = max(worst, error)
        same = BRep_Tool.SameParameter_s(edge); same_range = BRep_Tool.SameRange_s(edge)
        failures += int(not same or not same_range)
        records.append(dict(owners=len(owners), curve_on_surface_mm=deviations, same_parameter=same,
                            same_range=same_range, tolerance_mm=BRep_Tool.Tolerance_s(edge)))
    return dict(max_curve_on_surface_mm=worst, same_parameter_or_range_failures=failures, edges=records)


def expression(shape):
    records = []
    for f in shapes(shape, TopAbs_FACE):
        s = BRep_Tool.Surface_s(f)
        records.append(dict(u=s.NbUPoles(), v=s.NbVPoles(), degree_u=s.UDegree(), degree_v=s.VDegree()))
    return dict(faces=records, actual_support_control_points=sum(x['u']*x['v'] for x in records),
                max_direction=max(max(x['u'], x['v']) for x in records))
