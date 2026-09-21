"""Geometry-preserving support segmentation to isolate a Creo representation check.

These open-face probes regenerate their trim edges and are NOT replacement parts.
"""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args()
    out=Path(args.run);control=out/'control';records=[]
    for fi in [0,2]:
        source=shapes(read_step(control/f'face_{fi}.step'),TopAbs_FACE)[0];surface=BRep_Tool.Surface_s(source)
        cuts=[surface.UKnot(i) for i in range(1,surface.NbUKnots()+1) if i in [1,surface.NbUKnots()] or surface.UMultiplicity(i)>=surface.UDegree()]
        faces=[];maximum=0.
        for a,b in zip(cuts[:-1],cuts[1:]):
            segment=surface.Copy();segment.Segment(a,b,0.,1.);face=BRepBuilderAPI_MakeFace(segment,1e-7).Face()
            if source.Orientation()==TopAbs_REVERSED:face=TopoDS.Face_s(face.Reversed())
            faces.append(face);uv=np.array(np.meshgrid(np.linspace(a,b,73),np.linspace(0,1,61),indexing='ij')).reshape(2,-1).T
            p,_=evaluate(surface,uv);q,_=evaluate(segment,uv);maximum=max(maximum,float(np.linalg.norm(p-q,axis=1).max()))
        result=compound(faces);write_step(control/f'face_{fi}_split_probe.step',result)
        records.append(dict(patch=fi,segments=len(faces),max_same_parameter_position_difference_mm=maximum,topology=topology(result)))
    dump(out/'split_probe.json',dict(purpose='Isolate internal C0 knot representation for Creo; shape and existing normal crease unchanged.',
        engineering_replacement=False,records=records))


if __name__=='__main__':main()
