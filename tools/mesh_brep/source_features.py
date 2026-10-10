"""Split support domains at source creases without changing region ownership."""
import argparse
import json
import shutil
from pathlib import Path
import numpy as np
from scipy import sparse
from .partition_fit import incidence
from .graph_repair import feature_components
from .mesh import dump


def split(source, output, regions=None, angle=None):
    root,out=Path(source),Path(output); out.mkdir(parents=True,exist_ok=False)
    meta=json.loads((root/'run.json').read_text(encoding='utf-8'))
    angle=meta['config']['feature_angle'] if angle is None else float(angle)
    if not np.isfinite(angle) or not 0 < angle < 180:
        raise ValueError('Source crease angle must be between zero and 180 degrees')
    with np.load(meta['partition']) as data:
        v,f,labels=[data[k] for k in ('mesh_vertex_original','mesh_triangle','face_instance_id')]
    with np.load(root/'chart_index.npz') as archive:
        before=archive['face_chart']
    after=before.copy()
    selected=meta.get('partial_regions') if regions is None else regions
    if selected is not None and not set(selected).issubset(set(labels)):
        raise ValueError('Requested source regions are absent')
    active=np.unique(before if selected is None else before[np.isin(labels,selected)])
    _,owners=incidence(f)
    n=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]]); n/=np.linalg.norm(n,axis=1)[:,None]
    dot=np.sum(n[owners[:,0]]*n[owners[:,1]],axis=1)
    graph=sparse.coo_matrix((np.repeat(2+dot,2),(owners.ravel(),owners[:,::-1].ravel())),shape=(len(f),)*2).tocsr()
    threshold=np.cos(np.radians(angle)); next_id=int(before.max())+1; changes=[]
    for di in active:
        ids=np.flatnonzero(before==di)
        parts=feature_components(ids,graph,threshold)
        if len(parts)==1: continue
        parts.sort(key=len,reverse=True)
        new_ids=[int(di)]
        for part in parts[1:]:
            after[part]=next_id; new_ids.append(next_id); next_id+=1
        changes.append(dict(source_chart=int(di),source_region=int(labels[ids[0]]),
                            charts=new_ids,triangle_counts=list(map(len,parts))))
    relevant=np.isin(before[owners[:,0]],active)&(before[owners[:,0]]==before[owners[:,1]])&(dot<threshold)
    if np.any(after[owners[relevant,0]]==after[owners[relevant,1]]):
        raise ValueError('A source crease still lies inside a selected support domain')
    for name in ('charts','curves'): shutil.copytree(root/name,out/name)
    shutil.copy2(root/'fitting.json',out/'fitting.json')
    np.savez_compressed(out/'chart_index.npz',face_chart=after,source_labels=labels)
    report=dict(source_fit=str(root.resolve()),angle_degrees=angle,source_crease_edges=int(relevant.sum()),
                supports_before=int(len(np.unique(before))),supports_after=int(len(np.unique(after))),
                changed=changes,supplied_labels_preserved=True,source_coordinates_unchanged=True,
                complete_source_triangle_coverage=True,source_CAD_read=False,commercial_engine_used=False)
    dump(out/'source_feature_layout.json',report)
    meta.update(status='layout_candidate',source_fit=str(out.resolve()),requires_support_refit=bool(changes),
                required_refit_regions=sorted(set(c['source_region'] for c in changes)),
                source_feature_layout=report,failed_charts=[])
    dump(out/'run.json',meta)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--run',required=True); p.add_argument('--output',required=True)
    p.add_argument('--regions',type=int,nargs='+'); p.add_argument('--angle',type=float)
    a=p.parse_args(); print(split(a.run,a.output,a.regions,a.angle))
