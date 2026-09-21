"""Distinguish nominal knot multiplicity from actual loss of smoothness in Creo."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    target=out/'aligned_inner_cubic/span_probe';face=shapes(read_step(target/'all_c2_blocks.step'),TopAbs_FACE)[0];source=BRep_Tool.Surface_s(face);modified=source.Copy()
    values=[source.UKnot(i) for i in range(2,source.NbUKnots())]
    for value in values:modified.InsertUKnot(value,2,1e-12,False)
    f=BRepBuilderAPI_MakeFace(modified,1e-7).Face()
    if face.Orientation()==TopAbs_REVERSED:f=TopoDS.Face_s(f.Reversed())
    write_step(target/'redundant_c1_knots.step',f);a,b,c,d=source.Bounds();uv=np.array(np.meshgrid(np.linspace(a,b,201),np.linspace(c,d,91),indexing='ij')).reshape(2,-1).T
    p,_=evaluate(source,uv);q,_=evaluate(modified,uv);trials=[]
    for fi,face in enumerate(shapes(read_step(out/'aligned_inner_cubic/patch_faces.step'),TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face)
        for i in range(2,surface.NbUKnots()):
            if surface.UMultiplicity(i)<2:continue
            copy=surface.Copy();ok=copy.RemoveUKnot(i,1,1e-9)
            trials.append(dict(patch=fi,knot=surface.UKnot(i),old_multiplicity=surface.UMultiplicity(i),target_multiplicity=1,
                removal_tolerance_mm=1e-9,occt_removal_succeeded=bool(ok)))
    dump(target/'multiplicity_protocol.json',dict(source='First imported C2 block',nominal_c1_geometry_max_difference_mm=float(np.linalg.norm(p-q,axis=1).max()),
        source_complexity=surface_info(source),redundant_c1_complexity=surface_info(modified),
        final_candidate_c2_removal_trials=trials,final_candidate_changed=False))


if __name__=='__main__':main()
