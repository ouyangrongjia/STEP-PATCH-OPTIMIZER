"""Fit one shared interface network, then solve region interiors independently.

Regional seams are crease masks in the intermediate subdivision representation:
their positions cannot depend on another region's interior solve. Exported
surface tangent planes are fitted separately from source STL normals.
"""
import argparse
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import factorized
from .mesh import Locator,dump,stats
from .partition_fit import incidence
from .subdivision_fit import refine,limit_stencil,connectivity
from .mesh_detail_target import closest_segments
from .intersections import self_intersections


def membership(f,labels,n):
    wedge=np.unique(np.c_[f.ravel(),np.repeat(labels,3)],axis=0)
    counts=np.bincount(wedge[:,0],minlength=n)
    return wedge,counts


def fit(source,output,control_levels=1,sample_levels=2,iterations=8):
    src=Path(source);out=Path(output);out.mkdir(parents=True,exist_ok=False);start=time.perf_counter()
    meta=json.loads((src/'run.json').read_text(encoding='utf-8'))
    if meta.get('source_CAD_read') is not False:raise ValueError('A native mesh-only input is required')
    report=dict(status='running',algorithm='shared-boundaries-independent-regional-loop-v1',
                source_run=str(src.resolve()),source_CAD_read=False,commercial_engine_used=False,
                partition=meta['partition'],stl=meta['stl'],config=meta['config'],units='mm',
                control_levels=control_levels,sample_levels=sample_levels,iterations=iterations)
    dump(out/'run.json',report)
    try:
        a=np.load(src/'simplified_mesh.npz');coarse=a['triangles'];v=a['vertices'].copy();labels=a['labels']
        data=np.load(meta['partition']);sv=data['mesh_vertex_original'];sf=data['mesh_triangle'];sl=data['face_instance_id']
        edges,owners=incidence(coarse);regional=labels[owners[:,0]]!=labels[owners[:,1]]
        seams={tuple(e) for e in edges[regional]};sharp=set(seams)
        coarse_real_sharp=np.load(src/'subdivision_target.npz')['sharp']
        sharp.update(map(tuple,coarse_real_sharp))
        f=coarse.copy();parent=np.arange(len(f));uv=np.tile(np.array([[0.,0.],[1.,0.],[0.,1.]])[None],(len(f),1,1))
        for _ in range(control_levels):
            old_edges,_,_=connectivity(f)
            step,f,sharp,uv=refine(len(v),f,sharp,uv)
            # Refine control topology without pre-smoothing it. Otherwise a
            # collision introduced here cannot be undone by later crease masks.
            v=np.r_[v,(v[old_edges[:,0]]+v[old_edges[:,1]])/2];parent=np.repeat(parent,4)
        region=labels[parent];controls_wedge,counts=membership(f,region,len(v))
        boundary=np.flatnonzero(counts>1);fixed_junction=np.flatnonzero(counts>2)
        # Build a regular initial subdivision target; additional creases only
        # prevent local collisions and are recorded explicitly.
        safety=[]
        for attempt in range(16):
            stencil,fine,local_parent,local_uv=limit_stencil(v,f,sharp,sample_levels)
            bad=self_intersections(stencil@v,fine)
            safety.append(dict(attempt=attempt,intersecting_faces=len(bad),creases=len(sharp)))
            dump(out/'initialization.json',safety)
            if not len(bad):break
            vertices=np.unique(f[np.unique(local_parent[bad])]);ring=f[np.isin(f,vertices).any(axis=1)]
            previous=len(sharp)
            for tri in ring:sharp.update(tuple(sorted((int(x),int(y)))) for x,y in zip(tri,np.roll(tri,-1)))
            if len(sharp)==previous:raise ValueError('Cannot initialize a regular regional subdivision target')
        else:raise ValueError('Regional target initialization budget exhausted')
        fine_labels=region[local_parent];wedges,fine_counts=membership(fine,fine_labels,stencil.shape[0])
        interface_rows=np.flatnonzero(fine_counts>1);free_boundary=np.setdiff1d(boundary,fixed_junction)
        edge,own=incidence(fine);pairs=np.sort(fine_labels[own],axis=1)
        source_edges,source_owners=incidence(sf);source_pairs=np.sort(sl[source_owners],axis=1)
        chains=[]
        for pair in np.unique(pairs[pairs[:,0]!=pairs[:,1]],axis=0):
            ids=np.unique(edge[np.all(pairs==pair,axis=1)]);ids=ids[fine_counts[ids]==2]
            segment=source_edges[np.all(source_pairs==pair,axis=1)]
            if len(ids) and not len(segment):raise ValueError('Regional seam has no corresponding source interface')
            chains.append((ids,segment))
        blocks=[]
        for r in np.unique(labels):
            free=controls_wedge[(controls_wedge[:,1]==r)&(counts[controls_wedge[:,0]]==1),0]
            rows=wedges[(wedges[:,1]==r)&(fine_counts[wedges[:,0]]==1),0]
            if not len(free):continue
            matrix=stencil[rows][:,free].tocsr();h=(matrix.T@matrix+sparse.eye(len(free))*.002).tocsc()
            tri=sf[sl==r];used=np.unique(tri);reference=Locator(sv[used],np.searchsorted(used,tri))
            blocks.append((int(r),free,rows,matrix,factorized(h),reference))
        bm=stencil[interface_rows][:,free_boundary].tocsr()
        bsolve=factorized((bm.T@bm+sparse.eye(len(free_boundary))*.01).tocsc()) if len(free_boundary) else None
        control=v.copy();history=[]
        for iteration in range(iterations):
            points=stencil@control;desired=points[interface_rows].copy();slot=np.full(len(points),-1,dtype=int);slot[interface_rows]=np.arange(len(interface_rows))
            for ids,segments in chains:
                if len(ids):desired[slot[ids]]=closest_segments(points[ids],sv,segments)
            delta=np.zeros_like(control)
            if bsolve is not None:
                delta[free_boundary]=bsolve(bm.T@(desired-points[interface_rows])-.01*(control[free_boundary]-v[free_boundary]))
            # Each factorization contains only controls belonging to one region.
            distances=[]
            for r,free,rows,matrix,solve,reference in blocks:
                target,distance,_,_=reference.closest(points[rows]);distances.extend(distance)
                delta[free]=solve(matrix.T@(target-points[rows])-.002*(control[free]-v[free]))
            locked=np.zeros(len(v),dtype=bool);trials=[]
            for attempt in range(20):
                candidate=control+.7*delta;bad=self_intersections(stencil@candidate,fine)
                trials.append(dict(attempt=attempt,intersecting_faces=len(bad),locked_controls=int(locked.sum())))
                if not len(bad):control=candidate;break
                touched=np.unique(fine[bad]);locked[np.unique(stencil[touched].indices)]=True;delta[locked]=0.
            else:raise ValueError('Independent regional fit cannot resolve its geometric conflicts')
            record=dict(iteration=iteration,source_distance_mm=stats(distances),collision_trials=trials)
            history.append(record);dump(out/'iterations.json',history)
            print(dict(iteration=iteration,rms=record['source_distance_mm']['rms'],locked=int(locked.sum())),flush=True)
        bary=np.concatenate([1-local_uv.sum(axis=2,keepdims=True),local_uv],axis=2)
        mapped_uv=np.einsum('nij,njk->nik',bary,uv[local_parent])
        np.savez_compressed(out/'subdivision_target.npz',control=control,points=stencil@control,triangles=fine,
                            parents=parent[local_parent],face_uv=mapped_uv,coarse_triangles=coarse,coarse_regions=labels,
                            sharp=coarse_real_sharp,input_control=a['vertices'],source_ids=a['source_ids'])
        shutil.copy2(src/'simplified_mesh.npz',out/'simplified_mesh.npz')
        report.update(status='target_ready',independent_region_solves=len(blocks),interface_controls=len(free_boundary),
                      fixed_junction_controls=len(fixed_junction),control_vertices=len(v),target_triangles=len(fine),
                      iterations_report=history,initialization=safety,
                      maximum_regional_unknowns=max((len(x[1]) for x in blocks),default=0))
    except Exception as exc:report.update(status='failed',error=str(exc));raise
    finally:report['wall_seconds']=time.perf_counter()-start;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True)
    p.add_argument('--control-levels',type=int,default=1);p.add_argument('--sample-levels',type=int,default=2)
    p.add_argument('--iterations',type=int,default=8);a=p.parse_args();fit(a.source,a.output,a.control_levels,a.sample_levels,a.iterations)
