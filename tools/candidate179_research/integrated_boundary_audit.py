"""Check both sides of the replacement boundary after whole-model STEP reread."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from integrate import edge_checks


def center(face):
    surface=BRep_Tool.Surface_s(face);a,b,c,d=surface.Bounds()
    return xyz(surface.Value((a+b)/2,(c+d)/2))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True)
    parser.add_argument('--names',nargs='+',required=True);args=parser.parse_args()
    out=Path(args.run)
    for name in args.names:
        folder=out/name;local=shapes(read_step(folder/'patch_faces.step'),TopAbs_FACE)
        whole=read_step(folder/'whole_model_diagnostic.step');faces=shapes(whole,TopAbs_FACE)
        centers=np.array([center(f) for f in faces]);selected=[];matches=[]
        for i,face in enumerate(local):
            distance=np.linalg.norm(centers-center(face),axis=1);index=int(np.argmin(distance))
            if distance[index]>1e-7:raise ValueError('Replacement surface not found after whole STEP readback')
            selected.append(index);matches.append(dict(patch=i,full_face=index,center_distance_mm=float(distance[index])))
        if len(set(selected))!=len(local):raise ValueError('Ambiguous replacement matching')
        uses=edge_uses(faces);selected=set(selected);neighbours=set();outer_count=0
        for edge,indices in uses:
            inside=selected.intersection(indices);outside=set(indices)-selected
            if inside and outside:
                outer_count+=1;neighbours.update(outside)
        checks=edge_checks(compound([faces[i] for i in sorted(selected|neighbours)]),385)
        result=dict(stage='Full integrated STEP reread',patches=len(selected),original_boundary_edges=outer_count,
            neighbour_faces=len(neighbours),matched_faces=matches,checks=checks,
            passed=outer_count==26 and checks['failed_001mm']==0 and checks['same_parameter_false']==0)
        dump(folder/'integrated_boundary_audit.json',result)
        print(name,'passed',result['passed'],'boundary edges',outer_count,'curve-on-surface',checks['max_curve_on_surface_mm'],
            'SameParameter false',checks['same_parameter_false'],'max edge tolerance',checks['maximum_edge_tolerance_mm'],flush=True)


if __name__=='__main__':main()
