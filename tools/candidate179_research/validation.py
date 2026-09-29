"""Independent trimmed-surface checks retained from the rejected layout trial.

Evaluation charts never create output faces or alter STEP support counts.
"""
import argparse
import time
from geometry import *
from spline import from_occt, evaluate as spline_value
from evaluate import closest
from fit_graph import stats
from diagnose import curvature, seam_quality
from integrate import edge_checks
from scipy.spatial import cKDTree
from OCP.BRepTools import BRepTools
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf
from OCP.TopAbs import TopAbs_IN, TopAbs_ON
from OCP.gp import gp_Pnt2d


def angles(a,b):
    return np.degrees(np.arccos(np.clip(np.sum(a*b,axis=-1),-1.,1.)))


def prepared(shape):
    result = []; points = []; owners = []; params = []
    for fi, face in enumerate(shapes(shape, TopAbs_FACE)):
        surface = BRep_Tool.Surface_s(face)
        bounds = BRepTools.UVBounds_s(face)
        try:
            controls, tu, tv, degree = from_occt(surface)
            # STEP trim bounds can exceed clamped knot endpoints by a few ulps.
            # Never manufacture a zero-width chart from that roundoff interval.
            bounds=(max(bounds[0],tu[degree[0]]),min(bounds[1],tu[-degree[0]-1]),
                    max(bounds[2],tv[degree[1]]),min(bounds[3],tv[-degree[1]-1]))
            cuts=[]
            for axis,t in enumerate((tu,tv)):
                values,mults=np.unique(t,return_counts=True);lo,hi=bounds[2*axis:2*axis+2]
                epsilon=max(abs(hi-lo)*1e-12,1e-14)
                cuts.append(np.unique(np.r_[lo,values[(mults>=degree[axis])&(values>lo+epsilon)&(values<hi-epsilon)],hi]))
            charts=[]
            for u0,u1 in zip(cuts[0][:-1],cuts[0][1:]):
                for v0,v1 in zip(cuts[1][:-1],cuts[1][1:]):
                    # These are evaluation charts only. No B-Rep face is split
                    # or exported, and native STEP pole counts remain unchanged.
                    piece=surface.Copy();piece.Segment(float(u0),float(u1),float(v0),float(v1))
                    cp,a,b,d=from_occt(piece)
                    charts.append((piece,(u0,u1,v0,v1),(cp,a,b,d),(cp.min(axis=(0,1)),cp.max(axis=(0,1)))))
        except (ValueError, AttributeError):
            charts=[(surface,bounds,None,None)]
        for piece,limits,spline,bbox in charts:
            u=np.linspace(limits[0],limits[1],25);v=np.linspace(limits[2],limits[3],25)
            grid=np.stack(np.meshgrid(u,v,indexing='ij'),axis=-1).reshape(-1,2)
            p,_=evaluate(piece,grid);ci=len(result)
            artificial=[]
            for axis in (0,1):
                periodic=surface.IsUClosed() if axis==0 else surface.IsVClosed()
                for side in (0,1):
                    if periodic or abs(limits[2*axis+side]-bounds[2*axis+side])>1e-10:
                        artificial.append((axis,limits[2*axis+side]))
            result.append(dict(face=face,face_index=fi,surface=piece,spline=spline,bbox=bbox,bounds=limits,artificial_limits=artificial))
            points.extend(p);owners.extend([ci]*len(p));params.extend(grid)
    return result, cKDTree(points), np.asarray(owners), np.asarray(params)


def distances(points, shape, truth_normals=None):
    faces, tree, owners, parameters = prepared(shape)
    _, ids = tree.query(points, k=min(16, len(owners)))
    options = owners[ids]; initial = parameters[ids]
    result = np.full(len(points), np.inf); normal = np.zeros_like(points); chosen = np.zeros(len(points), dtype=int)
    uv_out = np.zeros((len(points), 2)); outside_count = 0; chart_boundary_queries=0; chart_boundary_corrections=0
    for pass_id in range(2):
        for fi, record in enumerate(faces):
            requested = np.any(options == fi, axis=1)
            if pass_id == 0:
                mask = requested
            elif record['bbox'] is None:
                mask = ~requested
            else:
                lo, hi = record['bbox']; lower = np.linalg.norm(np.maximum(lo-points, 0)+np.maximum(points-hi, 0), axis=1)
                mask = (~requested) & (lower < result)
            index = np.flatnonzero(mask)
            if not len(index):
                continue
            if record['spline'] is not None:
                p, tu, tv, degree = record['spline']
                if pass_id == 0:
                    pick = np.argmax(options[index] == fi, axis=1); uv = initial[index, pick]
                else:
                    # Query this face's own coarse grid for the AABB fallback.
                    grid_ids = np.flatnonzero(owners == fi)
                    values, _ = evaluate(record['surface'], parameters[grid_ids])
                    _, near = cKDTree(values).query(points[index]); uv = parameters[grid_ids[near]]
                # OCCT trim bounds can extend a few ulps past spline endpoints.
                # SciPy extrapolate=False yields NaNs there, silently losing an
                # otherwise closest face. Clamp to the exact spline domain.
                uv = np.clip(uv, [tu[degree[0]], tv[degree[1]]], [tu[-degree[0]-1], tv[-degree[1]-1]])
                d, n, uv, _ = closest(points[index], p, tu, tv, degree, uv, iterations=35)
                if not np.isfinite(d).all():
                    raise RuntimeError('Non-finite spline projection; validation cannot continue')
                # A C0 evaluation-chart boundary is not a physical trim edge.
                # A constrained Newton minimum there can hide another minimum
                # just inside the same chart. Query all such cases, not only the
                # holdout's worst points, with the kernel's multi-root projector.
                boundary=np.zeros(len(index),bool)
                for axis,value in record['artificial_limits']:
                    boundary|=abs(uv[:,axis]-value)<1e-9
                for local in np.flatnonzero(boundary):
                    chart_boundary_queries+=1
                    query=GeomAPI_ProjectPointOnSurf(gp_Pnt(*points[index[local]]),record['surface'],*record['bounds'],1e-10)
                    if query.NbPoints() and query.LowerDistance()<d[local]-1e-10:
                        chart_boundary_corrections+=1;d[local]=query.LowerDistance();uv[local]=query.LowerDistanceParameters()
                        _,nn=evaluate(record['surface'],uv[local:local+1]);n[local]=nn[0]
            else:
                d = np.full(len(index), np.inf); n = np.zeros((len(index), 3)); uv = np.zeros((len(index), 2))
                for j, point in enumerate(points[index]):
                    query = GeomAPI_ProjectPointOnSurf(gp_Pnt(*point), record['surface'], *record['bounds'], 1e-10)
                    if query.NbPoints():
                        d[j] = query.LowerDistance(); uv[j] = query.LowerDistanceParameters()
                _, n = evaluate(record['surface'], uv)
            if record['face'].Orientation() == TopAbs_REVERSED:
                n *= -1
            better = d < result[index]
            for local in np.flatnonzero(better):
                if BRepClass_FaceClassifier(record['face'], gp_Pnt2d(*uv[local]), 1e-9).State() not in (TopAbs_IN, TopAbs_ON):
                    outside_count += 1
                    query = BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*points[index[local]])).Vertex(), record['face'])
                    if not query.IsDone():
                        raise RuntimeError('Trim-aware kernel projection failed')
                    d[local] = query.Value()
                    proj = GeomAPI_ProjectPointOnSurf(query.PointOnShape2(1), record['surface'], *record['bounds'], 1e-10)
                    uv[local] = proj.LowerDistanceParameters(); _, nn = evaluate(record['surface'], uv[local:local+1])
                    n[local] = nn[0]*(-1 if record['face'].Orientation() == TopAbs_REVERSED else 1)
            better = d < result[index]; pick = index[better]
            result[pick] = d[better]; normal[pick] = n[better]; chosen[pick] = record['face_index']; uv_out[pick] = uv[better]
    if not np.isfinite(result).all():
        raise RuntimeError('Unresolved projection samples')
    angles_out = angles(normal, truth_normals) if truth_normals is not None else None
    indices = np.unique(np.r_[np.linspace(0, len(points)-1, 12).astype(int), np.argsort(result)[-8:],
                             [] if angles_out is None else np.argsort(angles_out)[-8:]]).astype(int)
    checks = []
    for i in indices:
        query = BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*points[i])).Vertex(), shape)
        if not query.IsDone():
            raise RuntimeError('Independent whole-region distance query failed')
        checks.append(dict(sample=int(i), numerical_mm=float(result[i]), kernel_mm=query.Value(), difference_mm=abs(float(result[i])-query.Value())))
    report = dict(distance_mm=stats(result), sample_count=len(points), kernel_checks=checks,
                  kernel_agreement_pass=max(c['difference_mm'] for c in checks) <= 1e-6,
                  trim_corrections=outside_count,
                  chart_boundary_kernel_queries=chart_boundary_queries,chart_boundary_kernel_corrections=chart_boundary_corrections,
                  evaluation_charts=len(faces),
                  method='Exact in-memory charts split at C0 parameter knots, coarse seeds, Newton projection, convex-control-hull AABB fallback, original face classifier and exact trim correction; sampled OCCT whole-region cross-check. Exported B-Rep faces are not split.')
    if angles_out is not None:
        report['normal_degrees'] = stats(angles_out)
    return report, dict(distance=result, normal_angle=angles_out, patch=chosen, uv=uv_out)


def continuity(shape):
    records = []
    for fi, face in enumerate(shapes(shape, TopAbs_FACE)):
        try:
            p, tu, tv, degree = from_occt(BRep_Tool.Surface_s(face))
        except (ValueError, AttributeError):
            records.append(dict(patch=fi, status='native_analytic_or_rational')); continue
        for axis, t in enumerate((tu, tv)):
            values, multiplicity = np.unique(t, return_counts=True)
            for i in range(1, len(values)-1):
                if multiplicity[i] < degree[axis]-1:
                    continue
                epsilon = min(values[i]-values[i-1], values[i+1]-values[i])*1e-7
                q = np.zeros((193, 2)); q[:, axis] = values[i]-epsilon
                other=(tu,tv)[1-axis];order=degree[1-axis]
                q[:, 1-axis] = np.linspace(other[order],other[-order-1],len(q))
                r = q.copy(); r[:, axis] += 2*epsilon
                def normal(uv):
                    du = spline_value(p, tu, tv, uv, 1, 0, degree); dv = spline_value(p, tu, tv, uv, 0, 1, degree)
                    n = np.cross(du, dv); return n/np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-30)
                records.append(dict(patch=fi, axis=axis, knot=float(values[i]), nominal_continuity=int(degree[axis]-multiplicity[i]),
                                    normal_jump_degrees=stats(angles(normal(q), normal(r))),
                                    curvature_jump_per_mm=stats(abs(curvature(p, tu, tv, degree, q)-curvature(p, tu, tv, degree, r)))))
    return dict(knot_records=records, maximum_sampled_normal_jump_degrees=max((r['normal_jump_degrees']['max'] for r in records if 'normal_jump_degrees' in r), default=0.),
                note='Cubic simple knots are C2 by construction. Reported finite-offset jumps at repeated knots are sampled diagnostics, not a continuous-domain proof.')


def sample(shape,count,seed):
    points,tri,uv,owners=tessellate(shape)
    area=np.linalg.norm(np.cross(points[tri[:,1]]-points[tri[:,0]],points[tri[:,2]]-points[tri[:,0]]),axis=1)
    rng=np.random.default_rng(seed);chosen=rng.choice(len(tri),count,p=area/area.sum())
    r=np.sqrt(rng.random(count));s=rng.random(count)
    q=np.einsum('ij,ijk->ik',np.c_[1-r,r*(1-s),r*s],uv[tri[chosen]])
    p=np.empty((count,3));n=np.empty_like(p)
    for fi,face in enumerate(shapes(shape,TopAbs_FACE)):
        mask=owners[chosen]==fi
        if not mask.any():continue
        p[mask],n[mask]=evaluate(BRep_Tool.Surface_s(face),q[mask])
        if face.Orientation()==TopAbs_REVERSED:n[mask]*=-1
    return p,n


def representation(shape):
    """Count native STEP supports, never the clamped evaluation copies."""
    from OCP.BRepAdaptor import BRepAdaptor_Curve2d
    from OCP.TopAbs import TopAbs_FORWARD
    faces=shapes(shape,TopAbs_FACE)
    supports=[surface_info(BRep_Tool.Surface_s(f)) for f in faces]
    curve_controls=0;pcurve_controls=0;curve_types={};pcurve_types={}
    for edge in shapes(shape,TopAbs_EDGE):
        if BRep_Tool.Degenerated_s(edge):continue
        first,last=BRep_Tool.Range_s(edge);curve=BRep_Tool.Curve_s(edge,first,last)
        kind=type(curve).__name__;curve_types[kind]=curve_types.get(kind,0)+1
        if hasattr(curve,'NbPoles'):curve_controls+=curve.NbPoles()
    for face in faces:
        for edge in shapes(face,TopAbs_EDGE):
            orientations=(TopAbs_FORWARD,TopAbs_REVERSED) if BRep_Tool.IsClosed_s(edge,face) else (edge.Orientation(),)
            for orientation in orientations:
                pc=BRepAdaptor_Curve2d(TopoDS.Edge_s(edge.Oriented(orientation)),face)
                kind=str(pc.GetType());pcurve_types[kind]=pcurve_types.get(kind,0)+1
                if 'BSplineCurve' in kind:pcurve_controls+=pc.BSpline().NbPoles()
                elif 'BezierCurve' in kind:pcurve_controls+=pc.Bezier().NbPoles()
    return dict(faces=len(faces),surfaces=supports,
        actual_support_control_points=sum(s.get('u_poles',0)*s.get('v_poles',0) for s in supports),
        maximum_direction_poles=max((max(s.get('u_poles',0),s.get('v_poles',0)) for s in supports),default=0),
        unique_3d_curve_controls=curve_controls,pcurve_controls=pcurve_controls,
        curve_types=curve_types,pcurve_types=pcurve_types)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',required=True,help='Original selected STEP region.')
    parser.add_argument('--candidate',required=True);parser.add_argument('--output',required=True)
    parser.add_argument('--count',type=int,default=30000);parser.add_argument('--reverse-count',type=int,default=12000)
    parser.add_argument('--seed',type=int,default=260930179)
    args=parser.parse_args();out=Path(args.output)
    if out.exists():raise FileExistsError(out)
    started=time.perf_counter();reference=read_step(args.reference);candidate=read_step(args.candidate)
    read_seconds=time.perf_counter()-started;t=time.perf_counter()
    source_points,source_normals=sample(reference,args.count,args.seed)
    reverse_points,_=sample(candidate,args.reverse_count,args.seed+1);sampling_seconds=time.perf_counter()-t
    t=time.perf_counter();forward,_=distances(source_points,candidate,source_normals)
    forward_seconds=time.perf_counter()-t;t=time.perf_counter();reverse,_=distances(reverse_points,reference)
    reverse_seconds=time.perf_counter()-t;t=time.perf_counter()
    seams=seam_quality(candidate);inside=continuity(candidate);edges=edge_checks(candidate,385)
    costs=representation(candidate);topo=topology(candidate)
    result=dict(status='DiagnosticOnly',can_apply=False,reference=str(Path(args.reference).resolve()),
        candidate=str(Path(args.candidate).resolve()),seed=args.seed,used_for_tuning=False,
        forward=forward,reverse=reverse,seams=seams,interior_continuity=inside,edge_checks=edges,
        topology=topo,representation=costs,local_step_bytes=Path(args.candidate).stat().st_size,
        direction_budget_pass=costs['maximum_direction_poles']<=25,
        stages=dict(step_read=read_seconds,sampling=sampling_seconds,forward=forward_seconds,
                    reverse=reverse_seconds,topology_and_representation=time.perf_counter()-t),
        wall_seconds=time.perf_counter()-started,
        note='Sampled independent geometry audit; does not replace original-interface/untouched-region audit or Creo. No model is modified.')
    out.parent.mkdir(parents=True,exist_ok=True);dump(out,result)
    print('DiagnosticOnly',costs['faces'],costs['actual_support_control_points'],forward['distance_mm']['max'],reverse['distance_mm']['max'],flush=True)


if __name__=='__main__':main()
