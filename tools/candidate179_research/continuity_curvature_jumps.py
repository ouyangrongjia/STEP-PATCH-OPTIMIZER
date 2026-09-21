"""Separate raw parametric second derivatives from physical height Hessian jumps."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from spline import from_occt,tensor
from hard_boundary import operators
from continuity_diagnose import angle


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run);data=np.load(out/'reference.npz');report={}
    for name in ['control','aligned_inner_cubic']:
        rows=[]
        for fi,face in enumerate(shapes(read_step(out/name/'patch_faces.step'),TopAbs_FACE)):
            surface=BRep_Tool.Surface_s(face);p,tu,tv,degree=from_occt(surface);local=(p-data['center'])@data['axes'].T;z=local[:,:,2].ravel()
            k,m=np.unique(tu,return_counts=True)
            for i in range(1,len(k)-1):
                if m[i]!=2:continue
                samples=np.unique(np.r_[np.linspace(0,1,1001),.001,.002,.005,.01,.02,.06])
                left=np.c_[np.full(len(samples),np.nextafter(k[i],-np.inf)),samples];right=left.copy();right[:,0]=np.nextafter(k[i],np.inf)
                x,na=evaluate(surface,left);y,nb=evaluate(surface,right);normal=angle(na,nb)
                a=operators(local,tu,tv,degree,left,True);b=operators(local,tu,tv,degree,right,True)
                second=[(a[j]-b[j])@z for j in range(3)];jump=np.sqrt(second[0]**2+2*second[1]**2+second[2]**2);worst=int(jump.argmax())
                raw=(tensor(left,tu,tv,2,0,degree)-tensor(right,tu,tv,2,0,degree))@p.reshape(-1,3)
                rows.append(dict(patch=fi,u_knot=float(k[i]),multiplicity=2,max_normal_jump_degrees=float(normal.max()),
                    maximum_physical_height_hessian_jump_per_mm=float(jump.max()),worst_v=float(samples[worst]),worst_xyz=((x[worst]+y[worst])/2).tolist(),
                    max_raw_parameter_duu_jump_mm=float(np.linalg.norm(raw,axis=1).max())))
        report[name]=rows
    dump(out/'c1_knot_curvature_audit.json',dict(samples_per_knot=1005,records=report,
        meaning='Physical height-Hessian differences include XY map curvature correction. They measure a second-derivative discontinuity at a C1 knot; not a C0 normal crease or a CAD error code.',
        scope='Numerical one-sided limits, no formal G2 claim.'))
    for name,rows in report.items():print(name,len(rows),'C1 knots; max physical Hessian jump',max(r['maximum_physical_height_hessian_jump_per_mm'] for r in rows),flush=True)


if __name__=='__main__':main()
