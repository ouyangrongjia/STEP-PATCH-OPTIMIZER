"""Local worst-region, seam, reverse-distance and support extrapolation audit."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree
from geometry import *
from spline import from_occt, evaluate as ev
from evaluate import closest
from fit_graph import stats
from integrate import edge_checks
from OCP.BRepAdaptor import BRepAdaptor_Curve2d
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
from OCP.TopAbs import TopAbs_IN, TopAbs_ON, TopAbs_FORWARD
from OCP.gp import gp_Pnt2d


def curvature(p,tu,tv,degree,uv):
    du=ev(p,tu,tv,uv,1,0,degree);dv=ev(p,tu,tv,uv,0,1,degree)
    n=np.cross(du,dv);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-30)
    E=np.sum(du*du,axis=1);F=np.sum(du*dv,axis=1);G=np.sum(dv*dv,axis=1)
    e=np.sum(ev(p,tu,tv,uv,2,0,degree)*n,axis=1);f=np.sum(ev(p,tu,tv,uv,1,1,degree)*n,axis=1);g=np.sum(ev(p,tu,tv,uv,0,2,degree)*n,axis=1)
    den=np.maximum(E*G-F*F,1e-30)
    H=(e*G-2*f*F+g*E)/(2*den);K=(e*g-f*f)/den
    return np.sqrt(np.maximum(4*H*H-2*K,0))


def seam_quality(shape):
    faces=shapes(shape,TopAbs_FACE);result=[]
    for ei,(edge,use) in enumerate(edge_uses(faces)):
        if BRep_Tool.Degenerated_s(edge):continue
        self_seam=len(use)==1 and BRep_Tool.IsClosed_s(edge,faces[use[0]])
        if len(use)!=2 and not self_seam:continue
        t,_=edge_samples(edge,385);normals=[];points=[]
        traces=[(use[0],TopoDS.Edge_s(edge.Oriented(o))) for o in (TopAbs_FORWARD,TopAbs_REVERSED)] if self_seam else [(fi,edge) for fi in use]
        for fi,oriented in traces:
            face=faces[fi];pc=BRepAdaptor_Curve2d(oriented,face)
            uv=np.array([[pc.Value(float(x)).X(),pc.Value(float(x)).Y()] for x in t])
            p,n=evaluate(BRep_Tool.Surface_s(face),uv)
            if face.Orientation()==TopAbs_REVERSED:n*=-1
            normals.append(n);points.append(p)
        angle=np.degrees(np.arccos(np.clip(np.sum(normals[0]*normals[1],axis=1),-1,1)))
        result.append(dict(edge=ei,normal_degrees=stats(angle),separation_mm=float(np.max(np.linalg.norm(points[0]-points[1],axis=1)))))
        if self_seam:result[-1]['self_seam']=True
    return result


def sample_shape(shape,count=6000,seed=181):
    p,t,uv,tf=tessellate(shape,.001)
    area=np.linalg.norm(np.cross(p[t[:,1]]-p[t[:,0]],p[t[:,2]]-p[t[:,0]]),axis=1)
    rng=np.random.default_rng(seed);choose=rng.choice(len(t),count,p=area/area.sum())
    r=np.sqrt(rng.random(count));s=rng.random(count);w=np.c_[1-r,r*(1-s),r*s]
    q=np.einsum('ij,ijk->ik',w,uv[t[choose]])
    points=np.zeros((count,3));faces=shapes(shape,TopAbs_FACE)
    for fi,face in enumerate(faces):
        mask=tf[choose]==fi
        points[mask],_=evaluate(BRep_Tool.Surface_s(face),q[mask])
    return points


def reverse_distance(shape,original,data,count=6000,seed=181):
    points=sample_shape(shape,count,seed)
    centers=data['mesh_points'][data['mesh_triangles']].mean(axis=1)
    _,idx=cKDTree(centers).query(points,k=24)
    candidates=data['mesh_triangle_face'][idx]
    result=np.full(len(points),np.inf)
    for fi,face in enumerate(shapes(original,TopAbs_FACE)):
        mask=np.any(candidates==fi,axis=1)
        if not np.any(mask):continue
        p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face))
        distance,_,_,_=closest(points[mask],p,tu,tv,degree,iterations=15)
        result[mask]=np.minimum(result[mask],distance)
    # Cross-check global trimmed original B-Rep distances at worst and fixed points.
    indices=np.unique(np.r_[np.linspace(0,len(points)-1,12).astype(int),np.argsort(result)[-8:]])
    checks=[]
    for i in indices:
        query=BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*points[i])).Vertex(),original)
        if query.IsDone():checks.append(dict(sample=int(i),numeric_mm=float(result[i]),occt_mm=query.Value()))
    return dict(distance_mm=stats(result),sample_count=len(points),seed=seed,
                method='area sampling of actual candidate triangulation, exact candidate surfaces; nearest original rectangular supports from 24 nearby triangle centroids',
                occt_global_checks=checks),points,result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True);args=parser.parse_args()
    out=Path(args.run);data=np.load(out/'reference.npz');original=read_step(out/'original_region.step')
    source_faces=shapes(original,TopAbs_FACE)
    face_ids=json.loads((out/'input_audit.json').read_text(encoding='utf-8'))['selected_face_ids']
    baseline=read_step(Path(args.baseline)/'trimmed_patch_faces.step')
    reference_curvature=np.empty(len(data['test_points']))
    for fi,face in enumerate(source_faces):
        mask=data['test_faces']==face_ids[fi]
        p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face));reference_curvature[mask]=curvature(p,tu,tv,degree,data['test_uv'][mask])
    boundary_distance,_=cKDTree(data['boundary_points']).query(data['test_points'])
    reports={'original_curvature_per_mm':stats(reference_curvature),'original_internal_seams':seam_quality(original)}
    variants=[('baseline',baseline,out/'baseline_evaluation.npz')]
    variants.extend((n,read_step(out/n/'four_patch_diagnostic.step'),out/n/'evaluation.npz') for n in ['g17_data','g17_outer','g25_strict_outer','g17_svd','g17_exterior_fair'])
    for name,shape,file in variants:
        val=np.load(file);faces=shapes(shape,TopAbs_FACE);k=np.empty(len(data['test_points']))
        if name=='baseline':
            for fi,face in enumerate(faces):
                mask=val['patch']==fi;p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face));k[mask]=curvature(p,tu,tv,degree,val['uv'][mask])
        else:
            p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(faces[0]));k=curvature(p,tu,tv,degree,val['uv'])
        bands=[]
        for lo,hi in [(0,.01),(.01,.05),(.05,.2),(.2,100)]:
            mask=(boundary_distance>=lo)&(boundary_distance<hi)
            bands.append(dict(boundary_distance_mm=[lo,hi],samples=int(mask.sum()),distance_mm=stats(val['distance'][mask]),normal_degrees=stats(val['normal_angle'][mask]),curvature_per_mm=stats(k[mask])))
        report=dict(curvature_on_original_area_samples_per_mm=stats(k),bands=bands,seams=seam_quality(shape),edge_checks=edge_checks(shape,193))
        if name!='baseline':
            outside=[]
            for i,(u,v) in enumerate(val['uv']):
                if not any(BRepClass_FaceClassifier(face,gp_Pnt2d(float(u),float(v)),1e-9).State() in (TopAbs_IN,TopAbs_ON) for face in faces):outside.append(i)
            corrected_distance=val['distance'].copy();corrected_normal=val['normal_angle'].copy()
            for i in outside:
                query=BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*data['test_points'][i])).Vertex(),shape)
                if not query.IsDone():raise RuntimeError('Trim-aware projection failed')
                point=xyz(query.PointOnShape2(1));fit=np.load(out/name/'surface.npz')
                uv=(((point-fit['center'])@fit['axes'].T)[:2]-fit['domain'][0])/fit['domain'][1]
                _,normal=evaluate(BRep_Tool.Surface_s(faces[0]),np.array([uv]));corrected_distance[i]=query.Value()
                corrected_normal[i]=np.degrees(np.arccos(np.clip(normal[0]@data['test_normals'][i],-1,1)))
            report['trim_correction']=dict(outside_support_projections=len(outside),distance_mm=stats(corrected_distance),normal_degrees=stats(corrected_normal))
            np.savez_compressed(out/name/'trim_evaluation.npz',distance=corrected_distance,normal_angle=corrected_normal,outside=np.array(outside))
            fit=np.load(out/name/'surface.npz');q=np.stack(np.meshgrid(np.linspace(0,1,151),np.linspace(0,1,151),indexing='ij'),axis=-1).reshape(-1,2)
            world=ev(p,tu,tv,q,degrees=degree);height=((world-data['center'])@data['axes'].T)[:,2]
            report['untrimmed_support_height_range_mm']=[float(height.min()),float(height.max())]
        reverse,points,distance=reverse_distance(shape,original,data);report['reverse']=reverse
        np.savez_compressed(out/(name+'_reverse.npz'),points=points,distance=distance)
        reports[name]=report;print(name,'reverse',reverse['distance_mm'],'normal_p95',float(np.quantile(val['normal_angle'],.95)),flush=True)
        dump(out/'local_diagnostics.json',reports)
    # Exact source-side normal transition at the observed worst candidate normal.
    worst=int(np.argmax(np.load(out/'g17_data/evaluation.npz')['normal_angle']))
    fid=int(data['test_faces'][worst]);face=source_faces[face_ids.index(fid)];source_uv=data['test_uv'][worst]
    fractions=np.r_[0.,np.geomspace(1e-6,.2,100)]
    uv=np.tile(source_uv,(len(fractions),1));axis=int(np.argmin(np.minimum(source_uv,1-source_uv)));side=int(source_uv[axis]>.5)
    uv[:,axis]=1-fractions if side else fractions
    points,normals=evaluate(BRep_Tool.Surface_s(face),uv)
    if face.Orientation()==TopAbs_REVERSED:normals*=-1
    fit=np.load(out/'g17_data/surface.npz');graph_uv=(((points-fit['center'])@fit['axes'].T)[:,:2]-fit['domain'][0])/fit['domain'][1]
    cp,cn=evaluate(BRep_Tool.Surface_s(shapes(variants[1][1],TopAbs_FACE)[0]),graph_uv)
    normal_difference=np.degrees(np.arccos(np.clip(np.sum(cn*normals,axis=1),-1,1)))
    along=np.linalg.norm(points-points[0],axis=1)
    np.savetxt(out/'boundary_profile.csv',np.c_[fractions,along,normal_difference,np.linalg.norm(cp-points,axis=1)],delimiter=',',header='source_uv_inward_fraction,distance_from_edge_mm,normal_error_deg,graph_distance_mm',comments='')
    reports['profile']=dict(face_id=fid,source_uv=source_uv.tolist(),axis=axis,boundary_side=side,maximum_normal_error_deg=float(normal_difference.max()))
    dump(out/'local_diagnostics.json',reports)


if __name__=='__main__':main()
