"""Bounded ablations of projected graph supports on the original CAD region."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from geometry import dump, write_step, compound
from spline import fit_graph, to_occt, evaluate
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


CONFIGS = [
    dict(name='g17_data', n=17, fairness=0., boundary_weight=0., seam_weight=0.),
    dict(name='g17_fair', n=17, fairness=.001, boundary_weight=0., seam_weight=0.),
    dict(name='g17_outer', n=17, fairness=.001, boundary_weight=100., seam_weight=0.),
    dict(name='g17_frozen_seams', n=17, fairness=.001, boundary_weight=100., seam_weight=100.),
    dict(name='g25_outer', n=25, fairness=.001, boundary_weight=100., seam_weight=0.),
    dict(name='g25_strict_outer', n=25, fairness=.001, boundary_weight=100000., seam_weight=0.),
]


def stats(x):
    return dict(max=float(np.max(x)),rms=float(np.sqrt(np.mean(x*x))),p95=float(np.quantile(x,.95)),p99=float(np.quantile(x,.99)))


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--run',required=True)
    parser.add_argument('--baseline',required=True)
    args=parser.parse_args()
    out=Path(args.run)
    data=np.load(out/'reference.npz')
    network=json.loads((Path(args.baseline)/'curve_network.json').read_text(encoding='utf-8-sig'))
    internal=np.array([s['point'] for e in network['edges'] if not e['original_boundary'] for s in e['samples']])
    center,axes=data['center'],data['axes']
    local=(np.vstack([data['mesh_points'],data['boundary_points'],internal])-center)@axes.T
    lo,hi=local[:,:2].min(axis=0)-1e-5,local[:,:2].max(axis=0)+1e-5
    domain=np.array([lo,hi-lo])
    dump(out/'protocol.json',dict(units='mm',configurations=CONFIGS,maximum_fits=6,
        execution_budget_minutes=90,source_truth='Original STEP FaceIds 1702..1750; baseline only provides comparison and artificial seam layout',
        primary_constraints=dict(distance_mm=.03,boundary_mm=.001,normal_degrees=15,seam_normal_degrees=5,max_poles_per_direction=25),
        boundary_semantics='Penalty experiments. No hard-boundary or Apply success is inferred from a low residual.',
        development='All six planned configurations reported; candidate179 is a development case, not independent confirmation.',
        fairness='Mean squared physical second derivatives of graph height; coefficient in mm^4; 61x61 midpoint quadrature',
        geometry_plan='Four trimmed faces on a shared graph support; preserve projected baseline partition, allow artificial seam height to change'))
    reports=[]
    for config in CONFIGS:
        started=time.perf_counter(); target=out/config['name'];target.mkdir(exist_ok=True)
        p,tu,tv,solve=fit_graph(data['train_points'],data['boundary_points'],internal,center,axes,domain,**{k:v for k,v in config.items() if k!='name'})
        np.savez_compressed(target/'surface.npz',poles=p,tu=tu,tv=tv,center=center,axes=axes,domain=domain)
        surface=to_occt(p,tu,tv)
        write_step(target/'support.step',BRepBuilderAPI_MakeFace(surface,1e-7).Face())
        result=dict(config=config,solve=solve,unique_support_poles=config['n']**2)
        for key,samples in [('train',data['train_points']),('test',data['test_points']),('boundary',data['boundary_points']),('old_seam',internal)]:
            projected=(samples-center)@axes.T
            uv=(projected[:,:2]-lo)/(hi-lo)
            values=evaluate(p,tu,tv,uv)
            error=np.linalg.norm(values-samples,axis=1)
            result[key+'_graph_distance_mm']=stats(error)
            if key=='test':
                du=evaluate(p,tu,tv,uv,1,0);dv=evaluate(p,tu,tv,uv,0,1)
                normals=np.cross(du,dv);normals/=np.linalg.norm(normals,axis=1)[:,None]
                angles=np.degrees(np.arccos(np.clip(np.sum(normals*data['test_normals'],axis=1),-1,1)))
                result['test_correspondence_normal_degrees']=stats(angles)
        result['seconds']=time.perf_counter()-started
        dump(target/'fit.json',result);reports.append(result)
        print(json.dumps(result),flush=True)
    dump(out/'fit_results.json',reports)


if __name__=='__main__':
    main()
