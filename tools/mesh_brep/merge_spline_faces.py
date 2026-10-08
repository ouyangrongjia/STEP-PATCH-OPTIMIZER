"""Exact rectangular B-spline concatenation inside supplied mesh regions.

Common knot refinement and a degree-multiplicity joining knot preserve both
input surfaces. No smoothing, source-CAD fitting or tolerance growth is used.
Faces spanning uncertain source-label boundaries are retained separately.
"""
import argparse
from pathlib import Path
import time
import numpy as np
from OCP.BRep import BRep_Tool
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace,BRepBuilderAPI_Sewing,BRepBuilderAPI_MakeSolid
from OCP.BRepLib import BRepLib
from OCP.BRepTools import BRepTools
from OCP.Geom import Geom_BSplineSurface
from OCP.TopAbs import TopAbs_FACE,TopAbs_EDGE,TopAbs_SHELL,TopAbs_REVERSED
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS
from OCP.TColgp import TColgp_Array2OfPnt
from OCP.gp import gp_Pnt
from . import cad
from .mesh import Locator,dump
from .partition_cad import array,topology
from .repair_spline_seams import read
from .step_quality import native


def classify(face,loc,labels,purity):
    surface=BRep_Tool.Surface_s(face)
    if not isinstance(surface,Geom_BSplineSurface) or surface.IsURational() or surface.IsVRational():return -1,None
    bounds=np.array(surface.Bounds()).reshape(2,2)
    t=(np.arange(11)+.5)/11
    uv=np.stack(np.meshgrid(t,t,indexing='ij'),axis=-1).reshape(-1,2)
    q=bounds[:,0]+uv*(bounds[:,1]-bounds[:,0]);xyz,_,du,dv=native(surface,q)
    _,_,ids,_=loc.closest(xyz);value=labels[ids];weight=np.linalg.norm(np.cross(du,dv),axis=1)
    unique=np.unique(value);mass=np.array([weight[value==k].sum() for k in unique])
    if mass.sum()<=1e-20:return -1,None
    winner=int(np.argmax(mass));ratio=float(mass[winner]/mass.sum())
    return (int(unique[winner]) if ratio>=purity else -1),dict(region=int(unique[winner]),purity=ratio)


def edge_side(edge,face):
    s=BRep_Tool.Surface_s(face);bounds=np.array(s.Bounds()).reshape(2,2)
    a,b=BRep_Tool.Range_s(edge);pc=BRep_Tool.CurveOnSurface_s(edge,face,a,b)
    if pc is None:return None
    uv=np.array([pc.Value(float(t)).Coord() for t in np.linspace(a,b,5)])
    norm=(uv-bounds[:,0])/(bounds[:,1]-bounds[:,0]);extent=np.ptp(norm,axis=0)
    axis=int(np.argmax(extent));fixed=1-axis
    if extent[fixed]>1e-8:return None
    high=int(np.mean(norm[:,fixed])>.5)
    if np.max(np.abs(norm[:,fixed]-high))>1e-8:return None
    return axis,high,float(norm[:,axis].min()),float(norm[:,axis].max())


def adjacency(faces):
    indexed=TopTools_IndexedMapOfShape();uses=[];rect=[]
    for fi,face in enumerate(faces):
        natural=True
        for edge in cad.shapes(face,TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge):natural=False;continue
            side=edge_side(edge,face);natural &= side is not None
            j=indexed.Add(edge)-1
            if j==len(uses):uses.append([])
            uses[j].append((fi,side))
        rect.append(natural)
    pairs={}
    for use in uses:
        if len(use)!=2:continue
        a,b=sorted(use,key=lambda x:x[0])
        if a[1] is None or b[1] is None or not rect[a[0]] or not rect[b[0]]:continue
        pairs.setdefault((a[0],b[0]),[]).append((a[1],b[1]))
    result=[]
    for (a,b),seams in pairs.items():
        sides=[]
        for owner in [0,1]:
            seq=[x[owner] for x in seams]
            if len({x[:2] for x in seq})!=1:break
            intervals=sorted((x[2],x[3]) for x in seq)
            if abs(intervals[0][0])>1e-8 or abs(intervals[-1][1]-1)>1e-8:break
            if any(abs(x[1]-y[0])>1e-8 for x,y in zip(intervals,intervals[1:])):break
            sides.append(seq[0][:2])
        if len(sides)==2:result.append((a,b,*sides))
    return result


def free_face_indices(shape):
    faces=cad.shapes(shape,TopAbs_FACE);indexed=TopTools_IndexedMapOfShape();owners=[]
    for fi,face in enumerate(faces):
        for edge in cad.shapes(face,TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge):continue
            j=indexed.Add(edge)-1
            if j==len(owners):owners.append([])
            owners[j].extend([fi]*(2 if BRep_Tool.IsClosed_s(edge,face) else 1))
    bad={n[0] for n in owners if len(n)==1}
    return faces,bad


def normalized(surface,side,want_high):
    s=surface.Copy();axis,high=side;sign=1;matrix=np.eye(2);offset=np.zeros(2)
    if axis==0:
        s.ExchangeUV();sign*=-1;matrix=matrix[::-1];offset=offset[::-1]
    if high!=want_high:
        bounds=s.Bounds();s.UReverse();sign*=-1;matrix[0]*=-1;offset[0]=bounds[0]+bounds[1]-offset[0]
    for direction in ['U','V']:
        axis=0 if direction=='U' else 1
        n=getattr(s,'Nb'+direction+'Knots')();get=getattr(s,direction+'Knot')
        values=np.array([get(i) for i in range(1,n+1)]);low=values[0];width=values[-1]-low
        matrix[axis]/=width;offset[axis]=(offset[axis]-low)/width;values=(values-low)/width
        getattr(s,'Set'+direction+'Knots')(array(values))
    return s,sign,matrix,offset


def preserve_shared_trims(first,second,surface,maps,sign,tolerance):
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeWire
    from OCP.Geom2d import Geom2d_TrimmedCurve
    from OCP.Geom2dConvert import Geom2dConvert
    from OCP.TopExp import TopExp
    from OCP.TopLoc import TopLoc_Location
    from OCP.gp import gp_Pnt2d
    groups=[cad.shapes(f,TopAbs_EDGE) for f in [first,second]]
    builder=BRep_Builder();outer=[];removed=0
    for fi,face in enumerate([first,second]):
        matrix,offset=maps[fi]
        for edge in groups[fi]:
            if any(edge.IsSame(other) for other in groups[1-fi]):removed+=1;continue
            a,b=BRep_Tool.Range_s(edge);original=BRep_Tool.CurveOnSurface_s(edge,face,a,b)
            pc=Geom2dConvert.CurveToBSplineCurve_s(Geom2d_TrimmedCurve(original,a,b))
            # Conversion can rescale a line's parameter; restore the exact edge
            # interval before transforming UV coordinates.
            lo,hi=pc.FirstParameter(),pc.LastParameter()
            pc.SetKnots(array([a+(pc.Knot(k)-lo)*(b-a)/(hi-lo) for k in range(1,pc.NbKnots()+1)]))
            for k in range(1,pc.NbPoles()+1):pc.SetPole(k,gp_Pnt2d(*(matrix@np.array(pc.Pole(k).Coord())+offset)))
            for t in np.linspace(a,b,17):
                expected=matrix@np.array(original.Value(float(t)).Coord())+offset
                if np.linalg.norm(np.array(pc.Value(float(t)).Coord())-expected)>1e-9:
                    raise ValueError('Affine pcurve transfer changes its parameterization')
                xyz=surface.Value(*pc.Value(float(t)).Coord());curve=BRep_Tool.Curve_s(edge,a,b)
                if xyz.Distance(curve.Value(float(t)))>tolerance:raise ValueError('Transferred boundary leaves the common 3D edge')
            builder.UpdateEdge(edge,pc,surface,TopLoc_Location(),BRep_Tool.Tolerance_s(edge))
            outer.append(edge if sign>0 else TopoDS.Edge_s(edge.Reversed()))
    if not removed:raise ValueError('Input faces have no shared topological edge')
    chain=[outer.pop(0)]
    while outer:
        end=TopExp.LastVertex_s(chain[-1],True)
        options=[i for i,e in enumerate(outer) if TopExp.FirstVertex_s(e,True).IsSame(end)]
        if len(options)!=1:raise ValueError('Merged boundary is not one oriented wire')
        chain.append(outer.pop(options[0]))
    wire=BRepBuilderAPI_MakeWire()
    for edge in chain:wire.Add(edge)
    face=BRepBuilderAPI_MakeFace(surface,wire.Wire(),True).Face()
    return face if sign>0 else TopoDS.Face_s(face.Reversed())


def concatenate(first,second,side_a,side_b,tolerance=1e-7):
    initial_surfaces=[BRep_Tool.Surface_s(f) for f in [first,second]]
    initial_controls=sum(s.NbUPoles()*s.NbVPoles() for s in initial_surfaces)
    a,sign_a,ma,oa=normalized(BRep_Tool.Surface_s(first),side_a,1)
    b,sign_b,mb,ob=normalized(BRep_Tool.Surface_s(second),side_b,0)
    if a.Value(1,0).Distance(b.Value(0,1))<a.Value(1,0).Distance(b.Value(0,0)):
        b.VReverse();sign_b*=-1;mb[1]*=-1;ob[1]=1-ob[1]
    original_a,original_b=a.Copy(),b.Copy()
    pu=max(a.UDegree(),b.UDegree());pv=max(a.VDegree(),b.VDegree())
    for s in [a,b]:s.IncreaseDegree(pu,pv)
    union={}
    for s in [a,b]:
        for j in range(1,s.NbVKnots()+1):
            k=round(s.VKnot(j),12);union[k]=max(union.get(k,0),s.VMultiplicity(j))
    for s in [a,b]:
        for k,m in sorted(union.items()):
            if 1e-10<k<1-1e-10:s.InsertVKnot(k,m,1e-10,False)
    def poles(s):return np.array([[s.Pole(i,j).Coord() for j in range(1,s.NbVPoles()+1)] for i in range(1,s.NbUPoles()+1)])
    ca,cb=poles(a),poles(b)
    if ca.shape[1]!=cb.shape[1]:raise ValueError('Transverse common knot refinement failed')
    gap=float(np.linalg.norm(ca[-1]-cb[0],axis=1).max())
    if gap>tolerance:raise ValueError(f'Boundary control points differ by {gap}')
    cp=np.concatenate([ca[:-1],cb]);cp[len(ca)-1]=(ca[-1]+cb[0])/2
    points=TColgp_Array2OfPnt(1,len(cp),1,cp.shape[1])
    for i in range(len(cp)):
        for j in range(cp.shape[1]):points.SetValue(i+1,j+1,gp_Pnt(*cp[i,j]))
    ku=[a.UKnot(i) for i in range(1,a.NbUKnots())]+[1.]+[1+b.UKnot(i) for i in range(2,b.NbUKnots()+1)]
    mu=[a.UMultiplicity(i) for i in range(1,a.NbUKnots())]+[pu]+[b.UMultiplicity(i) for i in range(2,b.NbUKnots()+1)]
    kv=[a.VKnot(i) for i in range(1,a.NbVKnots()+1)];mv=[a.VMultiplicity(i) for i in range(1,a.NbVKnots()+1)]
    result=Geom_BSplineSurface(points,array(ku),array(kv),array(mu,True),array(mv,True),pu,pv)
    sign=(-1 if first.Orientation()==TopAbs_REVERSED else 1)*sign_a
    other=(-1 if second.Orientation()==TopAbs_REVERSED else 1)*sign_b
    if sign!=other:raise ValueError('Neighbor surface orientations disagree')
    q=np.stack(np.meshgrid(np.linspace(0,1,9),np.linspace(0,1,9)),axis=-1).reshape(-1,2)
    error=max(max(original.Value(float(u),float(v)).Distance(result.Value(float(u+offset),float(v))) for u,v in q)
              for original,offset in [(original_a,0),(original_b,1)])
    if error>tolerance:raise ValueError(f'Concatenation changes surface geometry by {error}')
    ob[0]+=1
    face=preserve_shared_trims(first,second,result,[(ma,oa),(mb,ob)],sign,tolerance)
    return face,dict(maximum_sampled_geometry_change_mm=error,common_curve_control_gap_mm=gap,
                     input_control_points=initial_controls,
                     refined_input_control_points=ca.shape[0]*ca.shape[1]+cb.shape[0]*cb.shape[1],output_control_points=len(cp)*cp.shape[1])


def merge(source,partition,output,rounds=4,purity=.99,strips=False):
    out=Path(output);out.mkdir(parents=True,exist_ok=False);started=time.perf_counter();shape=read(source)
    data=np.load(partition);loc=Locator(data['mesh_vertex_original'],data['mesh_triangle']);labels=data['face_instance_id']
    report=dict(source=str(Path(source).resolve()),partition=str(Path(partition).resolve()),input=topology(shape),
                label_purity_threshold=purity,rounds=[],status='running',strategy='directional-strips' if strips else 'greedy',
                topology_strategy='retain original shared edges and affine-transform pcurves')
    dump(out/'run.json',report)
    try:
        initial=report['input']
        if initial['solids']!=1 or initial['free_edges'] or initial['multiple_edges'] or not initial['brep_valid']:
            raise ValueError('Exact region merging requires one valid closed solid')
        orientation={};phase=0
        for iteration in range(rounds):
            faces=cad.shapes(shape,TopAbs_FACE);classified=[classify(f,loc,labels,purity) for f in faces]
            regions=[x[0] for x in classified];taken=set();made=[];merges=[];rejected=[]
            candidates=adjacency(faces)
            if strips and not orientation:
                graph={i:[] for i in range(len(faces))}
                for a,b,sa,sb in candidates:
                    if regions[a]>=0 and regions[a]==regions[b]:
                        graph[a].append((b,sa[0]^sb[0]));graph[b].append((a,sa[0]^sb[0]))
                axes={}
                for start in range(len(faces)):
                    if start in axes:continue
                    axes[start]=0;stack=[start]
                    while stack:
                        i=stack.pop()
                        for j,flip in graph[i]:
                            if j not in axes:axes[j]=axes[i]^flip;stack.append(j)
                orientation={BRep_Tool.Surface_s(f):axes[i] for i,f in enumerate(faces)}
            for a,b,sa,sb in candidates:
                if a in taken or b in taken or regions[a]<0 or regions[a]!=regions[b]:continue
                if strips:
                    oa=orientation[BRep_Tool.Surface_s(faces[a])];ob=orientation[BRep_Tool.Surface_s(faces[b])]
                    if (oa^sa[0])!=(ob^sb[0]) or (oa^sa[0])!=(1-phase):continue
                try:face,info=concatenate(faces[a],faces[b],sa,sb)
                except ValueError as e:rejected.append(dict(faces=[a,b],error=str(e)));continue
                if strips:orientation[BRep_Tool.Surface_s(face)]=phase
                taken.update([a,b]);made.append(face);merges.append(dict(faces=[a,b],region=regions[a],**info))
            if not merges:
                if strips and phase==0:phase=1;continue
                break
            active=set(range(len(made)));rollback=[]
            surface_to_merge={BRep_Tool.Surface_s(f):i for i,f in enumerate(made)}
            original_surface={BRep_Tool.Surface_s(f):i for i,f in enumerate(faces)}
            face_to_merge={j:i for i,rec in enumerate(merges) for j in rec['faces']}
            neighbors={i:set() for i in range(len(faces))}
            for a,b,_,_ in candidates:neighbors[a].add(b);neighbors[b].add(a)
            for retry in range(8):
                taken={j for i in active for j in merges[i]['faces']}
                from OCP.BRep import BRep_Builder
                from OCP.TopoDS import TopoDS_Shell
                builder=BRep_Builder();candidate=TopoDS_Shell();builder.MakeShell(candidate)
                for i,f in enumerate(faces):
                    if i not in taken:builder.Add(candidate,f)
                for i in active:builder.Add(candidate,made[i])
                topo=topology(candidate)
                if not topo['free_edges'] and not topo['multiple_edges'] and topo['brep_valid']:break
                cf,bad=free_face_indices(candidate);bad.update(topo['invalid_faces']);undo=set()
                for fi in bad:
                    support=BRep_Tool.Surface_s(cf[fi])
                    if support in surface_to_merge:undo.add(surface_to_merge[support])
                    elif support in original_surface:
                        for neighbor in neighbors[original_surface[support]]:
                            if neighbor in face_to_merge:undo.add(face_to_merge[neighbor])
                undo &= active
                rollback.append(dict(retry=retry,free_edges=topo['free_edges'],invalid_faces=topo['invalid_faces'],undone_merges=sorted(undo)))
                if not undo:break
                active-=undo
            record=dict(round=iteration+1,axis_phase=phase if strips else None,merges=[merges[i] for i in sorted(active)],rejected=rejected,rollback=rollback,
                        topology=topo,seconds=time.perf_counter()-started)
            report['rounds'].append(record);dump(out/'run.json',report)
            print(dict(round=iteration+1,merges=len(active),faces=topo['faces'],valid=topo['brep_valid'],free_edges=topo['free_edges']),flush=True)
            if topo['free_edges'] or topo['multiple_edges'] or topo['shells']!=1 or not topo['brep_valid']:
                BRepTools.Write_s(candidate,str(out/f'rejected_round_{iteration+1}.brep'));record['accepted']=False;break
            shape=BRepBuilderAPI_MakeSolid(cad.shapes(candidate,TopAbs_SHELL)[0]).Solid();BRepLib.OrientClosedSolid_s(shape);record['accepted']=True
        report['output']=topology(shape);BRepTools.Write_s(shape,str(out/'model.brep'));cad.write_step(out/'model.step',shape)
        report['reread']=topology(cad.read_generated(out/'model.step'));report['status']='DiagnosticOnly';report['can_apply']=False
    except Exception as e:report.update(status='failed',error=f'{type(e).__name__}: {e}');raise
    finally:report['seconds']=time.perf_counter()-started;dump(out/'run.json',report)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--partition',required=True)
    p.add_argument('--output',required=True);p.add_argument('--rounds',type=int,default=4);p.add_argument('--purity',type=float,default=.99)
    p.add_argument('--strips',action='store_true')
    a=p.parse_args();merge(a.source,a.partition,a.output,a.rounds,a.purity,a.strips)
