"""Rectangular regional charts with independently segmented shared CAD edges.

Topological junctions may lie inside a support side; they need not each force a
new surface corner. Region labels and all source triangles remain unchanged.
Only nondisk regions are split into fitting domains. Cubic boundary curves are
fit once globally, then each domain fits its own tensor-product spline.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import time
import numpy as np
from .mesh import Locator,mean_value,vertex_normals,chain_parameters,dump,stats
from .partition_fit import load_partition,split_faces,loops,boundary_edges
from .partition_fan import disk_domains,FanConfig,fit_curve,half_curve,fit_patch,fit_domain,MeshChart
from .partition_cad import shared_chains
from .compact import curve


def corners_for_loop(v,loop):
    p=v[loop];_,_,axes=np.linalg.svd(p-p.mean(axis=0),full_matrices=False)
    q=(p-p.mean(axis=0))@axes[:2].T;q/=np.maximum(np.ptp(q,axis=0),1e-12)
    direction=np.array([[-1.,-1],[1,-1],[1,1],[-1,1]])
    ids=np.unique(np.argmax(q@direction.T,axis=0))
    if len(ids)!=4:
        t=np.r_[0,np.cumsum(np.linalg.norm(np.diff(np.r_[p,p[:1]],axis=0),axis=1))]
        ids=np.searchsorted(t[:-1],np.arange(4)*t[-1]/4)
        ids=np.unique(np.minimum(ids,len(loop)-1))
        while len(ids)<4:
            gaps=(np.roll(ids,-1)-ids)%len(loop);i=int(np.argmax(gaps))
            ids=np.sort(np.r_[ids,(ids[i]+gaps[i]//2)%len(loop)])
    return np.sort(ids)


class SquareChart:
    def __init__(self,v,f,domain,normals,roundness=0.):
        used=domain['vertices'];self.global_ids=used;self.xyz=v[used]
        self.tri=np.searchsorted(used,f[domain['triangles']]);self.normals=normals[used]
        loop=domain['loop'];corners=domain['corners'];fixed=np.empty((len(loop),2))
        square=np.array([[0.,0.],[1.,0.],[1.,1.],[0.,1.]]);outward=np.array([[0.,-1.],[1.,0.],[0.,1.],[-1.,0.]])
        self.boundaries=[]
        for side in range(4):
            indices=np.arange(corners[side],corners[side+1] if side<3 else len(loop)+corners[0]+1)%len(loop)
            if side<3:indices=np.r_[indices,corners[side+1]]
            points=v[loop[indices]];t=np.r_[0.,np.cumsum(np.linalg.norm(np.diff(points,axis=0),axis=1))];t/=t[-1]
            values=(1-t[:,None])*square[side]+t[:,None]*square[(side+1)%4]+(4*roundness*t*(1-t))[:,None]*outward[side]
            fixed[indices]=values
            self.boundaries.append(lambda s,t=t,values=values:np.stack([np.interp(s,t,values[:,k]) for k in range(2)],axis=1))
        self.uv,residual=mean_value(self.xyz,self.tri,np.searchsorted(used,loop),fixed)
        q=self.uv[self.tri];a=q[:,1]-q[:,0];b=q[:,2]-q[:,0];det=a[:,0]*b[:,1]-a[:,1]*b[:,0]
        if np.min(det)<-1e-12:raise ValueError('Regional square chart contains an inverted triangle')
        good=det>1e-14;area=np.linalg.norm(np.cross(self.xyz[self.tri[:,1]]-self.xyz[self.tri[:,0]],self.xyz[self.tri[:,2]]-self.xyz[self.tri[:,0]]),axis=1)
        self.report=dict(solve_residual=residual,collapsed_boundary_triangles=int((~good).sum()),
                         collapsed_source_area_fraction=float(area[~good].sum()/area.sum()),roundness=roundness)
        if not good.any():raise ValueError('Square chart has no nondegenerate interior')
        self.good_tri=self.tri[good];self.lookup=Locator(np.c_[self.uv,np.zeros(len(used))],self.good_tri)

    def query(self,uv):
        _,distance,ids,bary=self.lookup.closest(np.c_[uv,np.zeros(len(uv))])
        if distance.max()>1e-7:raise ValueError('Query left the regional square chart')
        tri=self.good_tri[ids];xyz=np.einsum('ij,ijk->ik',bary,self.xyz[tri]);normal=np.einsum('ij,ijk->ik',bary,self.normals[tri])
        normal/=np.maximum(np.linalg.norm(normal,axis=1)[:,None],1e-30)
        return xyz,normal


def concatenate(curves,lengths):
    total=sum(lengths);offset=0.;poles=[];knots=[];ranges=[]
    for ci,(c,length) in enumerate(zip(curves,lengths)):
        p=np.array([c.Pole(i).Coord() for i in range(1,c.NbPoles()+1)])
        t=np.repeat([c.Knot(i) for i in range(1,c.NbKnots()+1)],[c.Multiplicity(i) for i in range(1,c.NbKnots()+1)])
        start=offset/total;offset+=length;end=offset/total;t=start+(end-start)*t;ranges.append((start,end))
        if ci:
            if np.linalg.norm(poles[-1]-p[0])>1e-8:raise ValueError('Disconnected compound regional side')
            poles.extend(p[1:]);knots=knots[:-1]+t[4:].tolist()
        else:poles=p.tolist();knots=t.tolist()
    knots[0:4]=[0.]*4;knots[-4:]=[1.]*4
    return curve(np.asarray(poles),np.asarray(knots)),ranges


def curve_polyline_error(poles,knots,points,parameters):
    """Inspect extrema of squared distance on each cubic/linear span."""
    from scipy.interpolate import BSpline
    from numpy.polynomial.chebyshev import chebvander,chebmul,chebder,chebroots,chebval
    degree=len(knots)-len(poles)-1
    cuts=np.unique(np.r_[parameters,knots]);nodes=np.cos(np.arange(degree+1)*np.pi/degree)
    t=cuts[:-1,None]+np.diff(cuts)[:,None]*(nodes+1)/2
    xyz=BSpline(knots,poles,degree)(t.ravel())
    target=np.stack([np.interp(t.ravel(),parameters,points[:,k]) for k in range(3)],axis=1)
    coeff=np.linalg.solve(chebvander(nodes,degree),(xyz-target).reshape(-1,degree+1,3).transpose(1,0,2).reshape(degree+1,-1))
    coeff=coeff.reshape(degree+1,-1,3);maximum=-1.;where=0.
    for i in range(len(cuts)-1):
        squared=np.zeros(2*degree+1)
        for k in range(3):
            value=chebmul(coeff[:,i,k],coeff[:,i,k]);squared[:len(value)]+=value
        roots=chebroots(chebder(squared));roots=roots.real[(abs(roots.imag)<1e-8)&(abs(roots.real)<=1)]
        locations=np.r_[-1.,1.,roots];values=chebval(locations,squared);j=int(np.argmax(values))
        if values[j]>maximum:
            maximum=float(values[j]);where=float(cuts[i]+(locations[j]+1)*(cuts[i+1]-cuts[i])/2)
    return np.sqrt(max(0.,maximum)),where


def rounded_polyline(points,parameters,tolerance):
    """Quintic corner rounding with C2 joins and bounded local movement.

    Every corner uses equal physical cut lengths on its two incident edges.
    All Bezier controls lie on those original segments. End velocities match
    arc-length straight pieces and end accelerations vanish exactly.
    """
    from OCP.Geom import Geom_BSplineCurve
    from OCP.TColgp import TColgp_Array1OfPnt
    from OCP.gp import gp_Pnt
    from .partition_cad import array
    length=np.linalg.norm(np.diff(points,axis=0),axis=1);total=length.sum()
    cut=np.zeros(len(points));cut[1:-1]=np.minimum(.45*tolerance,.2*np.minimum(length[:-1],length[1:]))
    pieces=[];breaks=[0.]
    for i in range(len(points)-1):
        direction=(points[i+1]-points[i])/length[i]
        start=points[i]+cut[i]*direction;end=points[i+1]-cut[i+1]*direction
        pieces.append(start+np.linspace(0,1,6)[:,None]*(end-start));breaks.append(parameters[i+1]-cut[i+1]/total)
        if i+1<len(points)-1:
            j=i+1;following=(points[j+1]-points[j])/length[j];d=cut[j]
            a=points[j]-d*direction;b=points[j]+d*following
            pieces.append(np.array([a,a+.4*d*direction,a+.8*d*direction,
                                     b-.8*d*following,b-.4*d*following,b]))
            breaks.append(parameters[j]+d/total)
    poles=np.concatenate([pieces[0]]+[p[1:] for p in pieces[1:]])
    ps=TColgp_Array1OfPnt(1,len(poles))
    for i,p in enumerate(poles):ps.SetValue(i+1,gp_Pnt(*p))
    multi=np.full(len(breaks),5);multi[[0,-1]]=6
    result=Geom_BSplineCurve(ps,array(breaks),array(multi,True),5)
    for i in range(result.NbKnots()-1,1,-1):
        if not result.RemoveKnot(i,3,1e-10):
            raise ValueError('Cannot retain C2 rounded-polyline representation')
    cp=np.array([result.Pole(i).Coord() for i in range(1,result.NbPoles()+1)])
    kt=np.repeat([result.Knot(i) for i in range(1,result.NbKnots()+1)],
                 [result.Multiplicity(i) for i in range(1,result.NbKnots()+1)])
    error,_=curve_polyline_error(cp,kt,points,parameters)
    if error>tolerance:raise ValueError('Rounded polyline exceeds its movement budget')
    return result,float(error)


def compact_curve(points,parameters,tolerance,end_normals=None,allow_rounded=False):
    from .splines import basis
    fractions=np.linspace(0,1,5)
    breaks=[0.,1.];extra=[]
    for _ in range(96):
        cuts=np.unique(np.r_[parameters,breaks])
        t=np.unique(np.r_[(cuts[:-1,None]*(1-fractions)+cuts[1:,None]*fractions).ravel(),extra])
        xyz=np.stack([np.interp(t,parameters,points[:,k]) for k in range(3)],axis=1)
        knots=np.r_[np.zeros(3),breaks,np.ones(3)];a=basis(knots,t)
        p=np.zeros((len(knots)-4,3));p[[0,-1]]=points[[0,-1]]
        p[1:-1]=np.linalg.lstsq(a[:,1:-1],xyz-a@p,rcond=1e-12)[0]
        if end_normals is not None:
            # Both incident regions use this single curve. Endpoint tangents
            # lie in the common source tangent plane at graph junctions.
            h=a[:,1:-1].T@a[:,1:-1]+np.eye(len(p)-2)*1e-14
            c=np.zeros((2,3,len(p)-2));c[0,:,0]=end_normals[0];c[1,:,-1]=end_normals[1]
            inverse=np.stack([np.linalg.solve(h,c[:,k,:].T) for k in range(3)],axis=0).reshape(-1,2)
            c=c.reshape(2,-1);target=np.sum(points[[0,-1]]*end_normals,axis=1)
            correction=inverse@np.linalg.lstsq(c@inverse,c@p[1:-1].T.ravel()-target,rcond=1e-12)[0]
            p[1:-1]-=correction.reshape(3,-1).T
        residual=np.linalg.norm(a@p-xyz,axis=1);error=float(residual.max())
        if error<=tolerance*.95:
            exact,location=curve_polyline_error(p,knots,points,parameters)
            if exact<=tolerance:return curve(p,knots),float(exact)
            extra.append(location);continue
        index=int(np.argmax(residual));value=t[index];interval=int(np.clip(np.searchsorted(breaks,value)-1,0,len(breaks)-2))
        candidates=[(breaks[interval]+breaks[interval+1])/2]
        if interval+2<len(breaks) and abs(value-breaks[interval+1])<1e-10:
            candidates.append((breaks[interval+1]+breaks[interval+2])/2)
        breaks=sorted(breaks+candidates)
    if end_normals is None and allow_rounded:
        return rounded_polyline(points,parameters,tolerance)
    result,_=fit_curve(points,parameters,tolerance,end_normals=end_normals)
    cp=np.array([result.Pole(i).Coord() for i in range(1,result.NbPoles()+1)])
    kt=np.repeat([result.Knot(i) for i in range(1,result.NbKnots()+1)],
                 [result.Multiplicity(i) for i in range(1,result.NbKnots()+1)])
    error,_=curve_polyline_error(cp,kt,points,parameters)
    if error>tolerance:raise ValueError('Endpoint-constrained curve exceeds its continuous movement budget')
    return result,float(error)


def bounded_domains(v,f,labels,maximum):
    from scipy import sparse
    domains,assigned,edges,owners=disk_domains(v,f,labels)
    if maximum<=0:return domains,assigned,edges,owners
    graph=sparse.coo_matrix((np.ones(owners.size),(owners.ravel(),owners[:,::-1].ravel())),shape=(len(f),len(f))).tocsr()
    queue=[(d['triangles'],d['region'],0) for d in domains];result=[];assigned=np.full(len(f),-1,dtype=int)
    while queue:
        ids,region,depth=queue.pop(0);used=np.unique(f[ids]);boundary=None
        try:
            found=loops(boundary_edges(np.searchsorted(used,f[ids])))
            if len(found)==1:boundary=used[found[0]]
        except ValueError:pass
        if len(ids)>maximum or boundary is None:
            if depth>=24 or len(ids)<2:raise ValueError('Cannot obtain bounded regional disk charts')
            queue.extend((part,region,depth+1) for part in split_faces(v,f,ids,graph));continue
        assigned[ids]=len(result)
        result.append(dict(region=int(region),triangles=ids,vertices=used,loop=boundary,
                           perimeter=float(np.linalg.norm(v[np.roll(boundary,-1)]-v[boundary],axis=1).sum())))
    if np.any(assigned<0):raise ValueError('Regional chart subdivision lost source triangles')
    return result,assigned,edges,owners


def reconstruct(partition,stl,output,config,max_domain_triangles=0,roundness=0.,compact_curves=False,endpoint_tangents=False):
    out=Path(output);out.mkdir(parents=True,exist_ok=False);(out/'patches').mkdir();start=time.perf_counter()
    report=dict(status='running',algorithm='segmented-boundary-regional-rectangle-v1',source_CAD_read=False,
                commercial_engine_used=False,partition=str(Path(partition).resolve()),stl=str(Path(stl).resolve()),
                config=asdict(config),units='mm',max_domain_triangles=max_domain_triangles,
                roundness=roundness,compact_curves=compact_curves,endpoint_tangents=endpoint_tangents)
    dump(out/'run.json',report)
    try:
        v,f,labels,topology=load_partition(partition,stl);normal=vertex_normals(v,f)
        domains,assigned,edges,owners=bounded_domains(v,f,labels,max_domain_triangles)
        corner_vertices=set()
        for d in domains:
            if len(d['loop'])>=4:
                d['corners']=corners_for_loop(v,d['loop']);corner_vertices.update(d['loop'][d['corners']])
        original,adjacent=shared_chains(edges,owners,assigned);chains=[];chain_owners=[]
        for chain,pair in zip(original,adjacent):
            cuts=np.r_[0,np.flatnonzero(np.isin(chain[1:-1],list(corner_vertices)))+1,len(chain)-1]
            for a,b in zip(cuts[:-1],cuts[1:]):chains.append(chain[a:b+1]);chain_owners.append(pair)
        boundary=[];lookup={};errors=[];graph_curves={};endpoints={}
        for ci,chain in enumerate(chains):
            fitter=compact_curve if compact_curves else fit_curve
            normals=normal[chain[[0,-1]]] if endpoint_tangents else None
            c,error=fitter(v[chain],chain_parameters(v,chain),config.curve_tolerance,end_normals=normals)
            boundary.append(c);errors.append(error)
            for a,b in zip(chain[:-1],chain[1:]):lookup[tuple(sorted((int(a),int(b))))]=(ci,int(a))
            for half in (0,1):
                key=('boundary',ci,'half',half);graph_curves[key]=half_curve(c,half*.5,(half+1)*.5)
                endpoints[key]=(('mesh',int(chain[0])),('mid',ci)) if half==0 else (('mid',ci),('mesh',int(chain[-1])))
        expected=sum(1 if 'corners' in d else 3 for d in domains)
        if expected>config.max_patches:raise ValueError('Regional domain count exceeds the face budget')
        report.update(input_topology=topology,regions=len(np.unique(labels)),domains=len(domains),shared_curves=len(chains),
                      boundary_distance_mm=stats(errors),predicted_patches=expected)
        dump(out/'run.json',report);print(dict(stage='prepared',domains=len(domains),patches=expected,curves=len(chains)),flush=True)
        patches=[];records=[]
        for di,d in enumerate(domains):
            if 'corners' not in d:
                d['chart']=MeshChart(v,f,d)
                fitted,curves,ends,reports=fit_domain(v,f,d,di,chains,boundary,lookup,config)
                graph_curves.update(curves);endpoints.update(ends)
                for p,r in zip(fitted,reports):
                    i=len(patches);patches.append(p);records.append(r)
                    np.savez_compressed(out/'patches'/f'{i:05d}.npz',poles=p['poles'],tu=p['tu'],tv=p['tv'],region=p['region'],domain=di,orientation=p['orientation'])
                continue
            chart=SquareChart(v,f,d,normal,roundness);loop=d['loop'];sides=[];side_refs=[];side_lengths=[]
            for side in range(4):
                first=int(d['corners'][side]);last=int(d['corners'][(side+1)%4]);last=last if last>first else last+len(loop)
                segment=loop[np.arange(first,last+1)%len(loop)];refs=[]
                for a,b in zip(segment[:-1],segment[1:]):
                    ci,av=lookup[tuple(sorted((int(a),int(b))))];ref=(ci,int(a)==av)
                    if not refs or refs[-1]!=ref:refs.append(ref)
                curves=[];lengths=[]
                for ci,forward in refs:
                    c=boundary[ci].Copy()
                    if not forward:c.Reverse()
                    curves.append(c);lengths.append(float(np.linalg.norm(np.diff(v[chains[ci]],axis=0),axis=1).sum()))
                joined,ranges=concatenate(curves,lengths);sides.append(joined);side_refs.append((refs,ranges))
            uv_curves=chart.boundaries
            poles,tu,tv,sign,mappings,record=fit_patch(sides,uv_curves,chart,config,out/'failed_fit_inputs.npz')
            curve_keys=[];directions=[];segmented_mapping=[]
            for mapping,(refs,ranges) in zip(mappings,side_refs):
                for (ci,forward),(a,b) in zip(refs,ranges):
                    for position,half in enumerate([0,1] if forward else [1,0]):
                        curve_keys.append(('boundary',ci,'half',half));directions.append(forward)
                        segmented_mapping.append({**mapping,'range':[a+(b-a)*position/2,a+(b-a)*(position+1)/2]})
            patch=dict(poles=poles,tu=tu,tv=tv,orientation=sign,curves=curve_keys,directions=directions,
                       mappings=segmented_mapping,region=d['region'],domain=di)
            i=len(patches);patches.append(patch);records.append(dict(region=d['region'],domain=di,chart=chart.report,**record))
            np.savez_compressed(out/'patches'/f'{i:05d}.npz',poles=poles,tu=tu,tv=tv,region=d['region'],domain=di,orientation=sign)
            dump(out/'fitting_progress.json',dict(completed_domains=di+1,total_domains=len(domains),patches=len(patches),records=records))
            if di%20==0:print(dict(stage='fit',domain=di,patches=len(patches),seconds=time.perf_counter()-start),flush=True)
        np.savez_compressed(out/'source_domains.npz',face_domain=assigned,source_labels=labels,domain_region=np.array([d['region'] for d in domains]))
        dump(out/'fitting.json',dict(patches=records,complete_source_triangle_coverage=True,partition_labels_unchanged=True))
        from .partition_fan_cad import assemble
        report.update(assemble(patches,graph_curves,endpoints,config.construction_tolerance,out),status='DiagnosticOnly',can_apply=False)
    except Exception as exc:report.update(status='failed',error=f'{type(exc).__name__}: {exc}');raise
    finally:report['wall_seconds']=time.perf_counter()-start;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--partition',required=True);p.add_argument('--stl',required=True);p.add_argument('--output',required=True)
    p.add_argument('--controls',type=int,default=13);p.add_argument('--curve-tolerance',type=float,default=.005)
    p.add_argument('--samples',type=int,default=625);p.add_argument('--face-limit',type=int,required=True)
    p.add_argument('--max-domain-triangles',type=int,default=1024)
    p.add_argument('--roundness',type=float,default=0.);p.add_argument('--compact-curves',action='store_true')
    p.add_argument('--endpoint-tangents',action='store_true');p.add_argument('--full-interior',action='store_true')
    p.add_argument('--absolute-fairness',action='store_true');p.add_argument('--fairness',type=float,default=1e-5)
    p.add_argument('--normal-weight',type=float,default=.01);p.add_argument('--seam-weight',type=float,default=.05);a=p.parse_args()
    reconstruct(a.partition,a.stl,a.output,FanConfig(interior_controls=a.controls,curve_tolerance=a.curve_tolerance,
                samples_per_patch=a.samples,max_patches=a.face_limit,normal_weight=a.normal_weight,seam_weight=a.seam_weight,
                fairness=a.fairness,interior_mode='full' if a.full_interior else 'coarse_correction',
                regularize_correction=not a.absolute_fairness),a.max_domain_triangles,a.roundness,a.compact_curves,a.endpoint_tangents)
