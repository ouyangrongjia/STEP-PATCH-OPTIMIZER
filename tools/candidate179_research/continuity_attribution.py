"""Measure starts, finished surfaces, shared-seam movements and knot provenance."""
import argparse
import json
from pathlib import Path
import numpy as np
from geometry import *
from spline import from_occt
from designer_layout import uv_on_side


def compare(a,b,axes):
    left=shapes(read_step(a),TopAbs_FACE);right=shapes(read_step(b),TopAbs_FACE);rows=[]
    for i,(f,g) in enumerate(zip(left,right)):
        surface=BRep_Tool.Surface_s(f);other=BRep_Tool.Surface_s(g);_,tu,tv,_=from_occt(surface)
        u=np.unique(np.r_[np.linspace(0,1,101),tu]);v=np.unique(np.r_[np.linspace(0,1,81),tv]);uv=np.array(np.meshgrid(u,v,indexing='ij')).reshape(2,-1).T
        p,n=evaluate(surface,uv);q,m=evaluate(other,uv);change=q-p;local=change@axes.T
        rows.append(dict(patch=i,samples=len(uv),max_xyz_mm=float(np.linalg.norm(change,axis=1).max()),max_projected_xy_mm=float(np.linalg.norm(local[:,:2],axis=1).max()),max_height_mm=float(np.abs(local[:,2]).max())))
    return dict(max_xyz_mm=max(r['max_xyz_mm'] for r in rows),max_projected_xy_mm=max(r['max_projected_xy_mm'] for r in rows),patches=rows)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run);data=np.load(out/'reference.npz');axes=data['axes']
    result=dict(mapping_design=compare(out/'control/patch_faces.step',out/'linear_xy_soft/initial_layout.step',axes),
        initialization_change_same_final_space=compare(out/'linear_xy_soft/initial_layout.step',out/'aligned_refined_control/initial_layout.step',axes),
        final_sensitivity_to_initialization=compare(out/'linear_xy_soft/patch_faces.step',out/'aligned_refined_control/patch_faces.step',axes))
    shapes_by_name={n:shapes(read_step(out/n/'patch_faces.step'),TopAbs_FACE) for n in ['control','aligned_inner_cubic']}
    fit=json.loads((out/'control/fit.json').read_text(encoding='utf-8'));seams=[]
    for sid in range(8):
        fi,side=next((fi,s) for fi,sides in enumerate(fit['sides']) for s in sides if not s['outer'] and s['seam']==sid)
        uv=uv_on_side(side['mapping'],np.linspace(0,1,1001));points=[evaluate(BRep_Tool.Surface_s(shapes_by_name[n][fi]),uv)[0] for n in ['control','aligned_inner_cubic']]
        change=points[1]-points[0];seams.append(dict(seam=sid,max_same_parameter_movement_mm=float(np.linalg.norm(change,axis=1).max()),max_xy_movement_mm=float(np.linalg.norm((change@axes.T)[:,:2],axis=1).max())))
    result['artificial_seam_movement']=seams
    provenance=[]
    for i,(a,b) in enumerate(zip(shapes_by_name['control'],shapes_by_name['aligned_inner_cubic'])):
        _,tu,_,_=from_occt(BRep_Tool.Surface_s(a));_,newtu,_,_=from_occt(BRep_Tool.Surface_s(b));old=np.unique(tu);new=np.unique(newtu)
        removed=[float(t) for t in old if np.min(abs(new-t))>1e-10];provenance.append(dict(patch=i,removed_u_knots=removed))
    result['removed_knots']=provenance
    p=data['mesh_points'];tri=data['mesh_triangles'];xy=((p-data['center'])@axes.T)[:,:2];a=xy[tri[:,1]]-xy[tri[:,0]];b=xy[tri[:,2]]-xy[tri[:,0]];det=a[:,0]*b[:,1]-a[:,1]*b[:,0]
    result['source_mesh_planar_projection']=dict(triangles=len(tri),negative_triangles=int(np.sum(det<0)),zero_triangles=int(np.sum(det==0)),minimum_signed_twice_area_mm2=float(det.min()),note='Original oriented source triangles projected to PCA XY; no mesh UV optimizer used.')
    dump(out/'attribution_audit.json',result)
    print({k:v for k,v in result.items() if k in ['removed_knots','source_mesh_planar_projection']},flush=True)


if __name__=='__main__':main()
