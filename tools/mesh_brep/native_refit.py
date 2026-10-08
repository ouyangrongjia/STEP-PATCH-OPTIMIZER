"""Refit a native mesh target with collision-checked quad pairing.

This reuses only a declared native subdivision target, never external CAD or
commercial fitted surfaces. Source mesh/labels and all fitting costs remain
linked in the report.
"""
import argparse
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .partition_fit import incidence
from .regional_reconstruct import quad_layout
from .subdivision_reconstruct import Target,network_from_target,fit_target_patch


def safe_layout(v,f,labels,excluded_edges=None,face_limit=None):
    from .safe_regional import sampled_intersections
    excluded=set() if excluded_edges is None else set(excluded_edges);history=[]
    for attempt in range(16):
        layout=quad_layout(v,f,labels,excluded,matching='maximum')
        if face_limit is not None and len(layout)>face_limit:
            raise ValueError(f'Collision-safe layout exceeds face budget: {len(layout)} > {face_limit}')
        patches=[]
        for p in layout:
            a,b,c,*d=v[p['vertices']]
            patches.append(dict(**p,poles=np.array([[a,d[0] if d else c],[b,c]])))
        bad,check=sampled_intersections(patches)
        history.append(dict(attempt=attempt,faces=len(layout),collision=check))
        if not len(bad):return layout,history
        before=len(excluded)
        for pi in bad:
            source=layout[pi]['source']
            if len(source)==2:
                common=set(f[source[0]])&set(f[source[1]])
                excluded.add(tuple(sorted(common)))
        if len(excluded)==before:raise ValueError('Unpaired source triangles fail the collision diagnostic')
    raise ValueError('Collision-safe pairing did not converge')


def refit(source,output,degree=7,curve_degree=5,seam_weight=.05,scalar=False,face_limit=None,normal_weight=0.,tangent_policy='normal_plane',seam_mode='vector'):
    src=Path(source);out=Path(output);out.mkdir(parents=True,exist_ok=False);start=time.perf_counter()
    meta=json.loads((src/'run.json').read_text(encoding='utf-8'))
    if meta.get('source_CAD_read') is not False or meta.get('commercial_engine_used') is not False:
        raise ValueError('Refitting requires an explicitly native mesh-only target')
    report={**meta,'status':'running','source_run':str(src.resolve()),'algorithm':'native-collision-safe-target-refit',
            'curve_degree':curve_degree,'scalar_interior':scalar,'original_fit_seconds':meta.get('wall_seconds'),
            'source_normal_weight':normal_weight,'curve_tangent_policy':tangent_policy,'seam_mode':seam_mode}
    for key in ['watertight','topology','topology_before','assembly_seconds','wall_seconds']:report.pop(key,None)
    report['config']={**meta['config'],'degree':degree,'seam_weight':seam_weight}
    dump(out/'run.json',report)
    try:
        for name in ['simplified_mesh.npz','subdivision_target.npz']:shutil.copy2(src/name,out/name)
        mesh=np.load(src/'simplified_mesh.npz');a=np.load(src/'subdivision_target.npz')
        v=mesh['vertices'];f=mesh['triangles'];labels=mesh['labels'];sharp={tuple(k) for k in a['sharp']}
        edges,owners=incidence(f);source_network={tuple(k):dict(sharp=tuple(k) in sharp) for k in edges}
        target=Target(a['points'],a['triangles'],a['parents'],a['face_uv'],f,labels,
                      [tuple(o) for k,o in zip(edges,owners) if tuple(k) in sharp])
        layout,report['layout_safety']=safe_layout(v,f,labels,face_limit=face_limit)
        charts=[target.chart(p) for p in layout];positions=a['points'][:len(v)]
        if normal_weight:
            from .mesh import Locator,vertex_normals
            original=np.load(meta['partition']);sv=original['mesh_vertex_original'];sf=original['mesh_triangle'];sr=original['face_instance_id']
            normal=vertex_normals(sv,sf);lookups={}
            for region in np.unique(labels):
                tri=sf[sr==region];used=np.unique(tri);local=np.searchsorted(used,tri)
                lookups[int(region)]=(Locator(sv[used],local),local,normal[used])
            def wrap(chart,region):
                lookup,tri,nodal=lookups[region]
                def query(uv):
                    xyz,_=chart(uv);_,_,ids,bary=lookup.closest(xyz)
                    nn=np.einsum('ij,ijk->ik',bary,nodal[tri[ids]])
                    nn/=np.maximum(np.linalg.norm(nn,axis=1)[:,None],1e-30)
                    return xyz,nn
                return query
            charts=[wrap(chart,p['region']) for chart,p in zip(charts,layout)]
        network,errors=network_from_target(layout,charts,positions,source_network,curve_degree,tangent_policy)
        patches=[];records=[]
        for i,(patch,chart) in enumerate(zip(layout,charts)):
            poles,rec=fit_target_patch(patch,network,chart,positions,degree,seam_weight,scalar,normal_weight,seam_mode)
            patches.append(dict(**patch,poles=poles));records.append(dict(region=patch['region'],**rec))
        np.savez_compressed(out/'surfaces.npz',poles=np.array([p['poles'] for p in patches]),
                            regions=np.array([p['region'] for p in patches]),corners=np.array([len(p['vertices']) for p in patches]))
        dump(out/'fitting.json',dict(layout=layout,patches=records,boundary_fit=errors,
                                    opposed_samples=sum(r['opposed_samples'] for r in records)))
        from .regional_step import assemble
        report.update(assemble(positions,patches,network,out,meta['config']['tolerance']),
                      status='DiagnosticOnly',can_apply=False,boundary_target_distance_mm=errors)
    except Exception as exc:report.update(status='failed',error=str(exc));raise
    finally:report['wall_seconds']=time.perf_counter()-start;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True)
    p.add_argument('--degree',type=int,default=7);p.add_argument('--curve-degree',type=int,default=5)
    p.add_argument('--seam-weight',type=float,default=.05);p.add_argument('--scalar',action='store_true')
    p.add_argument('--face-limit',type=int);a=p.parse_args()
    refit(a.source,a.output,a.degree,a.curve_degree,a.seam_weight,a.scalar,a.face_limit)
