"""Fresh reverse-area samples after final candidate selection, with CAD spot checks."""
import argparse
from pathlib import Path
import numpy as np
from geometry import *
from diagnose import reverse_distance


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    data=np.load(out/'reference.npz');original=read_step(out/'original_region.step');result={}
    for name in ['control','aligned_inner_cubic']:
        report,p,d=reverse_distance(read_step(out/name/'patch_faces.step'),original,data,count=12000,seed=9180181)
        difference=max(abs(c['numeric_mm']-c['occt_mm']) for c in report['occt_global_checks'])
        report['max_numeric_vs_trimmed_kernel_disagreement_mm']=difference
        if difference>1e-6:raise ValueError('Unresolved reverse projection discrepancy')
        np.savez_compressed(out/name/'reverse_confirmation.npz',points=p,distance=d);result[name]=report
        print(name,'fresh reverse',report['distance_mm'],'kernel disagreement',difference,flush=True)
    dump(out/'reverse_confirmation.json',result)


if __name__=='__main__':main()
