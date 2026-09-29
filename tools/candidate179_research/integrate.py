"""Fixed-tolerance whole-part diagnostic, without changing acceptance gates."""
import argparse
import time
from pathlib import Path
import numpy as np
from geometry import *
from OCP.TopAbs import TopAbs_SHELL, TopAbs_FORWARD
from OCP.BRepBuilderAPI import BRepBuilderAPI_Sewing, BRepBuilderAPI_MakeSolid
from OCP.BRepLib import BRepLib
from OCP.BRepAdaptor import BRepAdaptor_Curve2d


def edge_checks(shape,count=97):
    records=[];closed=[]
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face)
        for ei,edge in enumerate(shapes(face,TopAbs_EDGE)):
            if BRep_Tool.Degenerated_s(edge):continue
            curve=BRepAdaptor_Curve(edge)
            first,last=curve.FirstParameter(),curve.LastParameter()
            orientations=(TopAbs_FORWARD,TopAbs_REVERSED) if BRep_Tool.IsClosed_s(edge,face) else (edge.Orientation(),)
            traces=[]
            for orientation in orientations:
                pc=BRepAdaptor_Curve2d(TopoDS.Edge_s(edge.Oriented(orientation)),face)
                deviations=[];uv_values=[]
                for t in np.linspace(first,last,count):
                    uv=pc.Value(float(t))
                    uv_values.append([uv.X(),uv.Y()])
                    deviations.append(curve.Value(float(t)).Distance(surface.Value(uv.X(),uv.Y())))
                traces.append(np.asarray(uv_values))
                records.append(dict(face=fi,edge=ei,same_parameter=bool(BRep_Tool.SameParameter_s(edge)),
                                    same_range=bool(BRep_Tool.SameRange_s(edge)),max_curve_on_surface_mm=max(deviations),
                                    tolerance_mm=BRep_Tool.Tolerance_s(edge)))
            if len(traces)==2:
                closed.append(dict(face=fi,edge=ei,pcurves_distinct=bool(np.max(abs(traces[0]-traces[1]))>1e-8),
                                   oriented_curve_on_surface_mm=[r['max_curve_on_surface_mm'] for r in records[-2:]]))
    return dict(checks=len(records),samples_per_use=count,max_curve_on_surface_mm=max(r['max_curve_on_surface_mm'] for r in records),
                failed_001mm=sum(r['max_curve_on_surface_mm']>.001 for r in records),
                same_parameter_false=sum(not r['same_parameter'] for r in records),
                maximum_edge_tolerance_mm=max(r['tolerance_mm'] for r in records),records=records,closed_self_seams=closed)


def integrate(patch, remaining):
    """Existing fixed-tolerance diagnostic sewing, shared with the profiler."""
    start=time.perf_counter();sew=BRepBuilderAPI_Sewing(.001,True,True,True,False)
    sew.SetMaxTolerance(.001);sew.SetLocalTolerancesMode(False)
    for face in remaining+shapes(patch,TopAbs_FACE):sew.Add(face)
    sew.Perform();shape=sew.SewedShape()
    if sew.NbFreeEdges()==0:
        solids=[]
        for item in shapes(shape,TopAbs_SHELL):
            solid=BRepBuilderAPI_MakeSolid(TopoDS.Shell_s(item)).Solid()
            BRepLib.OrientClosedSolid_s(solid);solids.append(solid)
        if solids:shape=compound(solids)
    return shape,dict(seconds=time.perf_counter()-start,sewing_tolerance_mm=.001,
                      local_tolerance_accumulation=False,free_edges=sew.NbFreeEdges(),multiple_edges=sew.NbMultipleEdges())


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',required=True);parser.add_argument('--run',required=True)
    parser.add_argument('--names',nargs='+',default=['g17_data','g25_strict_outer'])
    parser.add_argument('--patch-file',default='four_patch_diagnostic.step');args=parser.parse_args()
    out=Path(args.run);audit=__import__('json').loads((out/'input_audit.json').read_text(encoding='utf-8'));selected=set(audit['selected_face_ids'])
    original=read_step(args.source);faces=shapes(original,TopAbs_FACE);remaining=[f for i,f in enumerate(faces) if i not in selected]
    original_stats=topology(original)
    for name in args.names:
        start=time.perf_counter();target=out/name
        patch=read_step(target/args.patch_file)
        before=edge_checks(patch,193)
        shape,sewing=integrate(patch,remaining)
        write_step(target/'whole_model_diagnostic.step',shape)
        reread=read_step(target/'whole_model_diagnostic.step')
        result=dict(status='DiagnosticOnly',can_apply=False,sewing_tolerance_mm=.001,sewing_attempts=1,
                    local_tolerance_accumulation=False,original=original_stats,
                    sewing_free_edges=sewing['free_edges'],sewing_multiple_edges=sewing['multiple_edges'],
                    before_write=topology(shape),after_step_read=topology(reread),
                    patch_edge_checks=before,seconds=time.perf_counter()-start)
        dump(target/'integration.json',result)
        print(name,{k:v for k,v in result['after_step_read'].items() if k!='face_valid'},result['seconds'],flush=True)


if __name__=='__main__':main()
