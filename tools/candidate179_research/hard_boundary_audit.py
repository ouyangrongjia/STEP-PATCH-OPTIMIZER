"""Common-sample shape and original-boundary audit for locked patch interiors."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from geometry import *
from spline import from_occt
from evaluate import closest
from diagnose import curvature, reverse_distance
from boundary_audit import normals_on_edge
from fit_graph import stats
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf


def kernel_projection(point,faces):
    options=[]
    for fi,face in enumerate(faces):
        query=GeomAPI_ProjectPointOnSurf(gp_Pnt(*point),BRep_Tool.Surface_s(face),0.,1.,0.,1.,1e-10)
        if query.NbPoints():options.append((query.LowerDistance(),fi,query.LowerDistanceParameters()))
    return min(options,key=lambda x:x[0]) if options else None


def evaluate_shape(shape,data,out,initial_patch,initial_uv):
    points=data['test_points'];truth=data['test_normals'];faces=shapes(shape,TopAbs_FACE)
    distances=[];normals=[];parameters=[]
    for fi,face in enumerate(faces):
        p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face))
        distance,normal,uv,_=closest(points,p,tu,tv,degree,iterations=40)
        mask=initial_patch==fi
        if np.any(mask):
            d,n,q,_=closest(points[mask],p,tu,tv,degree,initial_uv[mask],iterations=40)
            better=d<distance[mask];ids=np.flatnonzero(mask)[better]
            distance[ids]=d[better];normal[ids]=n[better];uv[ids]=q[better]
        if face.Orientation()==TopAbs_REVERSED:normal*=-1
        distances.append(distance);normals.append(normal);parameters.append(uv)
        print(out.name,'projection',fi,flush=True)
    patch=np.argmin(distances,axis=0);i=np.arange(len(points))
    distance=np.asarray(distances)[patch,i];normal=np.asarray(normals)[patch,i];uv=np.asarray(parameters)[patch,i]
    angle=np.degrees(np.arccos(np.clip(np.sum(normal*truth,axis=1),-1,1)))
    check_ids=np.unique(np.r_[np.linspace(0,len(points)-1,24).astype(int),np.argsort(distance)[-12:],np.argsort(angle)[-12:]])
    checks=[]
    for index in check_ids:
        result=kernel_projection(points[index],faces)
        if result is None:continue
        d,fi,q=result;checks.append(dict(sample=int(index),numeric_mm=float(distance[index]),occt_mm=float(d)))
        if distance[index]-d>1e-6:
            raise RuntimeError('Unresolved closest-point search at validation sample; see '+str(index))
    k=np.empty(len(points))
    for fi,face in enumerate(faces):
        mask=patch==fi;p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face))
        k[mask]=curvature(p,tu,tv,degree,uv[mask])
    np.savez_compressed(out/'evaluation.npz',distance=distance,normal_angle=angle,patch=patch,uv=uv,curvature=k)
    worst=[]
    for label,values in [('distance_mm',distance),('normal_degrees',angle)]:
        for index in np.argsort(values)[-8:][::-1]:
            worst.append(dict(metric=label,sample=int(index),value=float(values[index]),source_face_id=int(data['test_faces'][index]),source_xyz_mm=points[index].tolist()))
    return dict(distance_mm=stats(distance),normal_degrees=stats(angle),curvature_per_mm=stats(k),
                occt_global_support_checks=checks,worst_samples=worst,
                domain_note='All four baseline trims are the full rectangular isoparametric boundaries, verified before fitting; support projection remains in each face domain.')


def original_boundary(out,baseline,data,variants):
    audit=json.loads((out/'input_audit.json').read_text(encoding='utf-8'));selected=set(audit['selected_face_ids'])
    model=read_step(audit['source']);faces=shapes(model,TopAbs_FACE)
    network=json.loads((baseline/'curve_network.json').read_text(encoding='utf-8-sig'))
    requested={int(e['original_edge_id']):e['requested_continuity'] for e in network['edges'] if e['original_boundary']}
    mapping={}
    for e in network['edges']:
        if e['original_boundary']:mapping.setdefault(int(e['original_edge_id']),set()).add(e['first_patch'])
    basefaces=shapes(variants['baseline'],TopAbs_FACE)
    ei=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(model,TopAbs_EDGE,ei)
    rows=[]
    for edge,uses in edge_uses(faces):
        inside=[i for i in uses if i in selected];outside=[i for i in uses if i not in selected]
        if len(inside)!=1 or len(outside)!=1:continue
        edge_id=ei.FindIndex(edge)-1;t,points=edge_samples(edge,385)
        _,truth=normals_on_edge(edge,faces[inside[0]],t);_,adjacent=normals_on_edge(edge,faces[outside[0]],t)
        allowed=sorted(mapping[edge_id]);ds=[];qs=[]
        for fi in allowed:
            p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(basefaces[fi]))
            d,_,q,_=closest(points,p,tu,tv,degree,iterations=40);ds.append(d);qs.append(q)
        choose=np.argmin(ds,axis=0);indices=np.arange(len(points));distance=np.asarray(ds)[choose,indices]
        uv=np.asarray(qs)[choose,indices];patch=np.asarray(allowed)[choose]
        for index in np.flatnonzero(distance>1e-7):
            result=kernel_projection(points[index],[basefaces[i] for i in allowed])
            if result is not None:
                d,fi,q=result;distance[index]=d;patch[index]=allowed[fi];uv[index]=q
        if distance.max()>1e-6:raise RuntimeError(f'Original edge {edge_id} not matched to frozen baseline')
        row=dict(original_edge_id=edge_id,requested_continuity=requested[edge_id],
            original_adjacent_normal_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(truth*adjacent,axis=1),-1,1)))),variants={})
        for name,shape in variants.items():
            targetfaces=shapes(shape,TopAbs_FACE);position=np.zeros_like(points);normal=np.zeros_like(points)
            for fi in allowed:
                mask=patch==fi
                if not np.any(mask):continue
                p,n=evaluate(BRep_Tool.Surface_s(targetfaces[fi]),uv[mask])
                if targetfaces[fi].Orientation()==TopAbs_REVERSED:n*=-1
                position[mask]=p;normal[mask]=n
            row['variants'][name]=dict(original_curve_to_support_mm=stats(np.linalg.norm(position-points,axis=1)),
                adjacent_normal_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(normal*adjacent,axis=1),-1,1)))))
        rows.append(row)
    result=dict(outer_edges=len(rows),samples_per_edge=385,edges=rows)
    dump(out/'boundary_audit.json',result)
    return {name:dict(original_boundary_max_mm=max(e['variants'][name]['original_curve_to_support_mm']['max'] for e in rows),
        requested_g1_max_normal_degrees=max(e['variants'][name]['adjacent_normal_degrees']['max'] for e in rows if e['requested_continuity']=='G1')) for name in variants}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True)
    args=parser.parse_args();out=Path(args.run);baseline=Path(args.baseline);started=time.perf_counter()
    data=np.load(out/'reference.npz');params=np.load(out/'parameterization.npz');n=len(data['train_points'])
    protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'))
    previous=Path(protocol['previous_run']);old=json.loads((previous/'evaluation.json').read_text(encoding='utf-8'))['baseline']
    reports={'baseline':dict(distance_mm=old['distance_mm'],normal_degrees=old['normal_degrees'])}
    variants={'baseline':read_step(baseline/'trimmed_patch_faces.step')};original=read_step(out/'original_region.step')
    for config in protocol['experiments']:
        name=config['name'];target=out/name;shape=read_step(target/'four_patch_diagnostic.step');variants[name]=shape
        report=evaluate_shape(shape,data,target,params['patch'][n:],params['uv'][n:])
        report['reverse'],points,distance=reverse_distance(shape,original,data)
        np.savez_compressed(target/'reverse.npz',points=points,distance=distance)
        reports[name]=report;dump(target/'evaluation.json',report)
        print(name,report['distance_mm'],report['normal_degrees'],flush=True)
    boundary=original_boundary(out,baseline,data,variants)
    for name,record in boundary.items():reports[name].update(record)
    reports['seconds']=time.perf_counter()-started
    dump(out/'evaluation.json',reports);print('Completed',reports['seconds'],flush=True)


if __name__=='__main__':main()
