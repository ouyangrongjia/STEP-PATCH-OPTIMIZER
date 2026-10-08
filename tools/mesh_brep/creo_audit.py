"""Inspect actual Creo STEP re-export, keeping native and OCCT facts separate."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import time

import numpy as np
from OCP.BRep import BRep_Tool
from OCP.TopAbs import TopAbs_FACE,TopAbs_EDGE
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS

from . import cad
from .partition_cad import topology
from .mesh import dump,Locator
from .regional_quality import display_mesh


def audit(step,source,output):
    step,source,output=map(Path,(step,source,output));started=time.perf_counter()
    raw=Counter(re.findall(r'=\s*(ADVANCED_FACE|MANIFOLD_SOLID_BREP|CLOSED_SHELL|OPEN_SHELL)\s*\(',step.read_text(encoding='ascii',errors='replace')))
    shape=cad.read_generated(step);report=dict(creo_step_entities=dict(raw),occt_reread=topology(shape))
    index=TopTools_IndexedMapOfShape();owners=[]
    for fi,face in enumerate(cad.shapes(shape,TopAbs_FACE)):
        for edge in cad.shapes(face,TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge):continue
            j=index.Add(edge)-1
            if j==len(owners):owners.append([])
            owners[j].extend([fi]*(2 if BRep_Tool.IsClosed_s(edge,face) else 1))
    records=[]
    for j,uses in enumerate(owners):
        if len(uses)!=1:continue
        edge=TopoDS.Edge_s(index.FindKey(j+1));a,b=BRep_Tool.Range_s(edge);curve=BRep_Tool.Curve_s(edge,a,b)
        if curve:
            points=np.array([curve.Value(float(t)).Coord() for t in np.linspace(a,b,17)])
            records.append(dict(edge=j,owners=uses,length_mm=float(np.linalg.norm(np.diff(points,axis=0),axis=1).sum()),center=points[8].tolist()))
    if records:
        if (source/'surfaces.npz').is_file():
            data=np.load(source/'surfaces.npz');p,f,_,owner,_=display_mesh(data['poles'],data['corners'],13)
            regions=data['regions']
        else:
            data=np.load(source/'display_mesh.npz');p,f,owner=data['points'],data['triangles'],data['owners']
            regions=np.full(int(owner.max())+1,-1,dtype=int)
            votes=json.loads((source/'source_region_votes.json').read_text(encoding='utf-8'))['faces']
            for rec in votes:regions[rec['face']]=rec.get('majority_region',-1)
        loc=Locator(p,f);_,distance,ids,_=loc.closest(np.array([r['center'] for r in records]))
        for i,r in enumerate(records):r.update(nearest_patch=int(owner[ids[i]]),region=int(regions[owner[ids[i]]]),distance_mm=float(distance[i]))
    report.update(free_edge_records=records,seconds=time.perf_counter()-started)
    dump(output,report);print(json.dumps({k:v for k,v in report.items() if k!='free_edge_records'}),flush=True)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--step',required=True);p.add_argument('--source',required=True);p.add_argument('--output',required=True)
    args=p.parse_args();audit(args.step,args.source,args.output)
