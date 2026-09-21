"""Frozen post-selection source-face grids at the old worst point and both corners."""
import argparse
import json
from pathlib import Path
import numpy as np
from geometry import *
from designer_layout import original_loop
from hard_boundary import parameterize
from hard_boundary_audit import evaluate_shape
from OCP.BRepTools import BRepTools


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--candidate',default='aligned_inner_cubic')
    parser.add_argument('--confirmed-worst',action='store_true');args=parser.parse_args();label='confirmed_worst_local' if args.confirmed_worst else 'local'
    out=Path(args.run);data=np.load(out/'reference.npz');original=read_step(out/'original_region.step');faces=shapes(original,TopAbs_FACE)
    ids=json.loads((out/'input_audit.json').read_text(encoding='utf-8'))['selected_face_ids'];_,_,owners=original_loop(original)
    baseline=np.load(out/'control/evaluation.npz');worst=int(np.argmax(baseline['normal_angle']));indices={ids.index(int(data['test_faces'][worst]))}
    # The two turns were identified from original_corner_audit.json, not from
    # error values of the selected candidate.
    corners=json.loads((out/'original_corner_audit.json').read_text(encoding='utf-8'))['corners']
    for row in corners:
        if row['regional_edge_after'] not in [0,6,10,18] and row['curve_tangent_jump_degrees']>1:
            owner=owners[row['regional_edge_after']];indices.add(next(i for i,f in enumerate(faces) if f.IsSame(owner)))
    if args.confirmed_worst:
        fresh=np.load(out/'confirmation_reference.npz');values=np.load(out/args.candidate/'confirmation/evaluation.npz');worst=int(np.argmax(values['normal_angle']))
        indices={ids.index(int(fresh['test_faces'][worst]))}
    allp=[];alln=[];alluv=[];allids=[];rows=[]
    for index in sorted(indices):
        face=faces[index];a,b,c,d=BRepTools.UVBounds_s(face);u=np.linspace(a,b,161);v=np.linspace(c,d,161)
        uv=np.array(np.meshgrid(u,v,indexing='ij')).reshape(2,-1).T;p,n=evaluate(BRep_Tool.Surface_s(face),uv)
        if face.Orientation()==TopAbs_REVERSED:n*=-1
        allp.extend(p);alln.extend(n);alluv.extend(uv);allids.extend([ids[index]]*len(uv));rows.append(dict(original_face_id=ids[index],grid=[161,161],xyz_bounds=[p.min(axis=0).tolist(),p.max(axis=0).tolist()]))
    fresh=dict(data);fresh.update(test_points=np.array(allp),test_normals=np.array(alln),test_uv=np.array(alluv),test_faces=np.array(allids),train_points=np.empty((0,3)),train_normals=np.empty((0,3)))
    np.savez_compressed(out/(label+'_reference.npz'),**fresh);reports={}
    reason='Independent confirmation worst-normal original face; post-selection localization only.' if args.confirmed_worst else 'Old control worst-normal face plus original inside-owner faces at the two extra boundary corners.'
    dump(out/(label+'_sampling.json'),dict(source_faces=rows,samples=len(allp),selection=reason,used_for_fitting=False))
    for name in ['control',args.candidate]:
        target=out/name/label;target.mkdir(exist_ok=True);shape=read_step(out/name/'patch_faces.step')
        patch,uv=parameterize(fresh,shapes(shape,TopAbs_FACE),target);report=evaluate_shape(shape,fresh,target,patch,uv)
        report['domain_note']='Exact source surface UV grids including boundary, projected to verified actual rectangular candidate Faces.'
        dump(target/'evaluation.json',report);reports[name]=report
        print(name,label,report['distance_mm'],report['normal_degrees'],flush=True)
    dump(out/(label+'_evaluation.json'),reports)


if __name__=='__main__':main()
