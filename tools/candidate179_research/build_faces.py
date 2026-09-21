"""Rebuild four projected trim loops; explicitly a moved-boundary diagnostic."""
import argparse
import time
from pathlib import Path
import numpy as np
from geometry import *
from spline import to_occt, array1
from OCP.Geom import Geom_TrimmedCurve
from OCP.GeomConvert import GeomConvert
from OCP.Geom2d import Geom2d_BSplineCurve
from OCP.TColgp import TColgp_Array1OfPnt2d
from OCP.gp import gp_Pnt2d
from OCP.TopAbs import TopAbs_FORWARD, TopAbs_WIRE, TopAbs_VERTEX
from OCP.BRepTools import BRepTools_WireExplorer
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeVertex
from OCP.BRepLib import BRepLib
from OCP.TopoDS import TopoDS_Shell


def project_curve(edge, center, axes, domain):
    first,last=BRep_Tool.Range_s(edge)
    curve=BRep_Tool.Curve_s(edge,first,last)
    spline=GeomConvert.CurveToBSplineCurve_s(Geom_TrimmedCurve(curve,first,last))
    poles=TColgp_Array1OfPnt2d(1,spline.NbPoles())
    for i in range(1,spline.NbPoles()+1):
        local=(xyz(spline.Pole(i))-center)@axes.T
        uv=(local[:2]-domain[0])/domain[1]
        poles.SetValue(i,gp_Pnt2d(*uv))
    knots=array1([spline.Knot(i) for i in range(1,spline.NbKnots()+1)])
    mult=array1([spline.Multiplicity(i) for i in range(1,spline.NbKnots()+1)],True)
    if spline.IsRational():
        weights=array1([spline.Weight(i) for i in range(1,spline.NbPoles()+1)])
        result=Geom2d_BSplineCurve(poles,weights,knots,mult,spline.Degree(),False)
    else:
        result=Geom2d_BSplineCurve(poles,knots,mult,spline.Degree(),False)
    return result,first,last


def build(baseline,surface,center,axes,domain):
    vertices=shapes(baseline,TopAbs_VERTEX)
    vi=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(baseline,TopAbs_VERTEX,vi)
    ei=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(baseline,TopAbs_EDGE,ei)
    new_vertices=[]
    for v in vertices:
        point=xyz(BRep_Tool.Pnt_s(TopoDS.Vertex_s(v)))
        uv=(((point-center)@axes.T)[:2]-domain[0])/domain[1]
        new_vertices.append(BRepBuilderAPI_MakeVertex(surface.Value(*uv)).Vertex())
    edges=[];errors=[]
    uses=edge_uses(shapes(baseline,TopAbs_FACE))
    for i in range(1,ei.Extent()+1):
        edge=TopoDS.Edge_s(ei.FindKey(i).Oriented(TopAbs_FORWARD))
        pcurve,first,last=project_curve(edge,center,axes,domain)
        v1=vi.FindIndex(TopExp.FirstVertex_s(edge,False))-1
        v2=vi.FindIndex(TopExp.LastVertex_s(edge,False))-1
        maker=BRepBuilderAPI_MakeEdge(pcurve,surface,new_vertices[v1],new_vertices[v2],first,last)
        if not maker.IsDone():
            raise RuntimeError(f'Edge {i} failed: {maker.Error()}')
        new_edge=maker.Edge()
        if not BRepLib.BuildCurves3d_s(new_edge,1e-7):
            raise RuntimeError(f'Edge {i} 3D approximation failed')
        ts,old=edge_samples(edge,385)
        lifted=np.array([xyz(surface.Value(pcurve.Value(float(t)).X(),pcurve.Value(float(t)).Y())) for t in ts])
        moved=np.linalg.norm(lifted-old,axis=1)
        _,actual=edge_samples(new_edge,385)
        cos=np.linalg.norm(lifted-actual,axis=1)
        errors.append(dict(edge_index=i-1,internal=len(uses[i-1][1])==2,
                           source_curve_movement_mm=float(moved.max()),curve_on_surface_mm=float(cos.max()),
                           same_parameter=bool(BRep_Tool.SameParameter_s(new_edge)),same_range=bool(BRep_Tool.SameRange_s(new_edge)),
                           edge_tolerance_mm=BRep_Tool.Tolerance_s(new_edge)))
        edges.append(new_edge)
    faces=[]
    for face in shapes(baseline,TopAbs_FACE):
        oldwire=TopoDS.Wire_s(shapes(face,TopAbs_WIRE)[0])
        explorer=BRepTools_WireExplorer(oldwire,face)
        maker=BRepBuilderAPI_MakeWire()
        loop=[]
        while explorer.More():
            edge=explorer.Current()
            index=ei.FindIndex(edge)-1
            oriented=TopoDS.Edge_s(edges[index].Oriented(edge.Orientation()))
            maker.Add(oriented)
            ts,ps=edge_samples(oriented,25)
            if edge.Orientation()==TopAbs_REVERSED:ps=ps[::-1]
            loop.extend(ps)
            explorer.Next()
        if not maker.IsDone():raise RuntimeError('Wire could not close')
        q=(np.array(loop)-center)@axes.T
        signed=np.sum(q[:,0]*np.roll(q[:,1],-1)-q[:,1]*np.roll(q[:,0],-1))
        wire=maker.Wire()
        if signed<0:wire=TopoDS.Wire_s(wire.Reversed())
        makerface=BRepBuilderAPI_MakeFace(surface,wire,True)
        if not makerface.IsDone():raise RuntimeError('Face construction failed')
        faces.append(makerface.Face())
    # STEP exports a compound of loose faces independently, losing shared edges.
    # A shell records the intended quilt adjacency without widening tolerances.
    shell=TopoDS_Shell();builder=BRep_Builder();builder.MakeShell(shell)
    for face in faces:builder.Add(shell,face)
    return shell,errors


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True)
    args=parser.parse_args();out=Path(args.run)
    baseline=read_step(Path(args.baseline)/'trimmed_patch_faces.step')
    for path in sorted(out.glob('g*/surface.npz')):
        started=time.perf_counter();data=np.load(path);surface=to_occt(data['poles'],data['tu'],data['tv'])
        shape,edges=build(baseline,surface,data['center'],data['axes'],data['domain'])
        write_step(path.parent/'four_patch_diagnostic.step',shape)
        p,t,uv,f=tessellate(shape)
        np.savez_compressed(path.parent/'mesh.npz',points=p,triangles=t,uv=uv,face=f)
        reread=read_step(path.parent/'four_patch_diagnostic.step')
        result=dict(status='FreeReconstructionDiagnostic',can_apply=False,outer_boundary_preserved=False,
                    before_write=topology(shape),after_read=topology(reread),edges=edges,
                    in_memory_shared_supports=1,design_support_poles=int(data['poles'].shape[0]*data['poles'].shape[1]),
                    step_surface_records=len(shapes(reread,TopAbs_FACE)),
                    step_total_support_poles=sum(BRep_Tool.Surface_s(f).NbUPoles()*BRep_Tool.Surface_s(f).NbVPoles() for f in shapes(reread,TopAbs_FACE)),
                    face_support_pole_uses=int(4*data['poles'].shape[0]*data['poles'].shape[1]),
                    seconds=time.perf_counter()-started)
        dump(path.parent/'brep.json',result)
        print(path.parent.name,result['after_read'], 'boundary_max',max(e['source_curve_movement_mm'] for e in edges if not e['internal']),flush=True)


if __name__=='__main__':main()
