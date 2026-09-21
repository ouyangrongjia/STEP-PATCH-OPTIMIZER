"""Recheck the archived control and localize knots, physical spans and normal error."""
import argparse
import json
from pathlib import Path
import numpy as np
from geometry import *
from spline import from_occt, tensor
from hard_boundary import parameterize
from hard_boundary_audit import evaluate_shape
from designer_audit import knot_jumps
from diagnose import reverse_distance, seam_quality
from integrate import edge_checks
from fit_graph import stats
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def angle(a,b):
    return np.degrees(np.arccos(np.clip(np.sum(a*b,axis=-1)/np.maximum(np.linalg.norm(a,axis=-1)*np.linalg.norm(b,axis=-1),1e-30),-1,1)))


def diagnose(shape,data,target):
    records=[];knot_records=[]
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face);p,tu,tv,degree=from_occt(surface)
        local=(p-data['center'])@data['axes'].T;flat=local.reshape(-1,3)
        k=np.unique(tu);rows=[]
        for v in [0.,.01,.02,.06,.2,.5,1.]:
            uv=np.c_[k,np.full(len(k),v)];values,_=evaluate(surface,uv)
            chords=np.linalg.norm(np.diff(values,axis=0),axis=1)
            adjacent=np.maximum(chords[1:]/chords[:-1],chords[:-1]/chords[1:])
            i=int(np.argmin(chords));rows.append(dict(v=v,minimum_span_chord_mm=float(chords.min()),median_span_chord_mm=float(np.median(chords)),
                maximum_span_chord_mm=float(chords.max()),maximum_adjacent_span_ratio=float(adjacent.max()),
                smallest_span_uv=[float(k[i]),float(k[i+1])],smallest_span_xyz=values[i:i+2].tolist()))
        mids=(k[1:]+k[:-1])/2;uv=np.array(np.meshgrid(mids,[.005,.02,.06,.2,.5,.9],indexing='ij')).reshape(2,-1).T
        du=tensor(uv,tu,tv,1,0,degree)@flat;dv=tensor(uv,tu,tv,0,1,degree)@flat
        sv=np.linalg.svd(np.stack([du[:,:2],dv[:,:2]],axis=2),compute_uv=False)
        records.append(dict(patch=fi,complexity=surface_info(surface),physical_u_spans=rows,
            normalized_uv_xy_jacobian_condition=stats(sv[:,0]/sv[:,1]),
            condition_note='Depends on normalized UV units; interpreted together with physical span lengths, not as an invariant quality score.'))
        for axis,t in enumerate([tu,tv]):
            knots,mults=np.unique(t,return_counts=True)
            for i in range(1,len(knots)-1):
                if mults[i]<degree[axis]:continue
                value=knots[i];epsilon=min(value-knots[i-1],knots[i+1]-value)*1e-7
                varying=np.unique(np.r_[np.linspace(0,1,1001),0.,.001,.005,.01,.02,.06,.1,.5,1.])
                left=np.zeros((len(varying),2));left[:,axis]=value-epsilon;left[:,1-axis]=varying
                right=left.copy();right[:,axis]=value+epsilon
                a,na=evaluate(surface,left);b,nb=evaluate(surface,right)
                bu=tensor(left,tu,tv,1 if axis==0 else 0,1 if axis==1 else 0,degree)@p.reshape(-1,3)
                bv=tensor(right,tu,tv,1 if axis==0 else 0,1 if axis==1 else 0,degree)@p.reshape(-1,3)
                normals=angle(na,nb);tangents=angle(bu,bv);maxi=int(np.argmax(normals))
                knot_records.append(dict(patch=fi,axis=axis,knot=float(value),multiplicity=int(mults[i]),
                    max_normal_jump_degrees=float(normals[maxi]),worst_transverse_parameter=float(varying[maxi]),worst_xyz=((a[maxi]+b[maxi])/2).tolist(),
                    parameter_offset_epsilon=float(epsilon),sample_offset_position_difference_mm=float(np.linalg.norm(a-b,axis=1).max()),
                    boundary_tangent_jump_degrees=[float(tangents[0]),float(tangents[-1])],
                    boundary_normal_jump_degrees=[float(normals[0]),float(normals[-1])]))
                np.savez_compressed(target/f'knot_{fi}_{axis}_{i}.npz',varying=varying,xyz=(a+b)/2,normal_jump=normals,tangent_jump=tangents)
        write_step(target/f'face_{fi}.step',face)
    return dict(patches=records,knot_profiles=knot_records)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args()
    out=Path(args.run);data=np.load(out/'reference.npz');target=out/'control'
    shape=read_step(target/'patch_faces.step');faces=shapes(shape,TopAbs_FACE)
    if not (target/'parameterization.npz').exists():parameterize(data,faces,target)
    params=np.load(target/'parameterization.npz');n=len(data['train_points'])
    report=evaluate_shape(shape,data,target,params['patch'][n:],params['uv'][n:])
    report['domain_note']='Archived five_knot_smooth Faces; rectangular iso-boundaries are checked separately in geometry_validation.json.'
    report['reverse'],points,distance=reverse_distance(shape,read_step(out/'original_region.step'),data)
    np.savez_compressed(target/'reverse.npz',points=points,distance=distance)
    report.update(knot_jumps=knot_jumps(shape),seams=seam_quality(shape),edge_checks=edge_checks(shape,385),topology=topology(shape))
    profiles=diagnose(shape,data,target);values=np.load(target/'evaluation.npz');worst=int(np.argmax(values['normal_angle']))
    profiles['worst_normal']=dict(sample=worst,xyz=data['test_points'][worst].tolist(),patch=int(values['patch'][worst]),
        uv=values['uv'][worst].tolist(),normal_degrees=float(values['normal_angle'][worst]),distance_mm=float(values['distance'][worst]))
    dump(target/'evaluation.json',report);dump(target/'defects.json',profiles)
    print('control',report['distance_mm'],report['normal_degrees'],flush=True)
    print('worst',profiles['worst_normal'],'knots',profiles['knot_profiles'],flush=True)


if __name__=='__main__':main()
