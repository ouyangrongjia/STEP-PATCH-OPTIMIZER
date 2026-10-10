"""Paired geometric boundary measurements on identical source-edge chains."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump


def seams(run,output,regions=None,samples=129,smooth_degrees=10.):
    """Measure both native polynomial supports on their actual shared curves."""
    from .partition_fit import incidence,parameters,positions
    from .regional_quality import weighted_stats
    if Path(output).exists():raise FileExistsError(output)
    root=Path(run);meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
    z=np.load(meta['partition']);v,f=z['mesh_vertex_original'],z['mesh_triangle']
    edges,owners=incidence(f);keys=edges[:,0]*len(v)+edges[:,1]
    normal=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
    normal/=np.linalg.norm(normal,axis=1)[:,None]
    records={int(p.stem):dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    rows=[];angles=[];gaps=[];weights=[];smooth=[]
    for path in sorted((root/'curves').glob('*.npz')):
        c=dict(np.load(path));pair=list(map(int,c['owners']))
        if regions is not None and not any(int(records[i]['region']) in regions for i in pair):continue
        q=np.unique(np.r_[np.linspace(0,1,samples),c['knots']])
        spline=BSpline(c['knots'],c['poles'],3);xyz=spline(q);nn=[];xx=[]
        for i in pair:
            r=records[i];uv=parameters(xyz,r)
            n=np.cross(positions(uv,r,1,0),positions(uv,r,0,1));n/=np.linalg.norm(n,axis=1)[:,None]
            nn.append(n);xx.append(positions(uv,r))
        angle=np.degrees(np.arccos(np.clip(np.sum(nn[0]*nn[1],axis=1),-1,1)))
        gap=np.linalg.norm(xx[0]-xx[1],axis=1)
        chain=c['chain'];k=np.minimum(chain[:-1],chain[1:])*len(v)+np.maximum(chain[:-1],chain[1:])
        source=normal[owners[np.searchsorted(keys,k)]]
        dihedral=np.degrees(np.arccos(np.clip(np.sum(source[:,0]*source[:,1],axis=1),-1,1)))
        segment=np.clip(np.searchsorted(c['parameters'],q,side='right')-1,0,len(dihedral)-1)
        mask=dihedral[segment]<=smooth_degrees
        ds=np.linalg.norm(spline(q,1),axis=1)*np.r_[np.diff(q)[0]/2,(q[2:]-q[:-2])/2,np.diff(q)[-1]/2]
        angles.append(angle);gaps.append(gap);weights.append(ds);smooth.append(mask)
        rows.append(dict(curve=int(path.stem),charts=pair,normal_degrees=weighted_stats(angle,ds),
            source_smooth_normal_degrees=weighted_stats(angle[mask],ds[mask]) if mask.any() else None))
    angle,gap,w,mask=map(np.concatenate,(angles,gaps,weights,smooth))
    report=dict(source_fit=str(root.resolve()),regions=regions,curves=len(rows),samples=len(angle),
        source_smooth_dihedral_degrees=smooth_degrees,
        all_normal_degrees=weighted_stats(angle,w),source_smooth_normal_degrees=weighted_stats(angle[mask],w[mask]),
        source_smooth_gap_mm=weighted_stats(gap[mask],w[mask]),per_curve=rows,
        scope='Actual shared-curve parameters on cached native polynomial supports; source dihedral is only a crease proxy. '
              'This sampled diagnostic does not certify G1/G2 or replace STEP validation.')
    dump(output,report);return report


def measures(record):
    spline=BSpline(record['knots'],record['poles'],3)
    cuts=np.unique(record['knots']);width=np.diff(cuts)
    q=np.unique((cuts[:-1,None]+width[:,None]*np.linspace(0,1,17)).ravel())
    velocity=spline(q,1);speed=np.linalg.norm(velocity,axis=1)
    tangent=velocity[speed>1e-12]/speed[speed>1e-12,None]
    turning=np.degrees(np.arccos(np.clip(np.sum(tangent[:-1]*tangent[1:],axis=1),-1,1))).sum()
    nodes,weights=np.polynomial.legendre.leggauss(8)
    q=(cuts[:-1,None]+width[:,None]*(nodes+1)/2).ravel()
    w=(width[:,None]*weights/2).ravel();d1=spline(q,1);d2=spline(q,2)
    speed=np.linalg.norm(d1,axis=1)
    bending=np.sum(np.cross(d1,d2)**2,axis=1)/np.maximum(speed,1e-12)**5
    return dict(length_mm=float(w@speed),total_tangent_turn_degrees=float(turning),
                curvature_squared_integral_per_mm=float(w@bending),control_points=len(record['poles']))


def compare(before,after,output):
    if Path(output).exists():raise FileExistsError(output)
    def load(root):
        result={}
        for p in sorted((Path(root)/'curves').glob('*.npz')):
            c=dict(np.load(p));key=tuple(map(int,c['chain']));key=min(key,key[::-1])
            if key in result:raise ValueError('Duplicate source-edge chain')
            result[key]=(int(p.stem),c)
        return result
    old,new=load(before),load(after);rows=[];changed=[]
    for key,(ci,c) in new.items():
        if key not in old:continue
        oi,b=old[key];bm,cm=measures(b),measures(c)
        row=dict(before_curve=oi,after_curve=ci,before=bm,after=cm)
        rows.append(row)
        if not (np.array_equal(c['knots'],b['knots']) and np.array_equal(c['poles'],b['poles'])):
            changed.append(row)
    def totals(items):
        return {side:{name:float(sum(row[side][name] for row in items)) for name in
                ('length_mm','total_tangent_turn_degrees','curvature_squared_integral_per_mm','control_points')}
                for side in ('before','after')}
    report=dict(before=str(Path(before).resolve()),after=str(Path(after).resolve()),
        source_chain_matches=len(rows),before_curves=len(old),after_curves=len(new),changed_matched_curves=len(changed),
        matched_totals=totals(rows),changed_matched_totals=totals(changed),
        changed=changed,scope='Same source-edge chains only; split or merged chains are not paired. '
        'Turning and bending are geometric diagnostics, not a replacement for shape, trim, or continuity checks.')
    dump(output,report);return report


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('before','after','output'):p.add_argument('--'+name,required=True)
    a=p.parse_args();r=compare(a.before,a.after,a.output)
    print({k:v for k,v in r.items() if k!='changed'})
