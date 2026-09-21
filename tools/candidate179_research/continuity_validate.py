"""Independent samples, exact-edge checks, and finite-span map diagnostics."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.special import comb
from geometry import *
from spline import from_occt,tensor
from hard_boundary import parameterize
from hard_boundary_audit import evaluate_shape
from designer_layout import original_loop
from continuity_diagnose import diagnose
from fit_graph import stats
from OCP.BRepTools import BRepTools
from OCP.BRepAdaptor import BRepAdaptor_Curve2d
from OCP.TopAbs import TopAbs_VERTEX


def domain_audit(shape):
    records=[]
    for i,face in enumerate(shapes(shape,TopAbs_FACE)):
        limits=BRepTools.UVBounds_s(face);a,b,c,d=limits;errors=[];sides=set()
        for edge in shapes(face,TopAbs_EDGE):
            pc=BRepAdaptor_Curve2d(edge,face);lo,hi=BRep_Tool.Range_s(edge)
            uv=np.array([[pc.Value(float(t)).X(),pc.Value(float(t)).Y()] for t in np.linspace(lo,hi,97)])
            residual=np.array([abs(uv[:,0]-a).max(),abs(uv[:,0]-b).max(),abs(uv[:,1]-c).max(),abs(uv[:,1]-d).max()])
            sides.add(int(np.argmin(residual)));errors.append(float(residual.min()))
        records.append(dict(face=i,uv_bounds=list(limits),max_pcurve_iso_residual=max(errors),represented_sides=sorted(sides),
            rectangular_iso_boundary=max(errors)<1e-8 and len(sides)==4))
    return dict(all_rectangular_iso_boundaries=all(r['rectangular_iso_boundary'] for r in records),faces=records)


def map_bounds(shape,data):
    x=np.linspace(0,1,6);b=np.array([[comb(5,i)*t**i*(1-t)**(5-i) for i in range(6)] for t in x]);inv=np.linalg.inv(b);rows=[]
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        p,tu,tv,degree=from_occt(BRep_Tool.Surface_s(face));flat=((p-data['center'])@data['axes'].T).reshape(-1,3)
        ku=np.unique(tu);kv=np.unique(tv);cells=[];queries=[]
        for a,c in zip(ku[:-1],ku[1:]):
            for d,e in zip(kv[:-1],kv[1:]):
                u=a+(c-a)*x;v=d+(e-d)*x;u[0]=np.nextafter(a,c);u[-1]=np.nextafter(c,a);v[0]=np.nextafter(d,e);v[-1]=np.nextafter(e,d)
                queries.append(np.array(np.meshgrid(u,v,indexing='ij')).reshape(2,-1).T);cells.append([a,c,d,e])
        uv=np.vstack(queries);du=tensor(uv,tu,tv,1,0,degree)@flat;dv=tensor(uv,tu,tv,0,1,degree)@flat
        det=(du[:,0]*dv[:,1]-du[:,1]*dv[:,0]).reshape(-1,6,6);sign=float(np.sign(np.median(det)))
        coeff=np.einsum('ij,bjk,lk->bil',inv,det*sign,inv);minimum=coeff.min(axis=(1,2));orientation=sign*(-1 if face.Orientation()==TopAbs_REVERSED else 1)
        rows.append(dict(patch=fi,span_cells=len(cells),minimum_bernstein_xy_determinant_mm2=float(minimum.min()),
            cells_without_positive_bound=int(np.sum(minimum<=0)),sampled_sign_reversals=int(np.sum(det*sign<=0)),
            face_normal_points_along_reference_axis=bool(orientation>0)))
    return dict(method='Floating-point Bernstein convex-hull bound for degree-(5,5) XY Jacobian on every knot cell, using one-sided derivatives at cell boundaries.',
        limitation='Numerical polynomial bounds, not interval-arithmetic proof; does not rate physical knot distribution.',patches=rows)


def edge_identity(original,candidate,target_edges=None):
    source,_,_=original_loop(original);target=target_edges if target_edges is not None else [e for e,use in edge_uses(shapes(candidate,TopAbs_FACE)) if len(use)==1]
    ends=np.array([edge_samples(e,2)[1] for e in target]);records=[];matched=[]
    for i,edge in enumerate(source):
        _,p=edge_samples(edge,385);direct=np.linalg.norm(ends-p[[0,-1]],axis=2).max(axis=1);reverse=np.linalg.norm(ends[:,::-1]-p[[0,-1]],axis=2).max(axis=1)
        j=int(np.argmin(np.minimum(direct,reverse)));matched.append(j);_,q=edge_samples(target[j],385)
        if reverse[j]<direct[j]:q=q[::-1]
        lo,hi=BRep_Tool.Range_s(edge);a,b=BRep_Tool.Range_s(target[j]);sc=BRep_Tool.Curve_s(edge,lo,hi);tc=BRep_Tool.Curve_s(target[j],a,b)
        same_basis=sc.Degree()==tc.Degree() and sc.NbPoles()==tc.NbPoles() and sc.NbKnots()==tc.NbKnots()
        pole_error=max(sc.Pole(k).Distance(tc.Pole(k)) for k in range(1,sc.NbPoles()+1)) if same_basis else None
        knot_error=max(abs(sc.Knot(k)-tc.Knot(k)) for k in range(1,sc.NbKnots()+1)) if same_basis else None
        weights_equal=same_basis and all(sc.Weight(k)==tc.Weight(k) for k in range(1,sc.NbPoles()+1))
        multiplicities_equal=same_basis and all(sc.Multiplicity(k)==tc.Multiplicity(k) for k in range(1,sc.NbKnots()+1))
        records.append(dict(regional_edge=i,candidate_edge=j,max_curve_difference_mm=float(np.linalg.norm(p-q,axis=1).max()),
            vertex_difference_mm=float(np.linalg.norm(p[[0,-1]]-q[[0,-1]],axis=1).max()),
            original_tolerance_mm=BRep_Tool.Tolerance_s(edge),candidate_tolerance_mm=BRep_Tool.Tolerance_s(target[j]),
            same_curve_basis=same_basis,control_pole_difference_mm=pole_error,knot_parameter_difference=knot_error,
            weights_equal=weights_equal,multiplicities_equal=multiplicities_equal,parameter_range_equal=lo==a and hi==b))
    def adjacent(a,b):return any(v.IsSame(w) for v in shapes(a,TopAbs_VERTEX) for w in shapes(b,TopAbs_VERTEX))
    same_connections=all(adjacent(source[i],source[j])==adjacent(target[matched[i]],target[matched[j]]) for i in range(len(source)) for j in range(i))
    return dict(original_count=len(source),candidate_count=len(target),unique_matches=len(set(matched)),
        original_vertices=len(shapes(compound(source),TopAbs_VERTEX)),candidate_vertices=len(shapes(compound(target),TopAbs_VERTEX)),edge_vertex_connections_preserved=same_connections,
        max_curve_difference_mm=max(r['max_curve_difference_mm'] for r in records),
        max_vertex_difference_mm=max(r['vertex_difference_mm'] for r in records),
        all_edge_tolerances_equal=all(r['original_tolerance_mm']==r['candidate_tolerance_mm'] for r in records),
        all_curve_representations_equal=all(r['same_curve_basis'] and r['control_pole_difference_mm']==0 and r['knot_parameter_difference']==0 and r['weights_equal'] and r['multiplicities_equal'] and r['parameter_range_equal'] for r in records),records=records)


def start_comparison(out):
    left=shapes(read_step(out/'aligned_inner_cubic/initial_layout.step'),TopAbs_FACE)
    right=shapes(read_step(out/'aligned_refined_control/initial_layout.step'),TopAbs_FACE);rows=[]
    for i,(a,b) in enumerate(zip(left,right)):
        sa=BRep_Tool.Surface_s(a);sb=BRep_Tool.Surface_s(b);_,tu,tv,_=from_occt(sb)
        u=np.unique(np.r_[np.linspace(0,1,129),tu]);v=np.unique(np.r_[np.linspace(0,1,67),tv]);uv=np.array(np.meshgrid(u,v,indexing='ij')).reshape(2,-1).T
        p,_=evaluate(sa,uv);q,_=evaluate(sb,uv);rows.append(dict(patch=i,samples=len(uv),max_xyz_difference_mm=float(np.linalg.norm(p-q,axis=1).max())))
    report=dict(treatment='Cubic versus eight-control-point artificial inner seams and propagated U knots; central interior stays 8x8, radial seams stay 10 controls.',
        same_geometric_start_max_mm=max(r['max_xyz_difference_mm'] for r in rows),patches=rows)
    if report['same_geometric_start_max_mm']>1e-8:raise RuntimeError('Matched compact experiment changed the geometric start')
    dump(out/'matched_start_audit.json',report)


def new_samples(out,data,count,seed):
    original=read_step(out/'original_region.step');faces=shapes(original,TopAbs_FACE)
    ids=json.loads((out/'input_audit.json').read_text(encoding='utf-8'))['selected_face_ids'];check=0.
    for i,face in enumerate(faces):
        mask=data['test_faces']==ids[i];p,_=evaluate(BRep_Tool.Surface_s(face),data['test_uv'][mask]);check=max(check,float(np.linalg.norm(p-data['test_points'][mask],axis=1).max()))
    if check>1e-8:raise ValueError('Source face order changed; remap by geometry before sampling')
    p=data['mesh_points'];tri=data['mesh_triangles'];area=np.linalg.norm(np.cross(p[tri[:,1]]-p[tri[:,0]],p[tri[:,2]]-p[tri[:,0]]),axis=1)
    rng=np.random.default_rng(seed);pick=rng.choice(len(tri),count,p=area/area.sum());r=np.sqrt(rng.random(count));s=rng.random(count)
    uv=np.einsum('ij,ijk->ik',np.c_[1-r,r*(1-s),r*s],data['mesh_uv'][tri[pick]]);faceids=data['mesh_triangle_face'][pick]
    points=np.zeros((count,3));normals=points.copy()
    for i,face in enumerate(faces):
        mask=faceids==i;points[mask],normals[mask]=evaluate(BRep_Tool.Surface_s(face),uv[mask])
        if face.Orientation()==TopAbs_REVERSED:normals[mask]*=-1
    result=dict(data);result.update(test_points=points,test_normals=normals,test_uv=uv,test_faces=np.array(ids)[faceids],train_points=np.empty((0,3)),train_normals=np.empty((0,3)),train_uv=np.empty((0,2)),train_faces=np.empty(0,dtype=int))
    dump(out/'confirmation_sampling.json',dict(count=count,seed=seed,method='Frozen original area-weighted triangles, exact original surface positions/normals, same method as old protocol.',
        original_face_correspondence_max_residual_mm=check,used_in_fitting_or_selection=False))
    return result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--candidate',default='aligned_inner_cubic')
    parser.add_argument('--stage',choices=['geometry','confirmation'],required=True);args=parser.parse_args();out=Path(args.run);data=np.load(out/'reference.npz')
    original=read_step(out/'original_region.step')
    if args.stage=='geometry':
        start_comparison(out);records={'original_domain':domain_audit(original)}
        for name in ['control','linear_xy_soft',args.candidate]:
            shape=read_step(out/name/'patch_faces.step');target=out/name
            detail=json.loads((target/'defects.json').read_text(encoding='utf-8')) if (target/'defects.json').exists() else {}
            detail.update(diagnose(shape,data,target));dump(target/'defects.json',detail)
            records[name]=dict(domain=domain_audit(shape),map_bounds=map_bounds(shape,data),original_edge_identity=edge_identity(original,shape))
            integrated=target/'integrated_boundary_audit.json'
            if integrated.exists():
                selected={r['full_face'] for r in json.loads(integrated.read_text(encoding='utf-8'))['matched_faces']}
                whole=read_step(target/'whole_model_diagnostic.step');faces=shapes(whole,TopAbs_FACE)
                boundary=[e for e,uses in edge_uses(faces) if selected.intersection(uses) and set(uses)-selected]
                records[name]['integrated_outer_edge_identity']=edge_identity(original,whole,boundary)
        dump(out/'geometry_validation.json',records)
    else:
        protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'));sampling=protocol['independent_confirmation'];fresh=new_samples(out,data,sampling['count'],sampling['seed'])
        np.savez_compressed(out/'confirmation_reference.npz',**fresh)
        reports={}
        for name in ['control',args.candidate]:
            target=out/name/'confirmation';target.mkdir(exist_ok=True);shape=read_step(out/name/'patch_faces.step')
            patch,uv=parameterize(fresh,shapes(shape,TopAbs_FACE),target);report=evaluate_shape(shape,fresh,target,patch,uv)
            report['domain_note']='Verified rectangular iso-bounded candidate Faces; the projection uses the actual complete face domain.'
            dump(target/'evaluation.json',report);reports[name]=report
            print(name,'independent',report['distance_mm'],report['normal_degrees'],flush=True)
        dump(out/'confirmation.json',reports)


if __name__=='__main__':main()
