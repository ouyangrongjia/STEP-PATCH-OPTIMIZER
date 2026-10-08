"""Improve internal triangle shape without changing supplied regional seams."""
import numpy as np
from .partition_fit import incidence
from .intersections import self_intersections
from .mesh import Locator,stats


def triangle_quality(v,f):
    p=v[f];cross=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);area=np.linalg.norm(cross,axis=1)
    lengths=np.sum((p-np.roll(p,-1,axis=1))**2,axis=(1,2))
    return 2*np.sqrt(3)*area/np.maximum(lengths,1e-30),cross/np.maximum(area[:,None],1e-30)


def improve(v,f,labels,source_v,source_f,error_mm,sweeps=12):
    f=f.copy();reference=Locator(source_v,source_f);history=[];before=stats(triangle_quality(v,f)[0])
    bary=np.array([[1/3]*3,[.6,.2,.2],[.2,.6,.2],[.2,.2,.6]])
    for iteration in range(sweeps):
        edges,owners=incidence(f);quality,normals=triangle_quality(v,f)
        keys=set(map(tuple,edges));candidates=[]
        for key,(x,y) in zip(edges,owners):
            if labels[x]!=labels[y] or normals[x]@normals[y]<.8:continue
            tri=f[x];k=next(j for j in range(3) if tri[j] not in key)
            c,a,b=tri[k],tri[(k+1)%3],tri[(k+2)%3];d=next(i for i in f[y] if i not in key)
            if tuple(sorted((c,d))) in keys:continue
            pair=np.array([[c,a,d],[c,d,b]]);q,n=triangle_quality(v,pair)
            old=min(quality[x],quality[y]);new=q.min()
            if new>old*1.1 and np.min(n@(normals[x]+normals[y])/2)>.7:
                candidates.append((new-old,int(x),int(y),pair))
        if not candidates:break
        candidates.sort(key=lambda c:c[0],reverse=True);selected=[];used=set()
        for score,x,y,pair in candidates:
            if x in used or y in used:continue
            selected.append((x,y,pair));used.update((x,y))
        sample=np.einsum('sj,ntjc->ntsc',bary,v[np.array([p for x,y,p in selected])])
        _,distance,_,_=reference.closest(sample.reshape(-1,3));good=distance.reshape(len(selected),-1).max(axis=1)<=error_mm
        saved=f.copy();accepted=[]
        for (x,y,pair),ok in zip(selected,good):
            if ok:f[[x,y]]=pair;accepted.append((x,y))
        bad=set(self_intersections(v,f));rejected=0
        for x,y in accepted:
            if x in bad or y in bad:f[[x,y]]=saved[[x,y]];rejected+=1
        if bad and len(self_intersections(v,f)):
            f=saved;rejected=len(accepted)
        history.append(dict(iteration=iteration,candidates=len(candidates),accepted=len(accepted)-rejected,rejected_collisions=rejected))
        if len(accepted)==rejected:break
    return f,dict(history=history,triangle_quality_before=before,triangle_quality_after=stats(triangle_quality(v,f)[0]),vertices_moved=0,region_boundaries_changed=False)
