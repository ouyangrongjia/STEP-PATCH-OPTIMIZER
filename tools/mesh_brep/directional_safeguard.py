"""Suppress tangential control oscillation before sacrificing STL relief.

Normal and tangential control displacements share vertex weights across the
entire patch network. Both polynomial bases preserve common boundary control
coefficients exactly; triangular patches remain total-degree polynomials.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump,vertex_normals
from .regional_reconstruct import evaluate
from .subdivision_reconstruct import triangle_transform,Target
from .surface_safeguard import elevate_tensor
from .safe_regional import sampled_intersections
from .regional_step import assemble
from .partition_fit import incidence


def displacement_components(cp,corners,normals):
    n=len(cp)-1
    if len(corners)==3:
        keys,transform=triangle_transform(n);weights=np.array(keys)/n
        origin=corners[0];coeff=np.linalg.lstsq(transform,(cp-origin).reshape(-1,3),rcond=1e-13)[0]+origin
        base=weights@corners;direction=weights@normals
    else:
        u,v=np.meshgrid(np.linspace(0,1,n+1),np.linspace(0,1,n+1),indexing='ij')
        u=u.ravel();v=v.ravel();weights=np.c_[(1-u)*(1-v),u*(1-v),u*v,(1-u)*v]
        transform=None;coeff=cp.reshape(-1,3);base=weights@corners;direction=weights@normals
    direction/=np.maximum(np.linalg.norm(direction,axis=1)[:,None],1e-30)
    delta=coeff-base;normal=direction*np.sum(delta*direction,axis=1)[:,None]
    return base,delta-normal,normal,weights,transform


def safeguard(source,output,max_attempts=45):
    src=Path(source);out=Path(output);out.mkdir(parents=True,exist_ok=False);start=time.perf_counter()
    report=json.loads((src/'run.json').read_text(encoding='utf-8'))
    report={**report,'status':'running','algorithm':'shared-control-directional-backtracking-v1','source_run':str(src.resolve())}
    for k in ['watertight','topology','topology_before','wall_seconds']:report.pop(k,None)
    dump(out/'run.json',report)
    try:
        data=np.load(src/'surfaces.npz');mesh=np.load(src/'simplified_mesh.npz');v=mesh['vertices'];f=mesh['triangles']
        source_mesh=np.load(report['partition'])
        normals=vertex_normals(source_mesh['mesh_vertex_original'],source_mesh['mesh_triangle'])[mesh['source_ids']]
        layout=json.loads((src/'fitting.json').read_text(encoding='utf-8'))['layout']
        a=np.load(src/'subdivision_target.npz');sharp={tuple(k) for k in a['sharp']};e,own=incidence(f)
        target=Target(a['points'],a['triangles'],a['parents'],a['face_uv'],f,mesh['labels'],[tuple(o) for k,o in zip(e,own) if tuple(k) in sharp])
        wt=np.ones(len(v));wn=np.ones(len(v));components=[];base_normals=[];target_normals=[];incident=[set() for _ in v]
        g=np.unique(np.r_[np.linspace(0,1,19),1e-6,.0001,.001,.999,.9999,1-1e-6])
        uv=np.stack(np.meshgrid(g,g,indexing='ij'),axis=-1).reshape(-1,2)
        for i,(p,cp) in enumerate(zip(layout,data['poles'])):
            ids=p['vertices'];components.append(displacement_components(cp,v[ids],normals[ids]))
            q=uv.copy()
            if len(ids)==3:q[:,1]=np.minimum(q[:,1],1-1e-5)
            aa,b,c,*d=v[ids];base=np.array([[aa,d[0] if d else c],[b,c]])
            base_normals.append(evaluate(base,q)[1]);target_normals.append(target.chart(p).face_normals(q))
            for j in ids:incident[j].add(i)
        patches=[None]*len(layout);pending=set(range(len(layout)));history=[]
        for attempt in range(max_attempts):
            bad=[]
            for i in sorted(pending):
                p=layout[i];ids=p['vertices'];base,tang,normal,weights,transform=components[i]
                coeff=base+(weights@wt[ids])[:,None]*tang+(weights@wn[ids])[:,None]*normal
                cp=coeff.reshape(data['poles'][i].shape) if transform is None else (transform@coeff).reshape(data['poles'][i].shape)
                q=uv.copy()
                if len(ids)==3:q[:,1]=np.minimum(q[:,1],1-1e-5)
                _,nn,jac=evaluate(cp,q)
                # The target and linear fallback bound the orientation guide.
                blend=float((wt[ids].mean()+wn[ids].mean())/2)
                reference=(1-blend)*base_normals[i]+blend*target_normals[i]
                reference/=np.maximum(np.linalg.norm(reference,axis=1)[:,None],1e-30)
                if np.min(np.sum(nn*reference,axis=1))<.01:bad.append(i)
                patches[i]={**p,'edges':[tuple(e) for e in p['edges']],'poles':cp}
            folded=len(bad);collision=None
            if not bad:
                bad,collision=sampled_intersections(patches,changed=pending if history else None)
                if not len(bad) and collision['checked_patches']<len(patches):bad,collision=sampled_intersections(patches)
            bad=np.asarray(bad,dtype=int);history.append(dict(attempt=attempt,refitted=len(pending),folded_patches=folded,collision=collision,rejected_patches=bad.tolist()))
            dump(out/'backtracking.json',history)
            print(dict(attempt=attempt,folded=folded,rejected=len(bad)),flush=True)
            if not len(bad):break
            ids=np.unique([j for i in bad for j in layout[i]['vertices']]);active=ids[wt[ids]>0]
            changed=False
            if len(active):wt[active]=np.where(wt[active]>.002,wt[active]*.25,0.);changed=True
            # Reduce normal detail only for patches whose whole tangential
            # displacement is already zero, rather than freezing their ring.
            normal_bad=[i for i in bad if np.all(wt[layout[i]['vertices']]==0)]
            if normal_bad:
                reduced=np.unique([j for i in normal_bad for j in layout[i]['vertices']])
                new=np.where(wn[reduced]>.002,wn[reduced]*.25,0.)
                changed=changed or not np.array_equal(new,wn[reduced]);wn[reduced]=new
            if not changed:raise ValueError('Directional fallback still has geometric conflicts')
            pending=set().union(*(incident[j] for j in ids))
        else:raise ValueError('Directional backtracking exceeded its declared budget')
        positions=np.zeros_like(v);network={}
        for i,p in enumerate(patches):
            cp=p['poles'];tri=len(p['vertices'])==3
            ends=[cp[0,0],cp[-1,0],cp[0,-1]] if tri else [cp[0,0],cp[-1,0],cp[-1,-1],cp[0,-1]]
            for j,point in zip(p['vertices'],ends):positions[j]=point
            curves=[cp[:,0],cp[-1,:],cp[0,::-1]] if tri else [cp[:,0],cp[-1,:],cp[::-1,-1],cp[0,::-1]]
            for key,vertex,c in zip(p['edges'],p['vertices'],curves):
                c=c if vertex==key[0] else c[::-1]
                if key not in network:network[key]=dict(cp=c,line=c.copy(),scale=1.,sharp=key in sharp,owners=[])
                elif np.max(abs(c-network[key]['cp']))>1e-9:raise ValueError('Directional damping broke a shared boundary')
                network[key]['owners'].append(i)
        np.savez_compressed(out/'surfaces.npz',poles=np.array([p['poles'] for p in patches]),regions=data['regions'],corners=data['corners'])
        dump(out/'fitting.json',dict(layout=layout,tangent_weights=wt.tolist(),normal_weights=wn.tolist(),history=history))
        report.update(assemble(positions,patches,network,out,report['config']['tolerance']),status='DiagnosticOnly',can_apply=False,
                      geometric_backtracking=history,full_normal_detail_vertices=int(np.sum(wn==1)),vertices=len(v))
    except Exception as exc:report.update(status='failed',error=str(exc));raise
    finally:report['wall_seconds']=time.perf_counter()-start;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True);a=p.parse_args()
    safeguard(a.source,a.output)
