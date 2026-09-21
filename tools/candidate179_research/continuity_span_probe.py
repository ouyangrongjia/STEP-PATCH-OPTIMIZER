"""Same-geometry dense-knot control within one C2 knot block of a failing support."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    target=out/'aligned_inner_cubic/span_probe';target.mkdir(exist_ok=True)
    face=shapes(read_step(out/'aligned_inner_cubic/patch_faces.step'),TopAbs_FACE)[1];surface=BRep_Tool.Surface_s(face)
    cuts=[surface.UKnot(i) for i in range(1,surface.NbUKnots()+1) if i in [1,surface.NbUKnots()] or surface.UMultiplicity(i)>=2]
    pieces=[]
    for a,b in zip(cuts[:-1],cuts[1:]):
        s=surface.Copy();s.Segment(a,b,0.,1.);f=BRepBuilderAPI_MakeFace(s,1e-7).Face()
        if face.Orientation()==TopAbs_REVERSED:f=TopoDS.Face_s(f.Reversed())
        pieces.append(f)
    write_step(target/'all_c2_blocks.step',compound(pieces))
    first=BRep_Tool.Surface_s(pieces[0]);dense=first.Copy();a,b,c,d=first.Bounds()
    for value in np.linspace(a,b,70)[1:-1]:dense.InsertUKnot(float(value),1,1e-12,False)
    f=BRepBuilderAPI_MakeFace(dense,1e-7).Face()
    if face.Orientation()==TopAbs_REVERSED:f=TopoDS.Face_s(f.Reversed())
    write_step(target/'first_block_dense.step',f)
    uv=np.array(np.meshgrid(np.linspace(a,b,201),np.linspace(0,1,91),indexing='ij')).reshape(2,-1).T;p,_=evaluate(first,uv);q,_=evaluate(dense,uv)
    dump(target/'protocol.json',dict(source_patch=1,source_geometry='Final aligned_inner_cubic',u_cuts=cuts,
        blocks=[surface_info(BRep_Tool.Surface_s(f)) for f in pieces],dense_block=surface_info(dense),
        dense_same_geometry_max_mm=float(np.linalg.norm(p-q,axis=1).max()),
        purpose='Separate a long compound support from its existing C2 blocks; dense first block isolates control count from actual geometry. Open-surface probes, not original-boundary replacement candidates.'))


if __name__=='__main__':main()
