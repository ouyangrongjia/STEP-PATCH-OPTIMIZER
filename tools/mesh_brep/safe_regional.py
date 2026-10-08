"""Local polynomial patches with one shared boundary and geometric backtracking.

Regions and their adjacency are supplied, never inferred or merged across labels.
All geometry is obtained from STL. A closed simplified mesh is the fallback for
local smoothing; rejected moves affect both incident patches simultaneously.
Sampling guards are diagnostics, not a continuous self-intersection certificate.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time

import numpy as np

from .mesh import Locator, dump, vertex_normals
from .partition_fit import load_partition
from .partition_network import simplify
from .regional_reconstruct import (Config, make_network, initial_support,
                                    fit_support, evaluate)
from .regional_quality import display_mesh
from .intersections import self_intersections


def sampled_intersections(patches, resolution=13,changed=None):
    selected=np.arange(len(patches))
    if changed is not None and len(changed)<len(patches)//5:
        low=np.array([p['poles'].min(axis=(0,1)) for p in patches])-1e-9
        high=np.array([p['poles'].max(axis=(0,1)) for p in patches])+1e-9
        included=np.zeros(len(patches),dtype=bool)
        for i in changed:included|=np.all((low<=high[i])&(high>=low[i]),axis=1)
        selected=np.flatnonzero(included)
    poles=np.array([patches[i]['poles'] for i in selected]);corners=np.array([len(patches[i]['vertices']) for i in selected])
    v,f,_,owners,_=display_mesh(poles,corners,resolution)
    # Join only coincident tessellation samples for collision diagnostics. This
    # does not modify the input, support geometry, CAD vertices, or CAD tolerance.
    _,ids,remap=np.unique(np.round(v,9),axis=0,return_index=True,return_inverse=True)
    v=v[ids];f=remap[f]
    good=np.all(np.diff(np.sort(f,axis=1),axis=1)!=0,axis=1);f=f[good];owners=owners[good]
    pairs=self_intersections(v,f,return_pairs=True);raw_count=len(pairs)
    # Duffy sampling makes arbitrarily thin triangles near the collapsed
    # side. Use the original affine parent for better-conditioned predicates.
    from .surface_safeguard import elevate_tensor
    from .intersections import confirm_pairs
    affine={}
    for local,i in enumerate(selected):
        p=patches[i]
        if len(p['vertices'])!=3:continue
        cp=p['poles'];a,b,c=cp[0,0],cp[-1,0],cp[0,-1]
        base=np.array([[a,c],[b,c]])
        limit=64*np.finfo(float).eps*max(1.,float(np.abs(cp).max()))
        if np.max(np.abs(cp-elevate_tensor(base,len(cp)-1)))<=limit:
            affine[local]=np.array([a,b,c])
    if len(pairs):
        touched=np.array([owners[x] in affine or owners[y] in affine for x,y in pairs])
        if touched.any():
            test=[]
            for x,y in pairs[touched]:
                test.extend([affine.get(int(owners[x]),v[f[x]]),affine.get(int(owners[y]),v[f[y]])])
            xyz=np.array(test).reshape(-1,3);tri=np.arange(len(xyz)).reshape(-1,3)
            confirmed=confirm_pairs(xyz,tri,np.arange(len(tri)).reshape(-1,2),1e-9)
            for j,(x,y) in enumerate(pairs[touched]):
                if owners[x]==owners[y] and int(owners[x]) in affine:confirmed[j]=False
            keep=np.ones(len(pairs),dtype=bool);keep[touched]=confirmed;pairs=pairs[keep]
    bad=np.unique(pairs)
    analytic_bad=np.array([],dtype=int)
    if affine:
        # Also bypass the sampled broad phase. A real crossing aligned with
        # tessellation edges can otherwise be mistaken for a shared seam.
        ids=np.array(list(affine));xyz=np.array(list(affine.values())).reshape(-1,3)
        tri=np.arange(len(xyz)).reshape(-1,3)
        analytic_bad=ids[self_intersections(xyz,tri,all_candidates=True)]
    rejected=np.union1d(np.unique(owners[bad]),analytic_bad)
    return selected[rejected],dict(triangles=len(f),intersecting_triangles=len(bad),resolution=resolution,
        analytic_intersecting_patches=len(analytic_bad),
        raw_intersection_pairs=raw_count,affine_parent_rejected_pairs=raw_count-len(pairs),
        diagnostic_join_mm=1e-9,checked_patches=len(selected),total_patches=len(patches))


def reconstruct(partition,stl,output,config,fit=True):
    out=Path(output);out.mkdir(parents=True,exist_ok=False);started=time.perf_counter()
    report=dict(status='running',algorithm='shared-PN-regional-backtracking-v1',config=asdict(config),
                stl=str(Path(stl).resolve()),partition=str(Path(partition).resolve()),
                source_CAD_read=False,units='mm',fit_interior=fit)
    dump(out/'run.json',report)
    try:
        sv,sf,sl,topo=load_partition(partition,stl)
        v,f,labels,source_ids,reduction=simplify(sv,sf,sl,config.simplify_error,config.target_triangles)
        np.savez_compressed(out/'simplified_mesh.npz',vertices=v,triangles=f,labels=labels,source_ids=source_ids)
        report.update(input_topology=topo,simplification=reduction)
        sn=vertex_normals(sv,sf);reference=Locator(sv,sf)
        nodal={}
        for label in np.unique(sl):
            selected=sf[sl==label];ids=np.unique(selected)
            normals=vertex_normals(sv[ids],np.searchsorted(ids,selected))
            nodal[int(label)]={i:normals[np.searchsorted(ids,source_ids[i])] for i in np.unique(f[labels==label])}
        layout=[dict(vertices=tri.tolist(),region=int(r),source=[i]) for i,(tri,r) in enumerate(zip(f,labels))]
        network=make_network(v,layout,nodal,config.feature_angle)
        # A single source normal for every smooth vertex eliminates differences
        # caused by the discrete partition boundary's unequal triangle fans.
        for key,edge in network.items():
            if not edge['sharp']:
                normal=sn[source_ids[list(key)]];chord=v[key[1]]-v[key[0]]
                tangent=chord-normal*(normal@chord)[:,None]
                edge['cp']=np.array([v[key[0]],v[key[0]]+tangent[0]/3,v[key[1]]-tangent[1]/3,v[key[1]]])
                edge['normals']={r:normal for r in edge['normals']}
        n=config.degree
        face_normal=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        face_normal/=np.linalg.norm(face_normal,axis=1)[:,None]
        scales=np.ones(len(f));patches=[None]*len(f);records=[None]*len(f);history=[]
        pending=set(range(len(f)))
        g=np.linspace(0,1,21);uv=np.stack(np.meshgrid(g,g[:-1],indexing='ij'),axis=-1).reshape(-1,2)
        for attempt in range(24):
            folded=[]
            for i in sorted(pending):
                patch=layout[i]
                for key in patch['edges']:network[key]['scale']=min(scales[j] for j in network[key]['owners'])
                if fit:
                    cp,record=fit_support(v,patch,network,reference,sn,config)
                else:
                    cp,_=initial_support(v,patch,network,n);record={}
                # Shared boundary damping was applied before fitting. Only the
                # interior correction is reduced by this patch's own scale.
                base,_=initial_support(v,patch,network,n)
                cp=base+scales[i]*(cp-base)
                _,normal,jac=evaluate(cp,uv)
                if np.min(normal@face_normal[i])<.01 or jac.min()<1e-16:folded.append(i)
                patches[i]=dict(**patch,poles=cp);records[i]=record
            bad=np.array(folded,dtype=int);collision=None
            if not len(bad):bad,collision=sampled_intersections(patches)
            history.append(dict(attempt=attempt,refitted=len(pending),folded_patches=len(folded),
                                collision=collision,rejected_patches=bad.tolist()))
            print(json.dumps({**history[-1],'rejected_patches':len(bad)}),flush=True)
            if not len(bad):break
            affected=set(bad)
            for i in bad:
                affected.update(j for key in layout[i]['edges'] for j in network[key]['owners'])
            for i in affected:scales[i]=scales[i]*.25 if scales[i]>.002 else 0.
            pending=set(affected)
            for i in affected:pending.update(j for key in layout[i]['edges'] for j in network[key]['owners'])
        else:raise ValueError('Local geometric backtracking exhausted')
        np.savez_compressed(out/'surfaces.npz',poles=np.array([p['poles'] for p in patches]),regions=labels,corners=np.full(len(f),3))
        dump(out/'fitting.json',dict(layout=layout,patches=records,smoothing_scales=scales.tolist(),history=history))
        report.update(fit_seconds=time.perf_counter()-started,geometric_backtracking=history,
                      fully_smoothed_patches=int(np.sum(scales==1)),locally_flat_patches=int(np.sum(scales==0)))
        from .regional_step import assemble
        report.update(assemble(v,patches,network,out,config.tolerance),status='DiagnosticOnly',can_apply=False)
    except Exception as error:
        report.update(status='failed',error=f'{type(error).__name__}: {error}');raise
    finally:
        report['wall_seconds']=time.perf_counter()-started;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--partition',required=True);p.add_argument('--stl',required=True)
    p.add_argument('--output',required=True);p.add_argument('--simplify-error',type=float,default=.1)
    p.add_argument('--degree',type=int,choices=[3,5],default=3);p.add_argument('--no-fit',action='store_true')
    args=p.parse_args()
    reconstruct(args.partition,args.stl,args.output,Config(simplify_error=args.simplify_error,degree=args.degree,normal_weight=.02,seam_weight=.05),not args.no_fit)
