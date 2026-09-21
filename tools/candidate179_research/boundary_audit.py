"""Evaluate original outer edges and the requested neighbour continuity."""
import argparse
import json
from pathlib import Path
import numpy as np
from geometry import *
from fit_graph import stats
from OCP.BRepAdaptor import BRepAdaptor_Curve2d


def normals_on_edge(edge,face,t):
    pc=BRepAdaptor_Curve2d(edge,face);uv=np.array([[pc.Value(float(x)).X(),pc.Value(float(x)).Y()] for x in t])
    p,n=evaluate(BRep_Tool.Surface_s(face),uv)
    if face.Orientation()==TopAbs_REVERSED:n*=-1
    return p,n


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',required=True);parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True);args=parser.parse_args()
    out=Path(args.run);ref=np.load(out/'reference.npz');input_audit=json.loads((out/'input_audit.json').read_text(encoding='utf-8'));selected=set(input_audit['selected_face_ids'])
    model=read_step(args.source);allfaces=shapes(model,TopAbs_FACE);ei=TopTools_IndexedMapOfShape();TopExp.MapShapes_s(model,TopAbs_EDGE,ei)
    network=json.loads((Path(args.baseline)/'curve_network.json').read_text(encoding='utf-8-sig'))
    requested={int(e['original_edge_id']):e['requested_continuity'] for e in network['edges'] if e['original_boundary']}
    variants={name:(np.load(out/name/'surface.npz'),BRep_Tool.Surface_s(shapes(read_step(out/name/'four_patch_diagnostic.step'),TopAbs_FACE)[0])) for name in ['g17_data','g17_outer','g25_strict_outer']}
    rows=[]
    for edge,uses in edge_uses(allfaces):
        inside=[i for i in uses if i in selected];outside=[i for i in uses if i not in selected]
        if len(inside)!=1 or len(outside)!=1:continue
        t,points=edge_samples(edge,385);_,original_normal=normals_on_edge(edge,allfaces[inside[0]],t);_,adjacent_normal=normals_on_edge(edge,allfaces[outside[0]],t)
        edge_id=ei.FindIndex(edge)-1
        row=dict(original_edge_id=edge_id,selected_face=inside[0],adjacent_face=outside[0],requested_continuity=requested.get(edge_id,'unmapped'),
                 original_normal_gap_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(original_normal*adjacent_normal,axis=1),-1,1)))),candidates={})
        for name,(fit,surface) in variants.items():
            uv=(((points-fit['center'])@fit['axes'].T)[:,:2]-fit['domain'][0])/fit['domain'][1]
            p,n=evaluate(surface,uv)
            row['candidates'][name]=dict(original_curve_to_support_mm=stats(np.linalg.norm(p-points,axis=1)),
                                       adjacent_normal_gap_degrees=stats(np.degrees(np.arccos(np.clip(np.sum(n*adjacent_normal,axis=1),-1,1)))))
        rows.append(row)
    points=ref['mesh_points'];tri=ref['mesh_triangles'];xy=((points-ref['center'])@ref['axes'].T)[:,:2]
    domain=np.load(out/'g17_data/surface.npz')['domain'];uv=(xy-domain[0])/domain[1]
    physical=np.stack([points[tri[:,1]]-points[tri[:,0]],points[tri[:,2]]-points[tri[:,0]]],axis=-1)
    _,r=np.linalg.qr(physical);param=np.stack([uv[tri[:,1]]-uv[tri[:,0]],uv[tri[:,2]]-uv[tri[:,0]]],axis=-1)
    singular=np.linalg.svd(param@np.linalg.inv(r),compute_uv=False);ratio=singular[:,0]/singular[:,1]
    result=dict(outer_edges=len(rows),samples_per_edge=385,graph_mesh_parameter_condition_ratio=stats(ratio),edges=rows)
    dump(out/'boundary_audit.json',result)
    print('outer_edges',len(rows),'param_condition',result['graph_mesh_parameter_condition_ratio'])
    for name in variants:
        print(name,'G1 requested edges',sum(r['requested_continuity']=='G1' for r in rows),
              'G1 worst',max(r['candidates'][name]['adjacent_normal_gap_degrees']['max'] for r in rows if r['requested_continuity']=='G1'))


if __name__=='__main__':main()
