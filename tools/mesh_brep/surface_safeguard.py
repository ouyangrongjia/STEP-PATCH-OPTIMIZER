"""Shared-vertex polynomial homotopy back to a closed source mesh network.

The blend is an exact Bernstein product, so each neighboring patch retains the
same physical boundary even when its smoothing amount changes locally.
"""
import argparse
import json
from pathlib import Path
import time
from functools import lru_cache

import numpy as np

from .mesh import dump
from .regional_reconstruct import elevate,evaluate,tensor
from .safe_regional import sampled_intersections
from .regional_step import assemble


@lru_cache(maxsize=None)
def elevation_matrix(source_degree,target_degree):
    return elevate(np.eye(source_degree+1),target_degree)


def elevate_tensor(cp,n):
    e=elevation_matrix(len(cp)-1,n)
    return np.einsum('ij,jkc,lk->ilc',e,cp,e)


def blend(cp,base,weights):
    n=len(cp)-1;origin=base[0,0].copy()
    difference=cp-origin-elevate_tensor(base-origin,n)
    result=np.zeros((n+2,n+2,3))
    for a in range(2):
        u=(np.arange(n+1)+1)/(n+1) if a else (n+1-np.arange(n+1))/(n+1)
        for b in range(2):
            v=(np.arange(n+1)+1)/(n+1) if b else (n+1-np.arange(n+1))/(n+1)
            result[a:a+n+1,b:b+n+1]+=difference*u[:,None,None]*v[None,:,None]*weights[a,b]
    return result+elevate_tensor(base-origin,n+1)+origin


def safeguard(source,output,max_attempts=30,orientation='target'):
    src=Path(source);out=Path(output);out.mkdir(parents=True,exist_ok=False);started=time.perf_counter()
    meta=json.loads((src/'run.json').read_text(encoding='utf-8'))
    with np.load(src/'surfaces.npz') as archive:data={k:archive[k] for k in archive.files}
    layout=json.loads((src/'fitting.json').read_text(encoding='utf-8'))['layout']
    original=np.load(src/'simplified_mesh.npz');v=original['vertices']
    report={**meta,'status':'running','algorithm':'shared-vertex-polynomial-backtracking-v2','source_run':str(src.resolve()),'orientation_reference':orientation}
    for key in ['watertight','topology','topology_before','wall_seconds','assembly_seconds']:report.pop(key,None)
    dump(out/'run.json',report)
    try:
        bases=[];weights=np.ones(len(v));incident=[set() for _ in v]
        for i,p in enumerate(layout):
            a,b,c,*d=v[p['vertices']]
            bases.append(np.array([[a,d[0] if d else c],[b,c]]))
            for vertex in p['vertices']:incident[vertex].add(i)
        patches=[None]*len(layout);history=[];pending=set(range(len(layout)))
        report['geometric_backtracking']=history
        g=np.unique(np.r_[np.linspace(0,1,19),1e-6,1e-4,.001,.01,.99,.999,.9999,1-1e-6])
        uv=np.stack(np.meshgrid(g,g,indexing='ij'),axis=-1).reshape(-1,2)
        ops={}
        for corners in (3,4):
            q=uv.copy()
            if corners==3:q[:,1]=np.minimum(q[:,1],1-1e-6)
            n=data['poles'].shape[1]
            ops[corners]=(tensor(n,q,1,0),tensor(n,q,0,1))
        base_normals=[];target_normals=[];parameters=[]
        if orientation=='target':
            from .subdivision_reconstruct import Target
            from .partition_fit import incidence
            a=np.load(src/'subdivision_target.npz');sharp={tuple(k) for k in a['sharp']};e,own=incidence(a['coarse_triangles'])
            target=Target(a['points'],a['triangles'],a['parents'],a['face_uv'],a['coarse_triangles'],a['coarse_regions'],[tuple(o) for k,o in zip(e,own) if tuple(k) in sharp])
        for base,p in zip(bases,layout):
            q=uv.copy()
            if len(p['vertices'])==3:q[:,1]=np.minimum(q[:,1],1-1e-5)
            base_normals.append(evaluate(base,q)[1])
            parameters.append(q)
            if orientation=='target':target_normals.append(target.chart(p).face_normals(q))
        for attempt in range(max_attempts):
            bad=[]
            for i in sorted(pending):
                p=layout[i];a,b,c,*d=weights[p['vertices']]
                w=np.array([[a,d[0] if d else c],[b,c]])
                cp=blend(data['poles'][i],bases[i],w)
                q=uv.copy()
                if len(p['vertices'])==3:q[:,1]=np.minimum(q[:,1],1-1e-5)
                du,dv=ops[len(p['vertices'])];local=(cp-cp[0,0]).reshape(-1,3)
                normal=np.cross(du@local,dv@local)
                normal/=np.maximum(np.linalg.norm(normal,axis=1)[:,None],1e-30)
                reference=base_normals[i]
                if orientation=='target':
                    u,z=parameters[i].T
                    weight=(1-u)*(1-z)*w[0,0]+u*(1-z)*w[1,0]+(1-u)*z*w[0,1]+u*z*w[1,1]
                    reference=(1-weight[:,None])*reference+weight[:,None]*target_normals[i]
                    reference/=np.maximum(np.linalg.norm(reference,axis=1)[:,None],1e-30)
                if np.min(np.sum(normal*reference,axis=1))<.01:bad.append(i)
                patches[i]={**p,'edges':[tuple(e) for e in p['edges']],'poles':cp}
            collision=None;folded=len(bad)
            if not bad:
                bad,collision=sampled_intersections(patches,changed=pending if history else None)
                if not len(bad) and collision['checked_patches']<len(patches):bad,collision=sampled_intersections(patches)
            bad=np.asarray(bad,dtype=int)
            history.append(dict(attempt=attempt,refitted=len(pending),folded_patches=folded,collision=collision,rejected_patches=bad.tolist()))
            dump(out/'backtracking.json',history)
            np.save(out/'backtracking_weights.npy',weights)
            print(json.dumps({**history[-1],'rejected_patches':len(bad)}),flush=True)
            if not len(bad):break
            vertices=np.unique([x for i in bad for x in layout[i]['vertices']])
            updated=np.where(weights[vertices]>.002,weights[vertices]*.25,0.)
            if np.array_equal(weights[vertices],updated):
                raise ValueError(f'Fallback geometry still intersects in patches {bad.tolist()}')
            weights[vertices]=updated
            pending=set().union(*(incident[i] for i in vertices))
        else:raise ValueError('Unable to remove local folds/intersections within declared retry budget')
        target=np.zeros_like(v);network={}
        sharp={tuple(x) for x in np.load(src/'subdivision_target.npz')['sharp']}
        for i,p in enumerate(patches):
            cp=p['poles'];tri=len(p['vertices'])==3
            ends=[cp[0,0],cp[-1,0],cp[0,-1]] if tri else [cp[0,0],cp[-1,0],cp[-1,-1],cp[0,-1]]
            for vertex,point in zip(p['vertices'],ends):target[vertex]=point
            curves=[cp[:,0],cp[-1,:],cp[0,::-1]] if tri else [cp[:,0],cp[-1,:],cp[::-1,-1],cp[0,::-1]]
            for key,a,curve in zip(p['edges'],p['vertices'],curves):
                curve=curve if a==key[0] else curve[::-1]
                if key not in network:network[key]=dict(cp=curve,line=curve.copy(),scale=1.,sharp=key in sharp,owners=[])
                elif np.max(np.linalg.norm(curve-network[key]['cp'],axis=1))>1e-9:raise ValueError('Shared homotopy boundary mismatch')
                network[key]['owners'].append(i)
        np.savez_compressed(out/'surfaces.npz',poles=np.array([p['poles'] for p in patches]),regions=data['regions'],corners=data['corners'])
        dump(out/'fitting.json',dict(layout=layout,vertex_smoothing_weights=weights.tolist(),history=history))
        report.update(geometric_backtracking=history,fully_smoothed_vertices=int(np.sum(weights==1)),flattened_vertices=int(np.sum(weights==0)))
        report.update(assemble(target,patches,network,out,meta['config']['tolerance']),status='DiagnosticOnly',can_apply=False)
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:
        report.update(wall_seconds=time.perf_counter()-started,scope='Safeguard and STEP stage; source_run contains fitting cost')
        dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True);p.add_argument('--orientation',choices=['base','target'],default='target')
    args=p.parse_args();safeguard(args.source,args.output,orientation=args.orientation)
