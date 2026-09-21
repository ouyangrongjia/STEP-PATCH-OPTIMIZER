"""New five-patch layout with exact original outer edges and variable shared seams."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy import sparse
from scipy.interpolate import LinearNDInterpolator
from scipy.sparse.linalg import splu
from geometry import *
from spline import array1, knots, basis, tensor, from_occt
from hard_boundary import operators, quadrature, parameterize
from integrate import edge_checks
from diagnose import seam_quality
from boundary_audit import normals_on_edge
from evaluate import differential
from OCP.Geom import Geom_TrimmedCurve, Geom_BSplineCurve
from OCP.Geom2d import Geom2d_BSplineCurve
from OCP.GeomConvert import GeomConvert, GeomConvert_CompCurveToBSplineCurve
from OCP.GeomFill import GeomFill_BSplineCurves, GeomFill_CoonsStyle
from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve
from OCP.TColgp import TColgp_Array1OfPnt, TColgp_Array1OfPnt2d
from OCP.ShapeAnalysis import ShapeAnalysis_FreeBounds
from OCP.BRepTools import BRepTools_WireExplorer
from OCP.TopAbs import TopAbs_WIRE, TopAbs_FORWARD
from OCP.BRepBuilderAPI import BRepBuilderAPI_Copy, BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeVertex
from OCP.TopoDS import TopoDS_Shell
from OCP.gp import gp_Pnt2d


def original_loop(region):
    wires=shapes(ShapeAnalysis_FreeBounds(region,1e-7,False,False).GetClosedWires(),TopAbs_WIRE)
    if len(wires)!=1:raise ValueError('Expected one original boundary')
    explorer=BRepTools_WireExplorer(TopoDS.Wire_s(wires[0]));edges=[];curves=[];owners=[]
    faces=shapes(region,TopAbs_FACE)
    while explorer.More():
        edge=explorer.Current();first,last=BRep_Tool.Range_s(edge)
        curve=GeomConvert.CurveToBSplineCurve_s(Geom_TrimmedCurve(BRep_Tool.Curve_s(edge,first,last),first,last))
        if edge.Orientation()==TopAbs_REVERSED:curve.Reverse()
        owner=next(f for f in faces if any(edge.IsSame(e) for e in shapes(f,TopAbs_EDGE)))
        edges.append(edge);curves.append(curve);owners.append(owner);explorer.Next()
    return edges,curves,owners


def bspline_curve(poles,t):
    p=TColgp_Array1OfPnt(1,len(poles))
    for i,x in enumerate(poles):p.SetValue(i+1,gp_Pnt(*x))
    k,m=np.unique(t,return_counts=True)
    return Geom_BSplineCurve(p,array1(k),array1(m,True),3)


def iso_mapping(surface,curve):
    sample=np.linspace(0,1,9);first,last=curve.FirstParameter(),curve.LastParameter()
    target=np.array([xyz(curve.Value(float(first+t*(last-first)))) for t in sample]);options=[]
    for axis in [0,1]:
        for fixed in [0.,1.]:
            for reverse in [False,True]:
                uv=np.empty((len(sample),2));uv[:,axis]=fixed;uv[:,1-axis]=1-sample if reverse else sample
                p,_=evaluate(surface,uv);options.append((float(np.max(np.linalg.norm(p-target,axis=1))),dict(axis=axis,fixed=fixed,reverse=reverse)))
    error,mapping=min(options,key=lambda x:x[0])
    if error>1e-7:raise ValueError(f'Cannot map layout side to exact surface boundary: {error}')
    return mapping


def uv_on_side(mapping,t):
    t=np.asarray(t);uv=np.empty((len(t),2));uv[:,mapping['axis']]=mapping['fixed']
    uv[:,1-mapping['axis']]=1-t if mapping['reverse'] else t
    return uv


def build_layout(data,region,protocol,layers):
    edges,source,owners=original_loop(region);corner=protocol['corner_edge_indices'];count=len(edges)
    outer=np.array([xyz(source[i].StartPoint()) for i in corner])
    local=(outer-data['center'])@data['axes'].T;xy=local[:,:2]*protocol['inner_scale']
    samples=(data['train_points']-data['center'])@data['axes'].T
    interpolator=LinearNDInterpolator(samples[:,:2],samples[:,2]);z=interpolator(xy)
    if np.any(~np.isfinite(z)):raise ValueError('Inner layout corners outside source domain')
    inner=np.c_[xy,z]@data['axes']+data['center'];t=knots(protocol['seam_control_points'])
    greville=np.array([t[i+1:i+4].mean() for i in range(len(t)-4)])
    seam_curves=[];seam_ends=[]
    for j in range(4):
        curve=bspline_curve(outer[j]*(1-greville[:,None])+inner[j]*greville[:,None],t)
        for value in protocol.get('radial_refinement',[]):curve.InsertKnot(value,1,1e-12,True)
        seam_curves.append(curve);seam_ends.append((None,j))
    for j in range(4):
        inner_t=knots(protocol.get('inner_seam_control_points',protocol['seam_control_points']))
        inner_g=np.array([inner_t[i+1:i+4].mean() for i in range(len(inner_t)-4)])
        seam_curves.append(bspline_curve(inner[j]*(1-inner_g[:,None])+inner[(j+1)%4]*inner_g[:,None],inner_t));seam_ends.append((j,(j+1)%4))
    surfaces=[];sides=[];outer_components=[];boundary=[]
    for j in range(4):
        end=corner[(j+1)%4];indices=list(range(corner[j],end)) if end>corner[j] else list(range(corner[j],count))+list(range(end))
        join=GeomConvert_CompCurveToBSplineCurve(source[indices[0]].Copy())
        for k in indices[1:]:
            if not join.Add(source[k].Copy(),protocol['source_curve_join_tolerance_mm'],True,True,0):raise ValueError('Original boundary chain failed')
        outer_curve=join.BSplineCurve()
        first,last=outer_curve.FirstParameter(),outer_curve.LastParameter()
        outer_curve.SetKnots(array1([(outer_curve.Knot(k)-first)/(last-first) for k in range(1,outer_curve.NbKnots()+1)]))
        curves=[outer_curve,seam_curves[(j+1)%4].Copy(),seam_curves[4+j].Copy(),seam_curves[j].Copy()]
        curves[2].Reverse();curves[3].Reverse()
        surface=GeomFill_BSplineCurves(*[c.Copy() for c in curves],GeomFill_CoonsStyle).Surface()
        outer_map=iso_mapping(surface,outer_curve)
        for fraction in layers:
            value=fraction if outer_map['fixed']==0 else 1-fraction
            if outer_map['axis']==0:surface.InsertUKnot(value,1,1e-12,False)
            else:surface.InsertVKnot(value,1,1e-12,False)
        records=[dict(outer=True,mapping=outer_map)]
        for sid in [(j+1)%4,4+j,j]:records.append(dict(outer=False,seam=sid,mapping=iso_mapping(surface,seam_curves[sid])))
        first,last=outer_curve.FirstParameter(),outer_curve.LastParameter()
        for k in indices:
            edge=edges[k];a,b=BRep_Tool.Range_s(edge);original_curve=BRep_Tool.Curve_s(edge,a,b)
            values=[]
            for parameter in [a,b]:
                query=GeomAPI_ProjectPointOnCurve(original_curve.Value(parameter),outer_curve)
                if not query.NbPoints():raise ValueError('Original endpoint projection failed')
                values.append((query.LowerDistanceParameter()-first)/(last-first))
            ts=np.linspace(a,b,193);normalized=values[0]+(ts-a)/(b-a)*(values[1]-values[0]);uv=uv_on_side(outer_map,normalized)
            p,n=normals_on_edge(edge,owners[k],ts);q,_=evaluate(surface,uv)
            error=float(np.max(np.linalg.norm(p-q,axis=1)))
            if error>1e-7:raise ValueError(f'Original edge {k} lost exact affine curve mapping: {error}')
            boundary.append(dict(edge=k,patch=j,range=[a,b],normalized_endpoints=values,mapping=outer_map,initial_error_mm=error))
        surfaces.append(surface);sides.append(records);outer_components.append(indices)
    center=GeomFill_BSplineCurves(*[c.Copy() for c in seam_curves[4:]],GeomFill_CoonsStyle).Surface()
    surfaces.append(center);sides.append([dict(outer=False,seam=i,mapping=iso_mapping(center,seam_curves[i])) for i in range(4,8)])
    return dict(surfaces=surfaces,sides=sides,edges=edges,owners=owners,outer_components=outer_components,boundary=boundary,
        seam_curves=seam_curves,seam_ends=seam_ends,
        seam_knots=[np.repeat([c.Knot(k) for k in range(1,c.NbKnots()+1)],[c.Multiplicity(k) for k in range(1,c.NbKnots()+1)]) for c in seam_curves],inner=inner,outer=outer)


def variable_maps(layout):
    sizes=[len(t)-4 for t in layout['seam_knots']];master_count=4+sum(n-2 for n in sizes);seam_maps=[];cursor=4
    for sid,(start,end) in enumerate(layout['seam_ends']):
        n=sizes[sid]
        m=np.zeros((n,master_count))
        if start is not None:m[0,start]=1
        m[-1,end]=1
        for k in range(1,n-1):m[k,cursor+k-1]=1
        cursor+=n-2
        seam_maps.append(m)
    counts=[(s.NbUPoles()-2)*(s.NbVPoles()-2) for s in layout['surfaces']]
    total=master_count+sum(counts);matrices=[];offset=master_count
    for surface,sides in zip(layout['surfaces'],layout['sides']):
        p,tu,tv,degree=from_occt(surface);nu,nv=p.shape[:2];boundary={}
        for side in sides:
            mapping=side['mapping'];constant=mapping['axis'];t=tv if constant==0 else tu
            count=len(t)-4;g=np.array([t[i+1:i+4].mean() for i in range(count)])
            if side['outer']:embedded=np.zeros((count,master_count))
            else:
                conversion=np.linalg.solve(basis(t,3,g),basis(layout['seam_knots'][side['seam']],3,1-g if mapping['reverse'] else g))
                embedded=conversion@seam_maps[side['seam']]
            for k,row in enumerate(embedded):
                i=(0 if mapping['fixed']==0 else nu-1) if constant==0 else k
                j=k if constant==0 else (0 if mapping['fixed']==0 else nv-1)
                key=i*nv+j;row[np.abs(row)<1e-12]=0
                if key in boundary and np.max(np.abs(boundary[key]-row))>1e-9:raise ValueError('Conflicting shared corner variables')
                boundary[key]=row
        rows=[];columns=[];values=[]
        for i in range(nu):
            for j in range(nv):
                key=i*nv+j
                if key in boundary:
                    row=boundary[key];nz=np.flatnonzero(row);rows.extend([key]*len(nz));columns.extend(nz);values.extend(row[nz])
                else:rows.append(key);columns.append(offset);values.append(1.);offset+=1
        matrices.append(sparse.csr_matrix((values,(rows,columns)),shape=(nu*nv,total)))
    return matrices,seam_maps,total


def assemble(data,layout,maps,parameters,protocol):
    total=maps[0].shape[1];points=(data['train_points']-data['center'])@data['axes'].T;normals=data['train_normals']@data['axes'].T
    matrices=[];targets=[];patchdata=[];n=len(points)
    mesh=data['mesh_points'];tri=data['mesh_triangles']
    reference_area=np.linalg.norm(np.cross(mesh[tri[:,1]]-mesh[tri[:,0]],mesh[tri[:,2]]-mesh[tri[:,0]]),axis=1).sum()/2
    def append(op,rhs,weight,mapping):
        factor=np.asarray(weight)
        if factor.ndim:op=op.multiply(factor[:,None]);rhs=rhs*factor
        else:op=op*float(factor);rhs=rhs*float(factor)
        matrices.append(op@mapping);targets.append(rhs)
    for fi,surface in enumerate(layout['surfaces']):
        p,tu,tv,degree=from_occt(surface);p=(p-data['center'])@data['axes'].T;z=p[:,:,2].ravel();patchdata.append((p,tu,tv,degree,z))
        mask=parameters['patch'][:n]==fi;uv=parameters['uv'][:n][mask];b=tensor(uv,tu,tv,degrees=degree)
        append(b,points[mask,2]-b@z,1/np.sqrt(n),maps[fi])
        dx,dy,_=operators(p,tu,tv,degree,uv)
        for op,k in [(dx,0),(dy,1)]:append(op,-normals[mask,k]/normals[mask,2]-op@z,np.sqrt(protocol['normal_weight_mm2']/n),maps[fi])
        quad,weight=quadrature(tu,tv);xx,xy,yy,det=operators(p,tu,tv,degree,quad,True)
        area=weight*np.abs(det);factor=np.sqrt(protocol['fairness_weight_mm4']*area/reference_area)
        for op,mult in [(xx,1),(xy,np.sqrt(2)),(yy,1)]:append(op,-op@z,factor*mult,maps[fi])
    # New seams have one shared position representation, and two fitted gradient traces.
    seam_reports=[]
    for sid in range(8):
        traces=[]
        for fi,sides in enumerate(layout['sides']):
            for side in sides:
                if not side['outer'] and side['seam']==sid:
                    p,tu,tv,degree,z=patchdata[fi];uv=uv_on_side(side['mapping'],np.linspace(0,1,193))
                    dx,dy,_=operators(p,tu,tv,degree,uv);traces.append((fi,dx,dy,z))
        if len(traces)!=2:raise ValueError('Internal edge does not have exactly two faces')
        a,b=traces
        for k in [1,2]:
            factor=np.sqrt(protocol['seam_gradient_weight_mm2']/(8*193))
            matrices.append((a[k]@maps[a[0]]-b[k]@maps[b[0]])*factor);targets.append((b[k]@b[3]-a[k]@a[3])*factor)
    for record in layout['boundary']:
        fi=record['patch'];a,b=record['range'];ts=np.linspace(a,b,193);s0,s1=record['normalized_endpoints']
        uv=uv_on_side(record['mapping'],s0+(ts-a)/(b-a)*(s1-s0))
        _,normal=normals_on_edge(layout['edges'][record['edge']],layout['owners'][record['edge']],ts)
        normal=normal@data['axes'].T
        if np.min(np.abs(normal[:,2]))<.05:raise ValueError('Boundary height-graph normal becomes singular')
        p,tu,tv,degree,z=patchdata[fi];dx,dy,_=operators(p,tu,tv,degree,uv)
        for op,k in [(dx,0),(dy,1)]:append(op,-normal[:,k]/normal[:,2]-op@z,np.sqrt(protocol['boundary_normal_weight_mm2']/(len(layout['boundary'])*193)),maps[fi])
    knot_weight=protocol.get('internal_knot_gradient_weight_mm2',0.)
    knot_records=[]
    for fi,(p,tu,tv,degree,z) in enumerate(patchdata):
        for axis,t in enumerate([tu,tv]):
            k,m=np.unique(t,return_counts=True)
            for i in range(1,len(k)-1):
                if m[i]>=degree[axis]:knot_records.append((fi,axis,k[i],min(k[i]-k[i-1],k[i+1]-k[i])*1e-7))
    if knot_weight:
        for fi,axis,value,epsilon in knot_records:
            p,tu,tv,degree,z=patchdata[fi];left=np.empty((193,2));left[:,axis]=value-epsilon;left[:,1-axis]=np.linspace(0,1,193)
            right=left.copy();right[:,axis]=value+epsilon
            a=operators(p,tu,tv,degree,left);b=operators(p,tu,tv,degree,right)
            for k in [0,1]:
                op=a[k]-b[k]
                append(op,-op@z,np.sqrt(knot_weight/(len(knot_records)*193)),maps[fi])
    return sparse.vstack(matrices).tocsc(),np.concatenate(targets),knot_records


def solve(data,layout,maps,parameters,protocol):
    matrix,target,knot_records=assemble(data,layout,maps,parameters,protocol)
    total=maps[0].shape[1];knot_weight=protocol.get('internal_knot_gradient_weight_mm2',0.)
    scale=1/np.maximum(np.sqrt(np.asarray(matrix.power(2).sum(axis=0)).ravel()),1e-15)
    scaled=matrix@sparse.diags(scale);normal=(scaled.T@scaled).tocsc()+sparse.diags(1e-12*scale*scale,format='csc')
    rhs=scaled.T@target;x=splu(normal).solve(rhs);delta=scale*x
    return delta,dict(unknowns=total,rows=matrix.shape[0],nnz=matrix.nnz,relative_linear_residual=float(np.linalg.norm(normal@x-rhs)/np.linalg.norm(rhs)),
        maximum_free_height_change_mm=float(np.max(np.abs(delta))),internal_c0_knots=len(knot_records),internal_knot_gradient_weight_mm2=knot_weight)


def export_layout(layout,path):
    builder=BRep_Builder();shell=TopoDS_Shell();builder.MakeShell(shell)
    # Reuse all original edge geometry and original tolerances. New seams share vertices.
    outer_vertices=[TopExp.FirstVertex_s(layout['edges'][i],True) for i in layout['corner_indices']]
    inner_vertices=[BRepBuilderAPI_MakeVertex(gp_Pnt(*p)).Vertex() for p in layout['inner_final']]
    newedges=[]
    for sid,curve in enumerate(layout['seam_curves']):
        if sid<4:a,b=outer_vertices[sid],inner_vertices[sid]
        else:a,b=inner_vertices[sid-4],inner_vertices[(sid-3)%4]
        newedges.append(BRepBuilderAPI_MakeEdge(curve,a,b,0.,1.).Edge())
    def pcurve(uv,first,last):
        p=TColgp_Array1OfPnt2d(1,2)
        for i,q in enumerate(uv):p.SetValue(i+1,gp_Pnt2d(*q))
        return Geom2d_BSplineCurve(p,array1([first,last]),array1([2,2],True),1)
    for record in layout['boundary']:
        edge=layout['edges'][record['edge']];surface=layout['surfaces'][record['patch']]
        uv=uv_on_side(record['mapping'],record['normalized_endpoints']);first,last=record['range']
        builder.UpdateEdge(edge,pcurve(uv,first,last),surface,TopLoc_Location(),BRep_Tool.Tolerance_s(edge))
    for fi,(surface,sides) in enumerate(zip(layout['surfaces'],layout['sides'])):
        for side in sides:
            if side['outer']:continue
            edge=newedges[side['seam']];uv=uv_on_side(side['mapping'],[0.,1.])
            builder.UpdateEdge(edge,pcurve(uv,0.,1.),surface,TopLoc_Location(),BRep_Tool.Tolerance_s(edge))
        wire=BRepBuilderAPI_MakeWire()
        if fi<4:
            for index in layout['outer_components'][fi]:wire.Add(layout['edges'][index])
            wire.Add(newedges[(fi+1)%4]);wire.Add(TopoDS.Edge_s(newedges[4+fi].Reversed()));wire.Add(TopoDS.Edge_s(newedges[fi].Reversed()))
        else:
            for edge in newedges[4:]:wire.Add(edge)
        if not wire.IsDone():raise ValueError('New layout wire is not connected')
        face=BRepBuilderAPI_MakeFace(surface,wire.Wire(),True).Face()
        _,normal=evaluate(surface,np.array([[.5,.5]]))
        if normal[0]@layout['normal_axis']<0:face=TopoDS.Face_s(face.Reversed())
        builder.Add(shell,face)
    write_step(path,shell)
    return shell


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--names',nargs='+');args=parser.parse_args()
    out=Path(args.run);protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'));data=np.load(out/'reference.npz')
    for config in protocol['configurations']:
        if args.names and config['name'] not in args.names:continue
        started=time.perf_counter();target=out/config['name'];target.mkdir(exist_ok=False)
        effective=dict(protocol);effective['seam_control_points']=config.get('seam_control_points',protocol['seam_control_points'])
        effective['radial_refinement']=config.get('radial_refinement',[])
        effective['internal_knot_gradient_weight_mm2']=config.get('internal_knot_gradient_weight_mm2',0.)
        effective['inner_scale']=config.get('inner_scale',protocol['inner_scale'])
        effective['inner_seam_control_points']=config.get('inner_seam_control_points',effective['seam_control_points'])
        region=read_step(out/'original_region.step');layout=build_layout(data,region,effective,config['boundary_layers'])
        layout.update(corner_indices=protocol['corner_edge_indices'],inner_final=layout['inner'].copy(),normal_axis=data['axes'][2])
        # A separate initial geometry records the new layout; it is never the shape truth.
        seed=export_layout(layout,target/'initial_layout.step')
        seed_faces=shapes(seed,TopAbs_FACE);parameterize(data,seed_faces,target)
        parameter_report=json.loads((target/'parameterization.json').read_text(encoding='utf-8'))
        parameter_report['method']='Invert newly constructed immutable layout XY maps; heights/normals come from original region; OCCT fallback for exceptions'
        dump(target/'parameterization.json',parameter_report);params=np.load(target/'parameterization.npz')
        maps,seam_maps,total=variable_maps(layout);delta,solver=solve(data,layout,maps,params,effective)
        patches=[]
        for fi,(surface,mapping) in enumerate(zip(layout['surfaces'],maps)):
            p,tu,tv,degree=from_occt(surface);change=(mapping@delta).reshape(p.shape[:2]);newp=p+change[:,:,None]*data['axes'][2]
            for i in range(p.shape[0]):
                for j in range(p.shape[1]):
                    if change[i,j]!=0:surface.SetPole(i+1,j+1,gp_Pnt(*newp[i,j]))
            np.savez_compressed(target/f'patch_{fi}.npz',poles=newp,tu=tu,tv=tv,degrees=degree)
            patches.append(dict(patch=fi,complexity=surface_info(surface),differential=differential(newp,tu,tv,degree,data['axes'])))
        for sid,curve in enumerate(layout['seam_curves']):
            height=seam_maps[sid]@delta[:seam_maps[sid].shape[1]]
            for i,x in enumerate(height):curve.SetPole(i+1,gp_Pnt(*(xyz(curve.Pole(i+1))+x*data['axes'][2])))
        layout['inner_final']=layout['inner']+delta[:4,None]*data['axes'][2]
        shape=export_layout(layout,target/'patch_faces.step');readback=read_step(target/'patch_faces.step')
        supports=[BRepBuilderAPI_MakeFace(s,1e-7).Face() for s in layout['surfaces']];write_step(target/'support.step',compound(supports))
        checks=edge_checks(readback,385)
        result=dict(config=config,status='DiagnosticOnly',can_apply=False,solver=solver,patches=patches,
            actual_control_points=sum(BRep_Tool.Surface_s(f).NbUPoles()*BRep_Tool.Surface_s(f).NbVPoles() for f in shapes(readback,TopAbs_FACE)),
            before_write=topology(shape),after_read=topology(readback),edge_checks=checks,seams=seam_quality(readback),
            boundary_mapping=layout['boundary'],sides=layout['sides'],inner_corners_initial=layout['inner'].tolist(),inner_corners_final=layout['inner_final'].tolist(),
            seconds=time.perf_counter()-started)
        dump(target/'fit.json',result)
        print(config['name'],'poles',result['actual_control_points'],'valid',result['after_read']['brep_valid'],
            'edge residual',checks['max_curve_on_surface_mm'],'max seam angle',max(s['normal_degrees']['max'] for s in result['seams']),flush=True)


if __name__=='__main__':main()
