"""Independent OCCT research utilities; all lengths are millimetres."""
from pathlib import Path
import json
import numpy as np
from OCP.STEPControl import STEPControl_Reader, STEPControl_Writer, STEPControl_AsIs
from OCP.IFSelect import IFSelect_RetDone
from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_SOLID, TopAbs_REVERSED
from OCP.TopExp import TopExp
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS, TopoDS_Compound
from OCP.BRep import BRep_Tool, BRep_Builder
from OCP.BRepAdaptor import BRepAdaptor_Curve
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.TopLoc import TopLoc_Location
from OCP.gp import gp_Pnt, gp_Vec
from OCP.BRepCheck import BRepCheck_Analyzer


def dump(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")


def shapes(shape, kind):
    result = TopTools_IndexedMapOfShape()
    TopExp.MapShapes_s(shape, kind, result)
    cast = {TopAbs_FACE: TopoDS.Face_s, TopAbs_EDGE: TopoDS.Edge_s}.get(kind, lambda x: x)
    return [cast(result.FindKey(i)) for i in range(1, result.Extent() + 1)]


def read_step(path):
    reader = STEPControl_Reader()
    if reader.ReadFile(str(path)) != IFSelect_RetDone:
        raise RuntimeError(f"Cannot read {path}")
    reader.TransferRoots()
    return reader.OneShape()


def compound(items):
    builder = BRep_Builder()
    result = TopoDS_Compound()
    builder.MakeCompound(result)
    for item in items:
        builder.Add(result, item)
    return result


def shell(faces):
    from OCP.TopoDS import TopoDS_Shell
    result=TopoDS_Shell();builder=BRep_Builder();builder.MakeShell(result)
    for face in faces:builder.Add(result,face)
    return result


def write_step(path, shape):
    writer = STEPControl_Writer()
    writer.Transfer(shape, STEPControl_AsIs)
    if writer.Write(str(path)) != IFSelect_RetDone:
        raise RuntimeError(f"Cannot write {path}")


def xyz(p):
    return np.array([p.X(), p.Y(), p.Z()])


def evaluate(surface, uv):
    points, normals = [], []
    for u, v in uv:
        p, du, dv = gp_Pnt(), gp_Vec(), gp_Vec()
        surface.D1(float(u), float(v), p, du, dv)
        n = xyz(du.Crossed(dv))
        points.append(xyz(p))
        normals.append(n / max(np.linalg.norm(n), 1e-30))
    return np.asarray(points), np.asarray(normals)


def tessellate(shape, deflection=0.003):
    BRepMesh_IncrementalMesh(shape, deflection, False, 0.1, True).Perform()
    points, tris, uvs, face_ids = [], [], [], []
    for fi, face in enumerate(shapes(shape, TopAbs_FACE)):
        loc = TopLoc_Location()
        mesh = BRep_Tool.Triangulation_s(face, loc)
        if mesh is None:
            continue
        offset = len(points)
        points.extend(xyz(mesh.Node(i).Transformed(loc.Transformation())) for i in range(1, mesh.NbNodes() + 1))
        uvs.extend([mesh.UVNode(i).X(), mesh.UVNode(i).Y()] for i in range(1, mesh.NbNodes() + 1))
        for i in range(1, mesh.NbTriangles() + 1):
            t = np.array(mesh.Triangle(i).Get()) - 1 + offset
            if face.Orientation() == TopAbs_REVERSED:
                t = t[[0, 2, 1]]
            tris.append(t)
            face_ids.append(fi)
    return np.asarray(points), np.asarray(tris), np.asarray(uvs), np.asarray(face_ids)


def edge_uses(faces):
    indexed = TopTools_IndexedMapOfShape()
    uses = []
    for fi, face in enumerate(faces):
        for edge in shapes(face, TopAbs_EDGE):
            index = indexed.Add(edge) - 1
            if index == len(uses):
                uses.append([])
            uses[index].append(fi)
    return [(TopoDS.Edge_s(indexed.FindKey(i + 1)), use) for i, use in enumerate(uses)]


def edge_samples(edge, count=97):
    curve = BRepAdaptor_Curve(edge)
    t = np.linspace(curve.FirstParameter(), curve.LastParameter(), count)
    return t, np.asarray([xyz(curve.Value(float(x))) for x in t])


def topology(shape):
    faces = shapes(shape, TopAbs_FACE)
    uses = edge_uses(faces)
    # A closed support uses its self seam twice on the same face. Unique
    # owning-face counts alone incorrectly classify that seam as a free edge.
    counts=[sum(2 if BRep_Tool.IsClosed_s(edge,faces[fi]) else 1 for fi in owners)
            for edge,owners in uses if not BRep_Tool.Degenerated_s(edge)]
    return dict(faces=len(faces), solids=len(shapes(shape, TopAbs_SOLID)),
                free_edges=sum(n == 1 for n in counts),
                multiple_edges=sum(n > 2 for n in counts),
                self_seam_edges=sum(BRep_Tool.IsClosed_s(e,faces[fi]) for e,u in uses
                                    if not BRep_Tool.Degenerated_s(e) for fi in u),
                degenerate_edges=sum(BRep_Tool.Degenerated_s(e) for e,u in uses),
                brep_valid=BRepCheck_Analyzer(shape, True).IsValid(),
                face_valid=[BRepCheck_Analyzer(f, True).IsValid() for f in faces])


def surface_info(surface):
    if not hasattr(surface, 'NbUPoles'):
        return dict(type=type(surface).__name__)
    return dict(type=type(surface).__name__, u_poles=surface.NbUPoles(), v_poles=surface.NbVPoles(),
                u_degree=surface.UDegree(), v_degree=surface.VDegree(),
                u_knots=surface.NbUKnots(), v_knots=surface.NbVKnots(),
                rational=surface.IsURational() or surface.IsVRational())
