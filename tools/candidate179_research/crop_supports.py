"""Exact support-domain restriction: preserve fitted geometry, remove unused extent."""
import argparse
import time
from pathlib import Path
import numpy as np
from geometry import *
from spline import from_occt
from evaluate import differential
from integrate import edge_checks
from diagnose import seam_quality
from OCP.BRepTools import BRepTools
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace
from OCP.TopoDS import TopoDS_Shell
from OCP.TopAbs import TopAbs_WIRE


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--name',default='g17_data');args=parser.parse_args()
    root=Path(args.run);target=root/(args.name+'_cropped');target.mkdir(exist_ok=True)
    ref=np.load(root/'reference.npz');original=read_step(root/args.name/'four_patch_diagnostic.step')
    builder=BRep_Builder();shell=TopoDS_Shell();builder.MakeShell(shell);supports=[];records=[]
    started=time.perf_counter()
    for fi,face in enumerate(shapes(original,TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face);bounds=BRepTools.UVBounds_s(face)
        cropped=surface.Copy();cropped.Segment(*bounds,1e-10,1e-10)
        for edge in shapes(face,TopAbs_EDGE):
            first,last=BRep_Tool.Range_s(edge,face)
            pc=BRep_Tool.CurveOnSurface_s(edge,face,first,last)
            builder.UpdateEdge(edge,pc,cropped,TopLoc_Location(),1e-7)
        wire=TopoDS.Wire_s(shapes(face,TopAbs_WIRE)[0])
        newface=BRepBuilderAPI_MakeFace(cropped,wire,True).Face()
        builder.Add(shell,newface);supports.append(BRepBuilderAPI_MakeFace(cropped,1e-7).Face())
        uv=np.stack(np.meshgrid(np.linspace(bounds[0],bounds[1],101),np.linspace(bounds[2],bounds[3],101),indexing='ij'),axis=-1).reshape(-1,2)
        old,_=evaluate(surface,uv);new,_=evaluate(cropped,uv);height=((new-ref['center'])@ref['axes'].T)[:,2]
        p,tu,tv,degree=from_occt(cropped)
        records.append(dict(patch=fi,bounds=list(bounds),complexity=surface_info(cropped),
                            maximum_geometry_change_mm=float(np.max(np.linalg.norm(new-old,axis=1))),
                            support_height_range_mm=[float(height.min()),float(height.max())],
                            differential=differential(p,tu,tv,degree,ref['axes'])))
    write_step(target/'four_patch_diagnostic.step',shell);write_step(target/'support.step',compound(supports))
    reread=read_step(target/'four_patch_diagnostic.step')
    p,t,uv,f=tessellate(reread);np.savez_compressed(target/'mesh.npz',points=p,triangles=t,uv=uv,face=f)
    result=dict(status='FreeReconstructionDiagnostic',can_apply=False,operation='Exact knot-insertion restriction to each Face UV box; no refit',
                source=args.name,patches=records,after_read=topology(reread),edge_checks=edge_checks(reread,193),seams=seam_quality(reread),
                actual_support_poles=sum(r['complexity']['u_poles']*r['complexity']['v_poles'] for r in records),seconds=time.perf_counter()-started)
    dump(target/'crop.json',result)
    print({k:v for k,v in result.items() if k not in ['patches','edge_checks','seams','after_read']})
    print([{k:v for k,v in r.items() if k!='differential'} for r in records])


if __name__=='__main__':main()
