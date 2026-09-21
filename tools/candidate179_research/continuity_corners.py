"""Identify original boundary corners by position, without assuming imported IDs."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from designer_layout import original_loop
from boundary_audit import normals_on_edge
from continuity_diagnose import angle


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args()
    out=Path(args.run);region=read_step(out/'original_region.step');edges,curves,owners=original_loop(region)
    rows=[]
    for i,curve in enumerate(curves):
        previous=curves[(i-1)%len(curves)];point=xyz(curve.StartPoint())
        a=xyz(previous.DN(previous.LastParameter(),1));b=xyz(curve.DN(curve.FirstParameter(),1))
        normals=[]
        for j in [(i-1)%len(curves),i]:
            edge=edges[j];ts,ps=edge_samples(edge,2);idx=int(np.argmin(np.linalg.norm(ps-point,axis=1)))
            _,n=normals_on_edge(edge,owners[j],[ts[idx]]);normals.append(n[0])
        rows.append(dict(regional_edge_after=i,xyz=point.tolist(),position_gap_mm=float(np.linalg.norm(xyz(previous.EndPoint())-point)),
            curve_tangent_jump_degrees=float(angle(a,b)),inside_owner_normal_jump_degrees=float(angle(*normals))))
    dump(out/'original_corner_audit.json',dict(corners=rows,source='original_region.step',note='Curve derivatives and original incident-face normals at the shared vertex; no candidate normals used.'))
    for row in rows:
        if row['curve_tangent_jump_degrees']>1:print(row,flush=True)


if __name__=='__main__':main()
