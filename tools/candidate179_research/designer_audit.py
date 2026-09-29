"""Shape, original-boundary, knot-crease and footprint checks for new layouts."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon
from geometry import *
from spline import from_occt
from diagnose import reverse_distance
from hard_boundary_audit import evaluate_shape
from designer_layout import original_loop, uv_on_side
from boundary_audit import normals_on_edge
from fit_graph import stats


def knot_jumps(shape):
    result=[]
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        surface=BRep_Tool.Surface_s(face)
        for axis in [0,1]:
            count=surface.NbUKnots() if axis==0 else surface.NbVKnots()
            knot=surface.UKnot if axis==0 else surface.VKnot
            multiplicity=surface.UMultiplicity if axis==0 else surface.VMultiplicity
            degree=surface.UDegree() if axis==0 else surface.VDegree()
            for k in range(2,count):
                if multiplicity(k)<degree:continue
                t=knot(k);epsilon=min(t-knot(k-1),knot(k+1)-t)*1e-7
                left=np.empty((385,2));left[:,axis]=t-epsilon;left[:,1-axis]=np.linspace(0,1,385)
                right=left.copy();right[:,axis]=t+epsilon
                _,nl=evaluate(surface,left);_,nr=evaluate(surface,right)
                angle=np.degrees(np.arccos(np.clip(np.sum(nl*nr,axis=1),-1,1)))
                result.append(dict(patch=fi,axis=axis,knot=t,multiplicity=multiplicity(k),normal_jump_degrees=stats(angle)))
    return result


def footprints(shape,data):
    polys=[];records=[]
    t=np.linspace(0,1,385)
    boundary=np.r_[np.c_[t,t*0],np.c_[t*0+1,t],np.c_[t[::-1],t*0+1],np.c_[t*0,t[::-1]]]
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        p,_=evaluate(BRep_Tool.Surface_s(face),boundary);xy=((p-data['center'])@data['axes'].T)[:,:2]
        poly=Polygon(xy);rect=np.asarray(poly.minimum_rotated_rectangle.exterior.coords)
        lengths=np.linalg.norm(np.diff(rect,axis=0),axis=1)
        records.append(dict(patch=fi,projected_area_mm2=poly.area,minimum_rectangle_aspect=float(lengths.max()/lengths.min()),valid_polygon=poly.is_valid))
        polys.append(poly)
    overlaps=[]
    for i in range(len(polys)):
        for j in range(i):overlaps.append(dict(patches=[j,i],area_mm2=polys[i].intersection(polys[j]).area))
    return dict(patches=records,overlaps=overlaps,total_projected_area_mm2=sum(p.area for p in polys))


def original_boundaries(out,protocol,variants,fits,output=None):
    data=np.load(out/'reference.npz');audit=json.loads((out/'input_audit.json').read_text(encoding='utf-8'))
    original=read_step(audit['source']);allfaces=shapes(original,TopAbs_FACE);selected=set(audit['selected_face_ids'])
    region=read_step(out/'original_region.step');regional_edges,_,_=original_loop(region)
    ends=np.array([edge_samples(e,3)[1][[0,-1]] for e in regional_edges]);indexmap=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(original,TopAbs_EDGE,indexmap)
    network=json.loads((Path(protocol['baseline'])/'curve_network.json').read_text(encoding='utf-8-sig'))
    requested={int(e['original_edge_id']):e['requested_continuity'] for e in network['edges'] if e['original_boundary']}
    rows=[]
    for edge,uses in edge_uses(allfaces):
        inside=[i for i in uses if i in selected];outside=[i for i in uses if i not in selected]
        if len(inside)!=1 or len(outside)!=1:continue
        ts,points=edge_samples(edge,385);target=points[[0,-1]]
        direct=np.linalg.norm(ends-target,axis=2).max(axis=1);backward=np.linalg.norm(ends[:,::-1]-target,axis=2).max(axis=1)
        ri=int(np.argmin(np.minimum(direct,backward)));reverse=backward[ri]<direct[ri]
        if min(direct[ri],backward[ri])>1e-7:raise ValueError('Unmatched original edge')
        _,normal=normals_on_edge(edge,allfaces[outside[0]],ts)
        _,original_normal=normals_on_edge(edge,allfaces[inside[0]],ts)
        edge_id=indexmap.FindIndex(edge)-1
        row=dict(original_edge_id=edge_id,regional_edge=ri,requested_continuity=requested[edge_id],
            original_normal_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(original_normal*normal,axis=1),-1,1)))),variants={})
        for name,shape in variants.items():
            record=next(r for r in fits[name]['boundary_mapping'] if r['edge']==ri)
            face=shapes(shape,TopAbs_FACE)[record['patch']]
            if record.get('project_edge',False):
                # Attached boundaries retain their 3D curve but have a new,
                # generally nonlinear pcurve. Audit the actual STEP pcurve.
                from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve
                candidates=[]
                for candidate in shapes(face,TopAbs_EDGE):
                    _,ep=edge_samples(candidate,2)
                    error=min(np.linalg.norm(ep-target,axis=1).max(),np.linalg.norm(ep[::-1]-target,axis=1).max())
                    candidates.append((error,candidate))
                error,candidate=min(candidates,key=lambda x:x[0])
                if error>1e-5:raise ValueError('Attached STEP boundary cannot be identified')
                a,b=BRep_Tool.Range_s(candidate);curve=BRep_Tool.Curve_s(candidate,a,b)
                pc=BRep_Tool.CurveOnSurface_s(candidate,face,a,b);uv=[]
                for point in points:
                    gp=gp_Pnt(*point);query=GeomAPI_ProjectPointOnCurve(gp,curve,a,b)
                    options=[(gp.Distance(curve.Value(a)),a),(gp.Distance(curve.Value(b)),b)]
                    if query.NbPoints():options.append((query.LowerDistance(),query.LowerDistanceParameter()))
                    uv.append(pc.Value(min(options)[1]).Coord())
                uv=np.asarray(uv)
            else:
                s=(ts-ts[0])/(ts[-1]-ts[0]);s=1-s if reverse else s
                a,b=record['normalized_endpoints'];uv=uv_on_side(record['mapping'],a+s*(b-a))
            p,n=evaluate(BRep_Tool.Surface_s(face),uv)
            if face.Orientation()==TopAbs_REVERSED:n*=-1
            row['variants'][name]=dict(original_curve_to_surface_mm=stats(np.linalg.norm(p-points,axis=1)),
                adjacent_normal_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(n*normal,axis=1),-1,1)))))
        rows.append(row)
    destination=Path(output) if output is not None else out/'boundary_audit.json'
    if destination.exists():
        previous=json.loads(destination.read_text(encoding='utf-8'))
        prior={e['original_edge_id']:e for e in previous['edges']}
        for row in rows:
            merged=dict(prior.get(row['original_edge_id'],{}).get('variants',{}));merged.update(row['variants']);row['variants']=merged
    dump(destination,dict(original_edges=len(rows),samples_per_edge=385,edges=rows))
    return {name:dict(original_boundary_max_mm=max(r['variants'][name]['original_curve_to_surface_mm']['max'] for r in rows),
        requested_g1_max_normal_degrees=max(r['variants'][name]['adjacent_normal_degrees']['max'] for r in rows if r['requested_continuity']=='G1')) for name in variants}


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--names',nargs='+');args=parser.parse_args()
    out=Path(args.run);protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'));data=np.load(out/'reference.npz')
    n=len(data['train_points']);started=time.perf_counter();original=read_step(out/'original_region.step')
    variants={};fits={};reports=json.loads((out/'evaluation.json').read_text(encoding='utf-8')) if (out/'evaluation.json').exists() else {}
    for config in protocol['configurations']:
        name=config['name']
        if args.names and name not in args.names:continue
        target=out/name;shape=read_step(target/'patch_faces.step');variants[name]=shape
        fits[name]=json.loads((target/'fit.json').read_text(encoding='utf-8'));params=np.load(target/'parameterization.npz')
        report=evaluate_shape(shape,data,target,params['patch'][n:],params['uv'][n:])
        report['domain_note']='All new quadrilateral patches are full rectangular iso-bounded Faces; original exterior curves remain exact.'
        report['reverse'],points,distance=reverse_distance(shape,original,data);np.savez_compressed(target/'reverse.npz',points=points,distance=distance)
        report['knot_jumps']=knot_jumps(shape);report['footprints']=footprints(shape,data)
        reports[name]=report;dump(target/'evaluation.json',report)
        print(name,report['distance_mm'],report['normal_degrees'],'knot jump',max((r['normal_jump_degrees']['max'] for r in report['knot_jumps']),default=0),flush=True)
    boundaries=original_boundaries(out,protocol,variants,fits)
    for name,record in boundaries.items():reports[name].update(record)
    reports['baseline_footprints']=footprints(read_step(Path(protocol['baseline'])/'trimmed_patch_faces.step'),data)
    reports['seconds']=time.perf_counter()-started;dump(out/'evaluation.json',reports)
    print('Completed',reports['seconds'],flush=True)


if __name__=='__main__':main()
