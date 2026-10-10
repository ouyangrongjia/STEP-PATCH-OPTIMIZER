"""Refit selected support-normal outliers while preserving the shared curves."""
import argparse
from concurrent.futures import ProcessPoolExecutor
from dataclasses import replace
import json
from pathlib import Path
import shutil
import time
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump
from .partition_fit import Config
from .source_normals import crease_fan_normals
from .skeleton_graph import fit_support
from .support_probe import probes,normal_targets
from .splines import tensor


def optimize(source,output,regions=None,angle_limit=45.,workers=4,normal_weight=None):
    if not np.isfinite(angle_limit) or not 0<angle_limit<90:
        raise ValueError('Normal limit must be between zero and 90 degrees')
    root,out=Path(source),Path(output);out.mkdir(parents=True,exist_ok=False)
    m=json.loads((root/'run.json').read_text(encoding='utf-8'))
    if not m.get('complete_source_coverage') or m['failed_charts']:
        raise ValueError('Normal repair requires a complete fitted partition')
    cfg=Config(**m['config']);cfg=replace(cfg,normal_angle_limit=angle_limit,
        normal_weight=cfg.normal_weight if normal_weight is None else normal_weight)
    if cfg.normal_weight<0 or not np.isfinite(cfg.normal_weight):raise ValueError('Invalid normal weight')
    z=np.load(m['partition']);v,f,labels=[z[k] for k in ('mesh_vertex_original','mesh_triangle','face_instance_id')]
    selected=set(np.unique(labels) if regions is None else regions)
    if not selected.issubset(set(labels)):raise ValueError('Unknown source regions')
    started=time.perf_counter()
    normal=crease_fan_normals(v,f,cfg.feature_angle,filter_steps=m['normal_filter_steps'])
    curves=[dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))]
    tasks=[];old={};screening=[]
    for path in sorted((root/'charts').glob('*.npz')):
        c=dict(np.load(path));di=int(path.stem)
        if int(c['region']) not in selected:continue
        used=c['vertex_ids'];tri=np.searchsorted(used,f[c['triangle_ids']])
        q,_=probes(c,v[used],tri);target=normal_targets(c,v[used],tri,normal[c['triangle_ids']],q)
        good=target[:,2]>.15;q=q[good];target=target[good];t=c['knots']
        grad=np.column_stack([tensor(q,t,t,int(axis==0),int(axis==1))@c['height']/c['size'][axis] for axis in range(2)])
        predicted=np.c_[-grad,np.ones(len(grad))];predicted/=np.linalg.norm(predicted,axis=1)[:,None]
        angle=np.degrees(np.arccos(np.clip(np.sum(predicted*target,axis=1),-1,1)))
        peak=float(angle.max(initial=0.))
        screening.append(dict(chart=di,region=int(c['region']),normal_max_degrees=peak,samples=len(angle)))
        if peak<=angle_limit:continue
        incident=[x for x in curves if di in x['owners']];samples=[]
        for x in incident:
            cuts=np.unique(np.r_[x['parameters'],x['knots']])
            q=np.unique((cuts[:-1,None]+np.diff(cuts)[:,None]*np.linspace(0,1,5)).ravel())
            samples.append(BSpline(x['knots'],x['poles'],3)(q))
        old[di]=c
        tasks.append((di,v[used],tri,normal[c['triangle_ids']],np.concatenate(samples),incident,cfg,'bounded'))
    dump(out/'normal_screening.json',screening)
    print(dict(stage='normal_screen',screened=len(screening),refit=len(tasks)),flush=True)
    for name in ('charts','curves'):shutil.copytree(root/name,out/name)
    shutil.copy2(root/'chart_index.npz',out/'chart_index.npz')
    fitting={r['chart']:r for r in json.loads((root/'fitting.json').read_text(encoding='utf-8'))}
    attempts=[];accepted=[]
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for di,c,info in pool.map(fit_support,tasks):
            info.update(chart=di,region=int(old[di]['region']));attempts.append(info)
            if c is not None:
                c.update({k:old[di][k] for k in ('region','triangle_ids','vertex_ids')})
                np.savez_compressed(out/'charts'/f'{di:05d}.npz',**c);fitting[di]=info;accepted.append(di)
            print(dict(stage='normal_refit',chart=di,accepted=info['accepted']),flush=True)
            dump(out/'normal_attempts.json',attempts)
    report=dict(source_fit=str(root.resolve()),regions=sorted(map(int,selected)),angle_limit_degrees=angle_limit,
        normal_weight=cfg.normal_weight,screened=len(screening),attempted=len(tasks),accepted_charts=accepted,
        rejected_charts=[r['chart'] for r in attempts if not r['accepted']],seconds=time.perf_counter()-started,
        shared_curves_unchanged=True,source_coordinates_and_labels_unchanged=True,
        failed_updates_keep_previous_support=True,can_apply=False,
        scope='Sampled normal cones and interior probes in selected source domains; actual trimmed STEP and CAD validation required.')
    dump(out/'normal_repair.json',report);dump(out/'fitting.json',list(fitting.values()))
    m.update(source_fit=str(out.resolve()),local_normal_repair=report,status='fitted')
    dump(out/'run.json',m);return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--run',required=True);p.add_argument('--output',required=True)
    p.add_argument('--regions',type=int,nargs='+');p.add_argument('--angle-limit',type=float,default=45.)
    p.add_argument('--workers',type=int,default=4);p.add_argument('--normal-weight',type=float)
    a=p.parse_args();print(optimize(a.run,a.output,a.regions,a.angle_limit,a.workers,a.normal_weight))
