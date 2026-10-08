"""Increase native fitting freedom without increasing exported patch count."""
import argparse
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .subdivision_fit import refine,fit_control_mesh


def fit(source,output,control_levels=1,sample_levels=2,iterations=6):
    src=Path(source);out=Path(output);out.mkdir(parents=True,exist_ok=False);started=time.perf_counter()
    report=json.loads((src/'run.json').read_text(encoding='utf-8'))
    if report.get('source_CAD_read') is not False or report.get('commercial_engine_used') is not False:
        raise ValueError('Only a native mesh target is supported')
    report={k:v for k,v in report.items() if k not in ['topology','topology_before','watertight','assembly_seconds','wall_seconds','continuity']}
    report.update(status='running',algorithm='multiresolution-native-loop-target',source_run=str(src.resolve()),
                  control_levels=control_levels,sample_levels=sample_levels,iterations=iterations)
    dump(out/'run.json',report)
    try:
        mesh=np.load(src/'simplified_mesh.npz');target=np.load(src/'base_target.npz')
        v=target['control'];f=mesh['triangles'].copy();regions=mesh['labels'];sharp={tuple(e) for e in target['sharp']}
        source=np.load(report['partition']);sv=source['mesh_vertex_original'];sf=source['mesh_triangle']
        uv=np.tile(np.array([[0.,0.],[1.,0.],[0.,1.]])[None],(len(f),1,1));parents=np.arange(len(f))
        for _ in range(control_levels):
            step,f,sharp,uv=refine(len(v),f,sharp,uv);v=step@v;parents=np.repeat(parents,4)
        control,points,triangles,local_parents,local_uv,history,effective,safety=fit_control_mesh(v,f,sharp,sv,sf,sample_levels,iterations)
        bary=np.concatenate([1-local_uv.sum(axis=2,keepdims=True),local_uv],axis=2)
        mapped_uv=np.einsum('nij,njk->nik',bary,uv[local_parents])
        np.savez_compressed(out/'subdivision_target.npz',control=control,points=points,triangles=triangles,
                            parents=parents[local_parents],face_uv=mapped_uv,coarse_triangles=mesh['triangles'],coarse_regions=regions,
                            sharp=target['sharp'],input_control=mesh['vertices'],source_ids=mesh['source_ids'])
        shutil.copy2(src/'simplified_mesh.npz',out/'simplified_mesh.npz')
        report.update(status='target_ready',control_vertices=len(v),target_triangles=len(triangles),
                      subdivision_fit=history,subdivision_collision_safeguards=safety)
    except Exception as exc:report.update(status='failed',error=str(exc));raise
    finally:report['wall_seconds']=time.perf_counter()-started;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True)
    p.add_argument('--control-levels',type=int,default=1);p.add_argument('--sample-levels',type=int,default=2)
    p.add_argument('--iterations',type=int,default=6);a=p.parse_args()
    fit(a.source,a.output,a.control_levels,a.sample_levels,a.iterations)
