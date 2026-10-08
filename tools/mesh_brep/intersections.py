"""VCGLib candidates with tolerance-aware narrow-phase confirmation.

The native filter can flag coplanar subdivision triangles touching along their
shared edges after floating-point interpolation. Confirm positive-length
plane intersections or positive-area coplanar overlap; declared tolerance is
smaller than the CAD construction tolerance. This is not an exact-arithmetic
certificate for the continuous fitted surfaces.
"""
import numpy as np
from numba import njit


@njit(cache=True)
def cross2(a,b):
    return a[0]*b[1]-a[1]*b[0]


@njit(cache=True)
def coplanar_overlap(a,b,normal,tolerance):
    drop=np.argmax(np.abs(normal));axis0=(drop+1)%3;axis1=(drop+2)%3
    clip=np.empty((3,2));polygon=np.zeros((12,2));count=3
    for i in range(3):
        clip[i,0]=a[i,axis0];clip[i,1]=a[i,axis1]
        polygon[i,0]=b[i,axis0];polygon[i,1]=b[i,axis1]
    sign=1. if cross2(clip[1]-clip[0],clip[2]-clip[0])>=0 else -1.
    for side in range(3):
        p=clip[side];edge=clip[(side+1)%3]-p
        result=np.zeros((12,2));number=0
        for i in range(count):
            x=polygon[i];y=polygon[(i+1)%count]
            dx=sign*cross2(edge,x-p);dy=sign*cross2(edge,y-p)
            if dx>=0:
                result[number]=x;number+=1
            if (dx>=0)!=(dy>=0):
                result[number]=x+(y-x)*(dx/(dx-dy));number+=1
        polygon=result;count=number
        if count<3:return False
    area=0.
    for i in range(count):area+=cross2(polygon[i]-polygon[0],polygon[(i+1)%count]-polygon[0])
    scale=max(np.linalg.norm(a[1]-a[0]),np.linalg.norm(a[2]-a[0]),np.linalg.norm(b[1]-b[0]),np.linalg.norm(b[2]-b[0]))
    return abs(area)*.5>tolerance*scale


@njit(cache=True)
def plane_interval(tri,d,line,tolerance):
    lo=1e100;hi=-1e100
    for i in range(3):
        j=(i+1)%3
        if abs(d[i])<=tolerance:
            value=np.dot(tri[i],line);lo=min(lo,value);hi=max(hi,value)
        if d[i]*d[j]<0 and abs(d[i])>tolerance and abs(d[j])>tolerance:
            point=tri[i]+(tri[j]-tri[i])*d[i]/(d[i]-d[j]);value=np.dot(point,line)
            lo=min(lo,value);hi=max(hi,value)
    return lo,hi


@njit(cache=True)
def confirm_pairs(v,f,pairs,tolerance):
    result=np.zeros(len(pairs),np.bool_)
    for k in range(len(pairs)):
        ia,ib=pairs[k];a=v[f[ia]];b=v[f[ib]]
        na=np.cross(a[1]-a[0],a[2]-a[0]);nb=np.cross(b[1]-b[0],b[2]-b[0])
        la=np.linalg.norm(na);lb=np.linalg.norm(nb)
        if la<1e-30 or lb<1e-30:continue
        na/=la;nb/=lb
        da=(a-b[0])@nb;db=(b-a[0])@na
        if np.min(da)>tolerance or np.max(da)<-tolerance or np.min(db)>tolerance or np.max(db)<-tolerance:continue
        if np.max(np.abs(da))<=tolerance and np.max(np.abs(db))<=tolerance:
            result[k]=coplanar_overlap(a,b,na,tolerance);continue
        shared=0
        for x in a:
            for y in b:
                if np.linalg.norm(x-y)<=tolerance:
                    shared+=1;break
        if shared>=2:continue
        line=np.cross(na,nb);length=np.linalg.norm(line)
        if length<1e-14:continue
        line/=length
        a0,a1=plane_interval(a,da,line,tolerance);b0,b1=plane_interval(b,db,line,tolerance)
        result[k]=min(a1,b1)-max(a0,b0)>tolerance
    return result


def self_intersections(vertices, triangles, tolerance=1e-9,all_candidates=False,return_pairs=False):
    empty=np.empty((0,2),dtype=int) if return_pairs else np.array([],dtype=int)
    if all_candidates:
        selected=np.arange(len(triangles))
    else:
        import pymeshlab
        mesh=pymeshlab.MeshSet()
        mesh.add_mesh(pymeshlab.Mesh(np.asarray(vertices,dtype=float),np.asarray(triangles,dtype=np.int32)))
        mesh.apply_filter('compute_selection_by_self_intersections_per_face')
        selected=np.flatnonzero(mesh.current_mesh().face_selection_array())
    if not len(selected):return empty
    from .mesh import Locator
    import vtk
    local_faces=triangles[selected];locator=Locator(vertices,local_faces)
    ids=vtk.vtkIdList();pairs=[]
    for local,fi in enumerate(selected):
        p=vertices[triangles[fi]];low=p.min(axis=0)-tolerance;high=p.max(axis=0)+tolerance
        bounds=np.stack([low,high],axis=1).ravel()
        locator.locator.FindCellsWithinBounds(bounds,ids)
        for j in range(ids.GetNumberOfIds()):
            other=ids.GetId(j)
            if other>local:pairs.append((fi,selected[other]))
    if not pairs:return empty
    pairs=np.asarray(pairs,dtype=np.int64)
    confirmed=confirm_pairs(vertices,triangles,pairs,tolerance)
    return pairs[confirmed] if return_pairs else np.unique(pairs[confirmed])
