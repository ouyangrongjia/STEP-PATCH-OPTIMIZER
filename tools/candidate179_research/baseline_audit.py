"""Reconstruct boundary-only filling styles to identify the archived supports."""
import argparse
import json
from pathlib import Path
import numpy as np
from geometry import *
from OCP.GeomFill import GeomFill_BSplineCurves,GeomFill_CoonsStyle,GeomFill_StretchStyle,GeomFill_CurvedStyle


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--baseline',required=True);parser.add_argument('--run',required=True);args=parser.parse_args()
    folder=Path(args.baseline);historical=json.loads((folder/'validation_summary.json').read_text(encoding='utf-8-sig'))
    shape=read_step(folder/'trimmed_patch_faces.step');records=[]
    q=np.stack(np.meshgrid(np.linspace(0,1,31),np.linspace(0,1,31),indexing='ij'),axis=-1).reshape(-1,2)
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face);truth,_=evaluate(surface,q);record=dict(patch=fi,styles={})
        for name,style in [('Coons',GeomFill_CoonsStyle),('Stretch',GeomFill_StretchStyle),('Curved',GeomFill_CurvedStyle)]:
            try:
                fill=GeomFill_BSplineCurves(surface.VIso(0.),surface.UIso(1.),surface.VIso(1.),surface.UIso(0.),style).Surface()
                a,b,c,d=fill.Bounds();errors=[]
                for swap in [False,True]:
                    for reverse_u in [False,True]:
                        for reverse_v in [False,True]:
                            uv=q[:,::-1].copy() if swap else q.copy()
                            if reverse_u:uv[:,0]=1-uv[:,0]
                            if reverse_v:uv[:,1]=1-uv[:,1]
                            uv=uv*np.array([b-a,d-c])+np.array([a,c]);values,_=evaluate(fill,uv)
                            errors.append(float(np.linalg.norm(values-truth,axis=1).max()))
                record['styles'][name]=dict(max_difference_mm=min(errors),complexity=surface_info(fill))
            except Exception as exc:record['styles'][name]=dict(error=str(exc))
        records.append(record)
    attempts=[dict(stop_reason=a.get('stop_reason'),iterations=len(a.get('iterations',[])),wall_ms=a.get('wall_ms')) for a in historical['fitting']['attempts']]
    result=dict(samples_per_surface=len(q),boundary_only_reconstruction=records,historical_attempts=attempts,
                archived_run_source=dict(commit='90282b948bc073c3b6f7e8804c25526780626807',dirty=True),
                interpretation='Exact numerical match to a boundary-only fill establishes the construction form, not bitwise identity of unavailable dirty source.')
    dump(Path(args.run)/'baseline_construction.json',result);print(result)


if __name__=='__main__':main()
