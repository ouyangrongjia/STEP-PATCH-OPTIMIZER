"""Exact boundary-preserving interior height refit on the archived four patches."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import splu
from geometry import *
from spline import from_occt, to_occt, tensor, evaluate as ev
from evaluate import closest, differential
from integrate import edge_checks
from diagnose import seam_quality
from fit_graph import stats
from OCP.BRepBuilderAPI import BRepBuilderAPI_Copy, BRepBuilderAPI_MakeFace
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf


def operators(poles, tu, tv, degree, uv, second=False):
    """Derivatives in fixed physical projected X,Y, including map curvature."""
    bu=tensor(uv,tu,tv,1,0,degree);bv=tensor(uv,tu,tv,0,1,degree)
    xy=poles.reshape(-1,3)[:,:2]
    du=bu@xy;dv=bv@xy
    determinant=du[:,0]*dv[:,1]-du[:,1]*dv[:,0]
    if np.any(np.abs(determinant)<1e-12):raise ValueError('Projected map singular at derivative samples')
    ux=dv[:,1]/determinant;vx=-du[:,1]/determinant
    uy=-dv[:,0]/determinant;vy=du[:,0]/determinant
    def scale(a,x):return a.multiply(x[:,None]).tocsr()
    dx=scale(bu,ux)+scale(bv,vx);dy=scale(bu,uy)+scale(bv,vy)
    if not second:return dx,dy,determinant
    hu=[]
    for a,b in [(2,0),(1,1),(0,2)]:
        derivative=tensor(uv,tu,tv,a,b,degree);mapping=derivative@xy
        hu.append(derivative-scale(dx,mapping[:,0])-scale(dy,mapping[:,1]))
    uu,uvv,vv=hu
    xx=scale(uu,ux*ux)+scale(uvv,2*ux*vx)+scale(vv,vx*vx)
    xyop=scale(uu,ux*uy)+scale(uvv,ux*vy+vx*uy)+scale(vv,vx*vy)
    yy=scale(uu,uy*uy)+scale(uvv,2*uy*vy)+scale(vv,vy*vy)
    return xx,xyop,yy,determinant


def quadrature(tu,tv):
    def axis(t):
        k=np.unique(t);mid=(k[1:]+k[:-1])/2;half=np.diff(k)/2
        return np.r_[mid-half/np.sqrt(3),mid+half/np.sqrt(3)],np.r_[half,half]
    u,wu=axis(tu);v,wv=axis(tv)
    return np.stack(np.meshgrid(u,v,indexing='ij'),axis=-1).reshape(-1,2),np.outer(wu,wv).ravel()


def parameterize(data,faces,out):
    points=np.vstack([data['train_points'],data['test_points']]);local=(points-data['center'])@data['axes'].T
    flat=local.copy();flat[:,2]=0
    distances=[];parameters=[];flat_surfaces=[]
    for i,face in enumerate(faces):
        p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face))
        q=(p-data['center'])@data['axes'].T;q[:,:,2]=0
        flat_surfaces.append(to_occt(q,tu,tv,degree))
        d,_,uv,_=closest(flat,q,tu,tv,degree,iterations=30)
        distances.append(d);parameters.append(uv)
        print('Projected coordinates',i,float(np.min(d)),flush=True)
    best=np.argmin(distances,axis=0);idx=np.arange(len(points))
    distance=np.asarray(distances)[best,idx];uv=np.asarray(parameters)[best,idx]
    corrections=[]
    # Verify exceptional Newton inversions against the independent CAD kernel.
    for i in np.flatnonzero(distance>1e-7):
        before=float(distance[i]);options=[]
        for fi,surface in enumerate(flat_surfaces):
            query=GeomAPI_ProjectPointOnSurf(gp_Pnt(*flat[i]),surface,0.,1.,0.,1.,1e-10)
            if query.NbPoints():options.append((query.LowerDistance(),fi,query.LowerDistanceParameters()))
        if options:
            value,fi,q=min(options,key=lambda x:x[0])
            if value<distance[i]:distance[i]=value;best[i]=fi;uv[i]=q
        corrections.append(dict(sample=int(i),initial_residual_mm=before,final_residual_mm=float(distance[i])))
    if np.max(distance)>1e-6:raise RuntimeError('Unresolved planar correspondence; do not fit silently')
    np.savez_compressed(out/'parameterization.npz',patch=best,uv=uv,distance=distance)
    dump(out/'parameterization.json',dict(samples=len(points),projected_residual_mm=stats(distance),
         outside_1e_6mm=int(np.sum(distance>1e-6)),kernel_corrections=corrections,
         method='Invert immutable baseline XY maps; independent kernel fallback for unresolved samples; original heights and normals only'))
    return best,uv


def fit_patch(p,tu,tv,degree,uv,truth,normal,rings,normal_weight,fairness):
    z=p[:,:,2].ravel();n=len(uv)
    interior=np.zeros(p.shape[:2],dtype=bool);interior[rings:-rings,rings:-rings]=True
    free=interior.ravel();b=tensor(uv,tu,tv,degrees=degree)
    matrices=[b/np.sqrt(n)];targets=[(truth[:,2]-b@z)/np.sqrt(n)]
    dx,dy,_=operators(p,tu,tv,degree,uv)
    if np.any(np.abs(normal[:,2])<.1):raise ValueError('Reference cannot be used as a stable height graph')
    for op,k in [(dx,0),(dy,1)]:
        factor=np.sqrt(normal_weight/n)
        matrices.append(op*factor);targets.append((-normal[:,k]/normal[:,2]-op@z)*factor)
    quad,weight=quadrature(tu,tv)
    xx,xy,yy,det=operators(p,tu,tv,degree,quad,True)
    area=weight*np.abs(det);f=np.sqrt(fairness*area/area.sum())
    for op,multiplier in [(xx,1),(xy,np.sqrt(2)),(yy,1)]:
        block=op.multiply((f*multiplier)[:,None]).tocsr()
        matrices.append(block);targets.append(-block@z)
    matrix=sparse.vstack(matrices).tocsc()[:,free];target=np.concatenate(targets)
    # Column scaling conditions the solve; constraints are exact variable elimination.
    scale=1/np.maximum(np.sqrt(np.asarray(matrix.power(2).sum(axis=0)).ravel()),1e-15)
    scaled=matrix@sparse.diags(scale)
    normal_matrix=(scaled.T@scaled).tocsc()+sparse.diags(1e-12*scale*scale,format='csc')
    rhs=scaled.T@target
    x=splu(normal_matrix).solve(rhs);delta=scale*x
    heights=z.copy();heights[free]+=delta
    return heights-z,dict(frozen_rings=rings,free_height_coefficients=int(free.sum()),samples=n,
        matrix_rows=matrix.shape[0],quadrature_samples=len(quad),fairness_weight_mm4=fairness,
        normal_weight_mm2=normal_weight,maximum_height_control_change_mm=float(np.max(np.abs(delta))),
        relative_linear_residual=float(np.linalg.norm(normal_matrix@x-rhs)/max(np.linalg.norm(rhs),1e-30)),
        training_vertical_distance_before_mm=stats(truth[:,2]-b@z),
        training_vertical_distance_after_mm=stats(truth[:,2]-b@heights))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True)
    args=parser.parse_args();out=Path(args.run);data=np.load(out/'reference.npz')
    protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'))
    original=read_step(Path(args.baseline)/'trimmed_patch_faces.step');faces=shapes(original,TopAbs_FACE)
    if (out/'parameterization.npz').exists():
        params=np.load(out/'parameterization.npz');patch,uv=params['patch'],params['uv']
    else:patch,uv=parameterize(data,faces,out)
    n=len(data['train_points']);truth=(data['train_points']-data['center'])@data['axes'].T
    normal=data['train_normals']@data['axes'].T
    for config in protocol['experiments']:
        start=time.perf_counter();target=out/config['name'];target.mkdir(exist_ok=False)
        copied=BRepBuilderAPI_Copy(original,True,False).Shape();newfaces=shapes(copied,TopAbs_FACE)
        records=[];supports=[]
        for i,(oldface,newface) in enumerate(zip(faces,newfaces)):
            surface=BRep_Tool.Surface_s(newface);p,tu,tv,degree=from_occt(surface)
            local=(p-data['center'])@data['axes'].T;mask=patch[:n]==i
            delta,record=fit_patch(local,tu,tv,degree,uv[:n][mask],truth[mask],normal[mask],
                config['frozen_boundary_control_rings'],protocol['normal_weight_mm2'],protocol['fairness_weight_mm4'])
            newp=p+delta.reshape(p.shape[:2])[:,:,None]*data['axes'][2]
            for a in range(p.shape[0]):
                for b in range(p.shape[1]):
                    if delta.reshape(p.shape[:2])[a,b]!=0:surface.SetPole(a+1,b+1,gp_Pnt(*newp[a,b]))
            # Entire rectangular boundary, independently from B-Rep edge flags.
            t=np.linspace(0,1,1001);boundary=np.r_[np.c_[t,t*0],np.c_[t,t*0+1],np.c_[t*0,t],np.c_[t*0+1,t]]
            oldvalue,oldnormal=evaluate(BRep_Tool.Surface_s(oldface),boundary);value,newnormal=evaluate(surface,boundary)
            record.update(patch=i,complexity=surface_info(surface),
                whole_boundary_change_mm=float(np.max(np.linalg.norm(value-oldvalue,axis=1))),
                whole_boundary_normal_change_degrees=float(np.max(np.degrees(np.arccos(np.clip(np.sum(oldnormal*newnormal,axis=1),-1,1))))),
                differential=differential(newp,tu,tv,degree,data['axes']))
            records.append(record);supports.append(BRepBuilderAPI_MakeFace(surface,1e-7).Face())
            np.savez_compressed(target/f'patch_{i}.npz',poles=newp,tu=tu,tv=tv,degrees=degree)
            print(config['name'],i,record['training_vertical_distance_after_mm'],flush=True)
        checks=edge_checks(copied,385)
        if checks['max_curve_on_surface_mm']>.001:raise RuntimeError('Frozen curves no longer lie on surfaces')
        write_step(target/'four_patch_diagnostic.step',copied);write_step(target/'support.step',compound(supports))
        readback=read_step(target/'four_patch_diagnostic.step')
        result=dict(config=config,can_apply=False,status='BoundaryLockedDiagnostic',patches=records,
            actual_control_points=sum(r['complexity']['u_poles']*r['complexity']['v_poles'] for r in records),
            before_write=topology(copied),after_read=topology(readback),edge_checks=edge_checks(readback,385),
            seams=seam_quality(readback),seconds=time.perf_counter()-start)
        dump(target/'fit.json',result)
        print(config['name'],'complete',result['seconds'],flush=True)


if __name__=='__main__':main()
