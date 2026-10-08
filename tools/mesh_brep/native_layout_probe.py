"""Measure fixed-region coarse layout costs before any surface fitting."""
import argparse
from pathlib import Path
import time
import numpy as np
from .partition_fit import load_partition
from .partition_network import simplify
from .regional_reconstruct import quad_layout
from .mesh import dump


def probe(partition,stl,output,errors,face_limit,anchors='triangle'):
    root=Path(output);root.mkdir(parents=True,exist_ok=False)
    v,f,labels,topo=load_partition(partition,stl)
    summary=dict(partition=str(Path(partition).resolve()),stl=str(Path(stl).resolve()),face_limit=face_limit,
                 source_CAD_read=False,commercial_engine_used=False,runs=[])
    dump(root/'run.json',summary)
    for error in errors:
        out=root/f'error_{error:g}';out.mkdir();start=time.perf_counter()
        try:
            p,t,r,ids,report=simplify(v,f,labels,error,face_limit,anchors=anchors)
            layout=quad_layout(p,t,r)
            np.savez_compressed(out/'network.npz',vertices=p,triangles=t,regions=r,source_ids=ids)
            dump(out/'layout.json',layout)
            record=dict(error_mm=error,triangles=len(t),patches=len(layout),quads=sum(len(x['vertices'])==4 for x in layout),
                        triangles_in_layout=sum(len(x['vertices'])==3 for x in layout),within_face_limit=len(layout)<=face_limit,
                        simplification=report,seconds=time.perf_counter()-start,status='layout_only')
        except Exception as e:record=dict(error_mm=error,status='failed',error=f'{type(e).__name__}: {e}',seconds=time.perf_counter()-start)
        dump(out/'run.json',record);summary['runs'].append(record);dump(root/'run.json',summary)
        print({k:v for k,v in record.items() if k!='simplification'},flush=True)
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--partition',required=True);p.add_argument('--stl',required=True)
    p.add_argument('--output',required=True);p.add_argument('--errors',type=float,nargs='+',default=[.2,.4,.8])
    p.add_argument('--face-limit',type=int,required=True);a=p.parse_args()
    probe(a.partition,a.stl,a.output,a.errors,a.face_limit)
