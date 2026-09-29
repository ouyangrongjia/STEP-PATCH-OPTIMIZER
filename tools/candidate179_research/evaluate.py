"""Common numerical projection, CAD differential and trim/topology diagnostics."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree
from geometry import *
from spline import from_occt, evaluate as spline_evaluate, tensor
from fit_graph import stats
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf
from OCP.BRepTools import BRepTools


def closest(points, poles, tu, tv, degrees, uv=None, iterations=18):
    limits=np.array([[tu[degrees[0]],tv[degrees[1]]],[tu[-degrees[0]-1],tv[-degrees[1]-1]]])
    closed=(np.max(np.linalg.norm(poles[0]-poles[-1],axis=1))<1e-9,
            np.max(np.linalg.norm(poles[:,0]-poles[:,-1],axis=1))<1e-9)
    def domain(q):
        result=np.clip(q,limits[0],limits[1])
        for axis,periodic in enumerate(closed):
            if periodic:
                outside=(q[:,axis]<limits[0,axis])|(q[:,axis]>limits[1,axis])
                result[outside,axis]=limits[0,axis]+np.mod(q[outside,axis]-limits[0,axis],limits[1,axis]-limits[0,axis])
        return result
    def ev(q,du=0,dv=0):return spline_evaluate(poles,tu,tv,q,du,dv,degrees)
    if uv is None:
        u=np.unique(np.r_[np.linspace(*limits[:,0],161),tu,(tu[:-1]+tu[1:])/2])
        v=np.unique(np.r_[np.linspace(*limits[:,1],161),tv,(tv[:-1]+tv[1:])/2])
        grid=np.stack(np.meshgrid(u,v,indexing='ij'),axis=-1).reshape(-1,2)
        _,index=cKDTree(ev(grid)).query(points)
        uv=grid[index].copy()
    else:uv=uv.copy()
    for iteration in range(iterations):
        previous_uv=uv.copy()
        p=ev(uv);du=ev(uv,1,0);dv=ev(uv,0,1);r=p-points
        aa=np.einsum('ij,ij->i',du,du);bb=np.einsum('ij,ij->i',du,dv);cc=np.einsum('ij,ij->i',dv,dv)
        ru=np.einsum('ij,ij->i',du,r);rv=np.einsum('ij,ij->i',dv,r)
        det=np.maximum(aa*cc-bb*bb,1e-30)
        step=np.c_[(cc*ru-bb*rv)/det,(aa*rv-bb*ru)/det]
        step=np.clip(step,-.08*(limits[1]-limits[0]),.08*(limits[1]-limits[0]))
        old=np.einsum('ij,ij->i',r,r)
        for scale in [1.,.5,.25,.125,.0625]:
            trial=domain(uv-scale*step);delta=ev(trial)-points
            val=np.einsum('ij,ij->i',delta,delta);mask=val<old
            uv[mask]=trial[mask];old[mask]=val[mask];step[mask]=0
        if np.max(np.linalg.norm(uv-previous_uv,axis=1))<1e-10:break
    p=ev(uv);du=ev(uv,1,0);dv=ev(uv,0,1);normals=np.cross(du,dv)
    normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-30)
    return np.linalg.norm(p-points,axis=1),normals,uv,p


def differential(poles,tu,tv,degrees,axes):
    u=np.unique(tu);v=np.unique(tv)
    # Three interior samples per nonzero knot span, plus a uniform grid.
    u=np.unique(np.r_[np.linspace(u[0],u[-1],101),*[u[:-1]+f*np.diff(u) for f in [.15,.5,.85]]])
    v=np.unique(np.r_[np.linspace(v[0],v[-1],101),*[v[:-1]+f*np.diff(v) for f in [.15,.5,.85]]])
    uv=np.stack(np.meshgrid(u,v,indexing='ij'),axis=-1).reshape(-1,2)
    du=spline_evaluate(poles,tu,tv,uv,1,0,degrees);dv=spline_evaluate(poles,tu,tv,uv,0,1,degrees)
    cross=np.cross(du,dv);mag=np.linalg.norm(cross,axis=1)
    signed=cross@axes[2]
    sign=1 if np.median(signed)>0 else -1
    normalized=mag/np.maximum(np.linalg.norm(du,axis=1)*np.linalg.norm(dv,axis=1),1e-30)
    return dict(samples=len(uv),minimum_jacobian_mm2=float(mag.min()),minimum_normalized_jacobian=float(normalized.min()),
                degeneracy_samples=int(np.sum(mag<1e-10)),projection_reversal_samples=int(np.sum(sign*signed<0)),
                projection_reversal_is_not_itself_a_proof_of_3d_self_intersection=True)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True)
    args=parser.parse_args();out=Path(args.run);data=np.load(out/'reference.npz');points=data['test_points'];truth_normals=data['test_normals']
    baseline=read_step(Path(args.baseline)/'trimmed_patch_faces.step')
    faces=shapes(baseline,TopAbs_FACE);distances=[];normals=[];parameters=[];projections=[];differentials=[];crosschecks=[]
    started=time.perf_counter()
    for i,face in enumerate(faces):
        s=BRep_Tool.Surface_s(face);p,tu,tv,degree=from_occt(s)
        distance,normal,uv,proj=closest(points,p,tu,tv,degree)
        # Use orientation as stored in the STEP face; do not silently abs the error.
        if face.Orientation()==TopAbs_REVERSED:normal*=-1
        distances.append(distance);normals.append(normal);parameters.append(uv);projections.append(proj)
        differentials.append(differential(p,tu,tv,degree,data['axes']))
        # Verify scipy basis values against the kernel independently of closest-point search.
        actual,_=evaluate(s,uv[::113]);approx=spline_evaluate(p,tu,tv,uv[::113],degrees=degree)
        crosschecks.append(float(np.max(np.linalg.norm(actual-approx,axis=1))))
        print('baseline patch',i,'seconds',time.perf_counter()-started,flush=True)
    distances=np.array(distances);best=distances.argmin(axis=0);indices=np.arange(len(points))
    distance=distances[best,indices];normal=np.array(normals)[best,indices]
    angles=np.degrees(np.arccos(np.clip(np.sum(normal*truth_normals,axis=1),-1,1)))
    buv=np.array(parameters)[best,indices];bproj=np.array(projections)[best,indices]
    np.savez_compressed(out/'baseline_evaluation.npz',distance=distance,normal_angle=angles,patch=best,uv=buv,projected=bproj)
    checks=[]
    # Expensive independent kernel searches are bounded and include the worst point.
    check_ids=np.unique(np.r_[np.linspace(0,len(points)-1,24).astype(int),np.argmax(distance),np.argmax(angles)])
    for index in check_ids:
        values=[]
        for face in faces:
            projector=GeomAPI_ProjectPointOnSurf(gp_Pnt(*points[index]),BRep_Tool.Surface_s(face),0.,1.,0.,1.,1e-9)
            if projector.NbPoints():values.append(projector.LowerDistance())
        if values:checks.append(dict(sample=int(index),numeric_mm=float(distance[index]),occt_mm=float(min(values))))
    reports={'baseline':dict(distance_mm=stats(distance),normal_degrees=stats(angles),differential=differentials,
                            basis_occt_max_disagreement_mm=max(crosschecks),occt_projection_checks=checks,
                            seconds=time.perf_counter()-started)}
    for path in sorted(out.glob('g*/surface.npz')):
        started=time.perf_counter();fit=np.load(path)
        initial=(((points-fit['center'])@fit['axes'].T)[:,:2]-fit['domain'][0])/fit['domain'][1]
        distance,normal,uv,proj=closest(points,fit['poles'],fit['tu'],fit['tv'],(3,3),initial,iterations=12)
        angles=np.degrees(np.arccos(np.clip(np.sum(normal*truth_normals,axis=1),-1,1)))
        np.savez_compressed(path.parent/'evaluation.npz',distance=distance,normal_angle=angles,uv=uv,projected=proj)
        # Reference-domain graph correspondence is a valid same-footprint witness;
        # numeric closest support distance is separately labelled and may cross a trim.
        report=dict(support_projection_distance_mm=stats(distance),support_projection_normal_degrees=stats(angles),
                    differential=differential(fit['poles'],fit['tu'],fit['tv'],(3,3),data['axes']),
                    seconds=time.perf_counter()-started)
        reports[path.parent.name]=report
        print(path.parent.name,report,flush=True)
    pp=data['mesh_points'];tt=data['mesh_triangles'];projected=(pp-data['center'])@data['axes'].T
    a=projected[tt[:,1],:2]-projected[tt[:,0],:2];b=projected[tt[:,2],:2]-projected[tt[:,0],:2]
    det=a[:,0]*b[:,1]-a[:,1]*b[:,0]
    reports['original_mesh_projection']=dict(triangles=len(tt),negative=int(np.sum(det < -1e-12)),degenerate=int(np.sum(np.abs(det)<=1e-12)),minimum_signed_double_area_mm2=float(det.min()))
    dump(out/'evaluation.json',reports)
    worst=[]
    for name,file in [('baseline',out/'baseline_evaluation.npz')]+[(p.parent.name,p) for p in out.glob('g*/evaluation.npz')]:
        values=np.load(file)
        for metric in ['distance','normal_angle']:
            for index in np.argsort(values[metric])[-10:][::-1]:
                worst.append(dict(name=name,metric=metric,sample=int(index),face_id=int(data['test_faces'][index]),
                                  source_xyz_mm=points[index].tolist(),source_uv=data['test_uv'][index].tolist(),value=float(values[metric][index])))
    dump(out/'worst_samples.json',worst)
    print('completed',flush=True)


if __name__=='__main__':main()
