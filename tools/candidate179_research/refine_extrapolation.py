"""Two evidence-triggered controls; same data, degree, poles and ridge."""
import argparse
import time
from pathlib import Path
import numpy as np
from geometry import *
from spline import fit_graph,to_occt,evaluate as ev
from fit_graph import stats
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.TopAbs import TopAbs_IN,TopAbs_ON
from OCP.gp import gp_Pnt2d


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    data=np.load(out/'reference.npz');base=np.load(out/'g17_data/surface.npz');faces=shapes(read_step(out/'g17_data/four_patch_diagnostic.step'),TopAbs_FACE)
    q=(np.arange(61)+.5)/61;grid=np.stack(np.meshgrid(q,q,indexing='ij'),axis=-1).reshape(-1,2)
    outside=np.array([not any(BRepClass_FaceClassifier(f,gp_Pnt2d(float(u),float(v)),1e-9).State() in (TopAbs_IN,TopAbs_ON) for f in faces) for u,v in grid])
    dump(out/'protocol_amendment.json',dict(reason='First six fits revealed 30 mm extrapolation outside trims. Exact support cropping failed to remove it.',
        maximum_additional_fits=2,total_maximum_fits=8,wall_budget_minutes=90,
        controls=['Same g17_data objective solved by augmented SVD instead of normal equations',
                  'Same g17_data solver, with physical bending penalty only outside trimmed region; weight reused from initial fairing ablation'],
        fairness_outside_samples=int(outside.sum()),fairness_total_grid_samples=len(grid),
        no_change_to_acceptance_gates=True))
    for name,method,fairness,uvq in [('g17_svd','svd',0.,None),('g17_exterior_fair','normal',.001,grid[outside])]:
        start=time.perf_counter();target=out/name;target.mkdir(exist_ok=True)
        p,tu,tv,solver=fit_graph(data['train_points'],data['boundary_points'],np.empty((0,3)),base['center'],base['axes'],base['domain'],
                               17,fairness,0.,0.,fairness_uv=uvq,solver_method=method)
        np.savez_compressed(target/'surface.npz',poles=p,tu=tu,tv=tv,center=base['center'],axes=base['axes'],domain=base['domain'])
        write_step(target/'support.step',BRepBuilderAPI_MakeFace(to_occt(p,tu,tv),1e-7).Face())
        report=dict(config=dict(name=name,n=17,fairness=fairness,fairness_domain='outside trim only' if fairness else 'none',boundary_weight=0.,seam_weight=0.),
                    solve=solver,unique_support_poles=289,maximum_pole_change_mm=float(np.linalg.norm(p-base['poles'],axis=2).max()))
        for key,points in [('train',data['train_points']),('test',data['test_points']),('boundary',data['boundary_points'])]:
            uv=(((points-base['center'])@base['axes'].T)[:,:2]-base['domain'][0])/base['domain'][1]
            fitted=ev(p,tu,tv,uv);report[key+'_graph_distance_mm']=stats(np.linalg.norm(fitted-points,axis=1))
            if key=='test':
                du=ev(p,tu,tv,uv,1,0);dv=ev(p,tu,tv,uv,0,1);n=np.cross(du,dv);n/=np.linalg.norm(n,axis=1)[:,None]
                report['test_correspondence_normal_degrees']=stats(np.degrees(np.arccos(np.clip(np.sum(n*data['test_normals'],axis=1),-1,1))))
        uv=np.stack(np.meshgrid(np.linspace(0,1,151),np.linspace(0,1,151),indexing='ij'),axis=-1).reshape(-1,2)
        heights=((ev(p,tu,tv,uv)-base['center'])@base['axes'].T)[:,2]
        report['untrimmed_height_range_mm']=[float(heights.min()),float(heights.max())];report['seconds']=time.perf_counter()-start
        dump(target/'fit.json',report);print(report,flush=True)


if __name__=='__main__':main()
