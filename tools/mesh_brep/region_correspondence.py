"""Audit supplied region correspondence without relabeling the input mesh.

Fitted-face attribution uses nearest STL votes and is explicitly diagnostic.
Tiny faces receive deterministic trimmed-domain samples instead of being dropped.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from OCP.BRep import BRep_Tool
from OCP.TopAbs import TopAbs_FACE
from . import cad
from .mesh import Locator, dump
from .partition_fit import incidence
from .step_quality import native


def audit(quality, output):
    root=Path(quality); meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
    data=np.load(meta['partition']); labels=data['face_instance_id']
    _,owners=incidence(data['mesh_triangle'])
    pairs=labels[owners]; expected={tuple(sorted(map(int,p))) for p in pairs if p[0]!=p[1]}
    votes=json.loads((root/'source_region_votes.json').read_text(encoding='utf-8'))['faces']
    small=[r['face'] for r in votes if r['samples']<25]
    if small:
        with np.load(root/'display_mesh.npz') as archive:
            mesh={k:archive[k] for k in ['points','triangles','owners','uv']}
        faces=cad.shapes(cad.read_generated(meta['step']),TopAbs_FACE)
        loc=Locator(data['mesh_vertex_original'],data['mesh_triangle']);rng=np.random.default_rng(930603)
        for fi in small:
            tri=mesh['triangles'][mesh['owners']==fi]
            xyz=mesh['points'][tri]; area=np.linalg.norm(np.cross(xyz[:,1]-xyz[:,0],xyz[:,2]-xyz[:,0]),axis=1)
            if not len(tri) or area.sum()<=1e-25: continue
            choice=rng.choice(len(tri),size=121,p=area/area.sum())
            r=rng.random((121,2));u=np.sqrt(r[:,0]);bary=np.c_[1-u,u*(1-r[:,1]),u*r[:,1]]
            uv=np.einsum('ij,ijk->ik',bary,mesh['uv'][tri[choice]])
            p,_,_,_=native(BRep_Tool.Surface_s(faces[fi]),uv)
            _,_,ids,_=loc.closest(p);unique,counts=np.unique(labels[ids],return_counts=True);winner=np.argmax(counts)
            votes[fi]=dict(face=fi,samples=121,majority_region=int(unique[winner]),majority_fraction=float(counts[winner]/121),
                           votes={str(int(k)):int(n) for k,n in zip(unique,counts)},tiny_face_supplement=True)
    assigned={r['face']:r.get('majority_region',-1) for r in votes}
    observed=set()
    for edge in json.loads((root/'edges.json').read_text(encoding='utf-8'))['records']:
        if len(edge['owners'])!=2:continue
        a,b=[assigned[i] for i in edge['owners']]
        if a>=0 and b>=0 and a!=b:observed.add(tuple(sorted([a,b])))
    found=set(assigned.values())-{-1}; expected_regions=set(map(int,np.unique(labels)))
    result=dict(method='area samples on trimmed fitted faces; nearest original STL label votes',
        changes_input_partition=False, exact_combinatorial_correspondence_certified=False,
        expected_regions=len(expected_regions), observed_regions=len(found), missing_regions=sorted(expected_regions-found),
        expected_adjacency_pairs=len(expected), observed_adjacency_pairs=len(observed),
        missing_adjacency_pairs=sorted(expected-observed), extra_adjacency_pairs=sorted(observed-expected),
        ambiguous_faces=[r['face'] for r in votes if r.get('majority_fraction',0)<.99], faces=votes)
    dump(output,result)
    return {k:v for k,v in result.items() if k!='faces'}


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--quality',required=True);p.add_argument('--output',required=True)
    a=p.parse_args();print(json.dumps(audit(a.quality,a.output)))
