"""Per-face native STEP sampling, independent of global area-only statistics."""
import argparse
from pathlib import Path
import numpy as np
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.TopAbs import TopAbs_FACE, TopAbs_IN, TopAbs_ON
from OCP.gp import gp_Pnt2d
from . import cad
from .mesh import Locator, dump, stats
from .source_normals import crease_fan_normals
from .step_quality import tessellate, native


def inside_trim(face, q):
    # OCCT's exact classifier supports conservative bounding-box rejection.
    # It avoids intersecting every spline edge for every interior sample.
    return np.array([BRepClass_FaceClassifier(face, gp_Pnt2d(*x), 1e-9, True).State()
                     in (TopAbs_IN, TopAbs_ON) for x in q])


def audit(step, partition, output, samples_per_face=1024, seed=810572):
    if samples_per_face < 1:
        raise ValueError('At least one sample per face is required')
    out=Path(output)
    if out.exists(): raise FileExistsError(out)
    data=np.load(partition)
    v,f=data['mesh_vertex_original'],data['mesh_triangle']
    locator=Locator(v,f);corner=crease_fan_normals(v,f)
    flat=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
    flat/=np.linalg.norm(flat,axis=1)[:,None]
    shape=cad.read_generated(step);faces=cad.shapes(shape,TopAbs_FACE)
    mesh,surfaces,signs=tessellate(shape,.001)
    tri=mesh['points'][mesh['triangles']]
    area=np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1)/2
    rows=[];all_distance=[];all_angle=[];all_flat=[];worst=[]
    for fi,face in enumerate(faces):
        ids=np.flatnonzero(mesh['owners']==fi);rng=np.random.default_rng(seed+fi)
        choices=rng.choice(ids,size=samples_per_face,p=area[ids]/area[ids].sum())
        u=rng.random((len(choices),2));s=np.sqrt(u[:,0]);b=np.c_[1-s,s*(1-u[:,1]),s*u[:,1]]
        q=np.einsum('ij,ijk->ik',b,mesh['uv'][mesh['triangles'][choices]])
        inside=inside_trim(face,q)
        q=q[inside]
        if not len(q): raise ValueError(f'Face {fi} has no verified interior samples')
        xyz,n,_,_=native(surfaces[fi],q,signs[fi]);_,distance,nearest,bary=locator.closest(xyz)
        target=np.einsum('ij,ijk->ik',bary,corner[nearest]);target/=np.linalg.norm(target,axis=1)[:,None]
        angle=np.degrees(np.arccos(np.clip(np.sum(n*target,axis=1),-1,1)))
        flat_angle=np.degrees(np.arccos(np.clip(np.sum(n*flat[nearest],axis=1),-1,1)))
        k=int(np.argmax(angle));j=int(np.argmax(distance))
        witnesses=[dict(face=fi,uv=q[h].tolist(),xyz=xyz[h].tolist(),distance_mm=float(distance[h]),
            normal_degrees=float(angle[h]),flat_normal_degrees=float(flat_angle[h]),
            source_triangle=int(nearest[h]),source_barycentric=bary[h].tolist()) for h in (k,j)]
        worst.extend(witnesses[:1] if k == j else witnesses)
        rows.append(dict(face=fi,samples=len(q),outside_trim_rejected=int((~inside).sum()),
            worst_distance=witnesses[1],
            source_distance_mm=stats(distance),source_fan_normal_degrees=stats(angle),
            source_flat_normal_degrees=stats(flat_angle),over_30=int((angle>30).sum()),over_90=int((angle>90).sum())))
        all_distance.append(distance);all_angle.append(angle);all_flat.append(flat_angle)
    angles=np.concatenate(all_angle)
    report=dict(step=str(Path(step).resolve()),partition=str(Path(partition).resolve()),seed=seed,
        requested_samples_per_face=samples_per_face,faces=len(faces),samples=len(angles),
        minimum_verified_samples_per_face=min(r['samples'] for r in rows),
        source_distance_mm=stats(np.concatenate(all_distance)),source_fan_normal_degrees=stats(angles),
        source_flat_normal_degrees=stats(np.concatenate(all_flat)),
        over_30=int((angles>30).sum()),over_45=int((angles>45).sum()),over_90=int((angles>90).sum()),
        worst_normals=sorted(worst,key=lambda x:-x['normal_degrees'])[:24],
        worst_distances=sorted(worst,key=lambda x:-x['distance_mm'])[:24],per_face=rows,
        trim_classifier='BRepClass_FaceClassifier, tolerance=1e-9, UseBndBox=true',
        scope='Stratified finite samples on actual trimmed STEP. Statistics are per-face balanced, not area weighted. '
              'Source normals use unfiltered crease-separated fans; flat normals are reported separately. '
              'Not a Hausdorff, continuity, or self-intersection certificate.')
    dump(out,report);return report


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('step','partition','output'):p.add_argument('--'+name,required=True)
    p.add_argument('--samples-per-face',type=int,default=1024)
    p.add_argument('--seed',type=int,default=810572)
    a=p.parse_args();r=audit(a.step,a.partition,a.output,a.samples_per_face,a.seed)
    print({k:v for k,v in r.items() if k not in ('per_face','worst_normals','worst_distances')})
