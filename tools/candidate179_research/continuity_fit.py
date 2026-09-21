"""Fixed-start height fits and polynomial coplanarity constraints at C0 knots.

Outer curves are unchanged. The hard constraints impose tangent-plane coplanarity,
not equal parameter derivatives. Rank and residuals are recorded, not assumed.
"""
import argparse
import json
import shutil
import time
from pathlib import Path
import numpy as np
from scipy import sparse, linalg
from scipy.sparse.linalg import splu
from geometry import *
from spline import from_occt, tensor, basis
from designer_layout import build_layout, uv_on_side, variable_maps, assemble, export_layout
from integrate import edge_checks
from diagnose import seam_quality
from evaluate import differential
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace


def fixed_start(out,data,protocol,seed_mode=None):
    previous=Path(protocol['previous_run']);old=json.loads((previous/'protocol.json').read_text(encoding='utf-8'))
    config=next(c for c in old['configurations'] if c['name']=='five_knot_smooth');effective=dict(old);effective.update(config)
    if seed_mode:effective['inner_seam_control_points']=4
    layout=build_layout(data,read_step(out/'original_region.step'),effective,config['boundary_layers'])
    if seed_mode:
        # Common coarse seed: exact original boundary, cubic artificial inner
        # curves, and the original central interior space. Refinement below
        # inserts knots into this existing geometry, never before Coons.
        for value in [.2,.4,.6,.8]:
            layout['surfaces'][4].InsertUKnot(value,1,1e-12,False)
            layout['surfaces'][4].InsertVKnot(value,1,1e-12,False)
        # Rebuilding even a straight-sided central Coons patch with four versus
        # eight boundary controls changes its interior XY map. Restore the
        # archived central map explicitly; only its initial height is different.
        archived=shapes(read_step(out/'control/patch_faces.step'),TopAbs_FACE)
        original_p,ou,ov,od=from_occt(BRep_Tool.Surface_s(archived[4]));p,tu,tv,degree=from_occt(layout['surfaces'][4])
        if p.shape!=original_p.shape or not np.allclose(tu,ou,atol=1e-12,rtol=0) or not np.allclose(tv,ov,atol=1e-12,rtol=0):raise ValueError('Central basis changed')
        local=(p-data['center'])@data['axes'].T;local[:,:,:2]=((original_p-data['center'])@data['axes'].T)[:,:,:2];p=local@data['axes']+data['center']
        for i in range(p.shape[0]):
            for j in range(p.shape[1]):layout['surfaces'][4].SetPole(i+1,j+1,gp_Pnt(*p[i,j]))
        if seed_mode=='refined':
            for surface in layout['surfaces'][:4]:
                for value in [.2,.4,.6,.8]:surface.InsertUKnot(value,1,1e-12,False)
            for curve in layout['seam_curves'][4:]:
                for value in [.2,.4,.6,.8]:curve.InsertKnot(value,1,1e-12,False)
            layout['seam_knots']=[np.repeat([c.Knot(k) for k in range(1,c.NbKnots()+1)],[c.Multiplicity(k) for k in range(1,c.NbKnots()+1)]) for c in layout['seam_curves']]
        layout.update(corner_indices=protocol['corner_edge_indices'],inner_final=layout['inner'].copy(),normal_axis=data['axes'][2])
        return layout,dict(initial_surfaces='Common coarse seed, archived central XY map restored, followed by geometry-preserving post-construction refinement for the matched fine control.',seed_mode=seed_mode)
    control=read_step(out/'control/patch_faces.step');faces=shapes(control,TopAbs_FACE);checks=[]
    for i,face in enumerate(faces):
        oldp,*_=from_occt(layout['surfaces'][i]);new=BRep_Tool.Surface_s(face).Copy();newp,*_=from_occt(new)
        checks.append(float(np.max(np.abs(((newp-oldp)@data['axes'].T)[:,:,:2]))));layout['surfaces'][i]=new
    if max(checks)>1e-8:raise ValueError('Control does not share the reconstructed fixed XY map')
    seam_errors=[]
    for sid,curve in enumerate(layout['seam_curves']):
        fi,side=next((fi,s) for fi,sides in enumerate(layout['sides']) for s in sides if not s['outer'] and s['seam']==sid)
        t=layout['seam_knots'][sid];samples=np.unique(np.r_[np.linspace(0,1,1001),t]);b=basis(t,3,samples)
        values,_=evaluate(layout['surfaces'][fi],uv_on_side(side['mapping'],samples))
        poles=linalg.lstsq(b,values,lapack_driver='gelsy')[0]
        for i,p in enumerate(poles):curve.SetPole(i+1,gp_Pnt(*p))
        actual=np.array([xyz(curve.Value(float(t))) for t in samples]);error=float(np.linalg.norm(actual-values,axis=1).max());seam_errors.append(error)
        if error>1e-8:raise ValueError('Archived shared seam not representable in original master space')
    inner=np.array([xyz(c.EndPoint()) for c in layout['seam_curves'][:4]])
    layout.update(corner_indices=protocol['corner_edge_indices'],inner_final=inner.copy(),inner=inner,normal_axis=data['axes'][2])
    return layout,dict(maximum_control_start_xy_difference_mm=max(checks),maximum_reconstructed_seam_residual_mm=max(seam_errors),
        initial_surfaces='Exact archived five_knot_smooth STEP supports; no Coons regeneration used as fit start.')


def coplanarity(layout,maps,data):
    constraints=[];targets=[];records=[]
    for fi,surface in enumerate(layout['surfaces']):
        p,tu,tv,degree=from_occt(surface);local=((p-data['center'])@data['axes'].T).reshape(-1,3)
        for axis,t in enumerate([tu,tv]):
            knots,mults=np.unique(t,return_counts=True)
            for i in range(1,len(knots)-1):
                if mults[i]<degree[axis]:continue
                if axis!=0 or tuple(degree)!=(3,3):raise ValueError('This prototype expects bicubic U knot constraints')
                other=np.unique(tv);samples=[]
                # Degree <= 8 polynomial determinant on each V span; 12 nodes
                # oversample it. Endpoints are also constrained, including the
                # original corner. XY is fixed, so the determinant is linear in Z.
                x=(1-np.cos(np.linspace(0,np.pi,12)))/2
                for a,b in zip(other[:-1],other[1:]):samples.extend(a+(b-a)*x)
                samples=np.unique(samples);left=np.c_[np.full(len(samples),np.nextafter(knots[i],-np.inf)),samples]
                right=left.copy();right[:,0]=np.nextafter(knots[i],np.inf)
                au=tensor(left,tu,tv,1,0,degree);bu=tensor(right,tu,tv,1,0,degree);vt=tensor(left,tu,tv,0,1,degree)
                a=au@local[:,:2];b=bu@local[:,:2];v=vt@local[:,:2]
                cross=lambda x,y:x[:,0]*y[:,1]-x[:,1]*y[:,0]
                op=bu.multiply(cross(a,v)[:,None])-au.multiply(cross(b,v)[:,None])+vt.multiply(cross(b,a)[:,None])
                mapped=(op@maps[fi]).toarray();rhs=-op@local[:,2]
                scale=np.linalg.norm(mapped,axis=1);keep=scale>1e-12;mapped=mapped[keep]/scale[keep,None];rhs=rhs[keep]/scale[keep]
                # Orthonormalize independent equations before the Schur solve.
                # Selecting raw rows squares their conditioning in that solve.
                u,s,vh=linalg.svd(mapped,full_matrices=False)
                rank=int(np.sum(s>max(s[0]*1e-10,1e-12)))
                constraints.append(vh[:rank]);targets.append((u[:,:rank].T@rhs)/s[:rank])
                records.append(dict(patch=fi,knot=float(knots[i]),rows=len(samples),rank=rank,
                    polynomial_degree_bound=8,rank_relative_threshold=1e-10,
                    retained_condition=float(s[0]/s[rank-1]),discarded_rhs_norm=float(np.linalg.norm(rhs-u[:,:rank]@(u[:,:rank].T@rhs)))))
    return np.vstack(constraints),np.concatenate(targets),records


def fit(out,name,config):
    start=time.perf_counter();data=np.load(out/'reference.npz');protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'))
    effective=dict(protocol);effective.update(config);target=out/name;target.mkdir(exist_ok=False)
    layout,start_report=fixed_start(out,data,protocol,config.get('common_compact_seed'));maps,seam_maps,total=variable_maps(layout)
    if config.get('linear_xy_blend',False):
        shifts=[]
        for surface in layout['surfaces'][:4]:
            p,tu,tv,degree=from_occt(surface);local=(p-data['center'])@data['axes'].T
            gv=np.array([tv[i+1:i+4].mean() for i in range(len(tv)-4)])
            newxy=local[:,0,None,:2]*(1-gv[None,:,None])+local[:,-1,None,:2]*gv[None,:,None]
            shifts.append(float(np.linalg.norm(newxy-local[:,:,:2],axis=2).max()))
            local[:,:,:2]=newxy;newp=local@data['axes']+data['center']
            for i in range(p.shape[0]):
                for j in range(1,p.shape[1]-1):surface.SetPole(i+1,j+1,gp_Pnt(*newp[i,j]))
        start_report.update(mapping_change='Linear transverse XY blend; same original and artificial boundary curves, knot vectors, height start and objective.',maximum_xy_pole_shift_mm=max(shifts))
        from hard_boundary import parameterize
        seed=export_layout(layout,target/'initial_layout.step');parameterize(data,shapes(seed,TopAbs_FACE),target)
    else:shutil.copy2(out/'control/parameterization.npz',target/'parameterization.npz')
    params=np.load(target/'parameterization.npz')
    matrix,rhs,knots=assemble(data,layout,maps,params,effective)
    scale=1/np.maximum(np.sqrt(np.asarray(matrix.power(2).sum(axis=0)).ravel()),1e-15)
    a=matrix@sparse.diags(scale);h=(a.T@a).tocsc()+sparse.diags(1e-12*scale*scale,format='csc');g=a.T@rhs
    factor=splu(h);x=factor.solve(g);constraint_report={}
    if config.get('hard_g1',False):
        c,d,records=coplanarity(layout,maps,data);cs=c*scale[None,:]
        inv=factor.solve(cs.T);schur=cs@inv
        multiplier=linalg.solve(schur,cs@x-d,assume_a='sym');x-=inv@multiplier
        constraint_report=dict(knots=records,max_constraint_residual_mm=float(np.max(np.abs(cs@x-d))),
            schur_condition=float(np.linalg.cond(schur)),constraints=len(d))
    delta=scale*x;patches=[]
    for fi,(surface,mapping) in enumerate(zip(layout['surfaces'],maps)):
        p,tu,tv,degree=from_occt(surface);change=(mapping@delta).reshape(p.shape[:2]);newp=p+change[:,:,None]*data['axes'][2]
        for i in range(p.shape[0]):
            for j in range(p.shape[1]):
                if change[i,j]!=0:surface.SetPole(i+1,j+1,gp_Pnt(*newp[i,j]))
        np.savez_compressed(target/f'patch_{fi}.npz',poles=newp,tu=tu,tv=tv,degrees=degree)
        patches.append(dict(patch=fi,complexity=surface_info(surface),differential=differential(newp,tu,tv,degree,data['axes'])))
    for sid,curve in enumerate(layout['seam_curves']):
        height=seam_maps[sid]@delta[:seam_maps[sid].shape[1]]
        for i,value in enumerate(height):curve.SetPole(i+1,gp_Pnt(*(xyz(curve.Pole(i+1))+value*data['axes'][2])))
    layout['inner_final']=layout['inner']+delta[:4,None]*data['axes'][2]
    shape=export_layout(layout,target/'patch_faces.step');reread=read_step(target/'patch_faces.step')
    write_step(target/'support.step',compound([BRepBuilderAPI_MakeFace(s,1e-7).Face() for s in layout['surfaces']]))
    result=dict(config=config,start=start_report,solver=dict(unknowns=total,rows=matrix.shape[0],constraints=constraint_report,
        maximum_free_height_change_mm=float(np.abs(delta).max()),weighted_residual_norm=float(np.linalg.norm(matrix@delta-rhs))),
        status='DiagnosticOnly',can_apply=False,patches=patches,actual_control_points=sum(s.NbUPoles()*s.NbVPoles() for s in layout['surfaces']),
        before_write=topology(shape),after_read=topology(reread),edge_checks=edge_checks(reread,385),seams=seam_quality(reread),
        boundary_mapping=layout['boundary'],sides=layout['sides'],inner_corners_initial=layout['inner'].tolist(),
        inner_corners_final=layout['inner_final'].tolist(),seconds=time.perf_counter()-start)
    dump(target/'fit.json',result);print(name,'seconds',result['seconds'],'constraints',constraint_report,'valid',result['after_read']['brep_valid'],flush=True)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--names',nargs='+');args=parser.parse_args()
    out=Path(args.run);protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'))
    for config in protocol['configurations']:
        if not args.names or config['name'] in args.names:fit(out,config['name'],config)


if __name__=='__main__':main()
