"""Optional exact support merging; no layout search or fitting policy.

Joining two faces removes a topological seam and joins the actual pole nets.
Original boundary curves are reused. A closed support keeps one two-pcurve
seam; it is not mislabeled as an open rectangular face. No tolerance repair.
"""
from dataclasses import dataclass
import time

from geometry import *
from spline import array1, from_occt, to_occt
from OCP.BRepTools import BRepTools, BRepTools_WireExplorer
from OCP.BRepBuilderAPI import BRepBuilderAPI_Copy, BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.Geom2d import Geom2d_BSplineCurve
from OCP.TColgp import TColgp_Array1OfPnt2d
from OCP.gp import gp_Pnt2d
from OCP.TopAbs import TopAbs_FORWARD, TopAbs_WIRE


@dataclass
class Cell:
    surface: object
    maps: dict
    eligible: bool = True


def line_pcurve(first, last, uv):
    poles = TColgp_Array1OfPnt2d(1, 2)
    for i, p in enumerate(uv):
        poles.SetValue(i+1, gp_Pnt2d(*p))
    return Geom2d_BSplineCurve(poles, array1([first, last]), array1([2, 2], True), 1)


def pcurve_samples(edge, face, count=17):
    first, last = BRep_Tool.Range_s(edge, face)
    curve = BRep_Tool.CurveOnSurface_s(edge, face, first, last)
    points = np.array([[curve.Value(float(t)).X(), curve.Value(float(t)).Y()]
                       for t in np.linspace(first, last, count)])
    return first, last, points


def rectangle_side(uv, tol=1e-8):
    for axis in (0, 1):
        for side in (0, 1):
            if np.max(abs(uv[:, axis]-side)) <= tol:
                return axis, side
    return None


def initial_cells(shape):
    result = []
    for fi, face in enumerate(shapes(shape, TopAbs_FACE)):
        surface = BRep_Tool.Surface_s(face)
        try:
            p, tu, tv, degree = from_occt(surface)
            lo = np.array([tu[0], tv[0]]); size = np.array([tu[-1]-tu[0], tv[-1]-tv[0]])
            matrix = np.diag(1/size); offset = -lo/size
            normalized = to_occt(p, (tu-tu[0])/size[0], (tv-tv[0])/size[1], degree)
            intervals = {(a, b): [] for a in (0, 1) for b in (0, 1)}
            eligible = True
            for edge in shapes(face, TopAbs_EDGE):
                _, _, uv = pcurve_samples(edge, face)
                uv = uv@matrix.T+offset; side = rectangle_side(uv)
                if side is None:
                    eligible = False; break
                if np.max(abs(uv-np.linspace(uv[0], uv[-1], len(uv)))) > 1e-8:
                    eligible = False; break
                intervals[side].append(sorted(uv[[0, -1], 1-side[0]]))
            for spans in intervals.values():
                cursor = 0.
                for a, b in sorted(spans):
                    if abs(a-cursor) > 1e-8: eligible = False
                    cursor = b
                if abs(cursor-1) > 1e-8: eligible = False
            result.append(Cell(normalized, {fi: (matrix, offset)}, eligible))
        except (ValueError, AttributeError):
            result.append(Cell(surface, {fi: (np.eye(2), np.zeros(2))}, False))
    return result


def orient(cell, swap, reverse_u, reverse_v):
    p, tu, tv, degree = from_occt(cell.surface)
    matrix = np.eye(2); offset = np.zeros(2)
    if swap:
        p = p.transpose(1, 0, 2); tu, tv = tv, tu; degree = degree[::-1]
        matrix = matrix[::-1]; offset = offset[::-1]
    if reverse_u:
        p = p[::-1]; tu = 1-tu[::-1]; matrix[0] *= -1; offset[0] = 1-offset[0]
    if reverse_v:
        p = p[:, ::-1]; tv = 1-tv[::-1]; matrix[1] *= -1; offset[1] = 1-offset[1]
    maps = {fi: (matrix@a, matrix@b+offset) for fi, (a, b) in cell.maps.items()}
    return Cell(to_occt(p, tu, tv, degree), maps)


def orientations(cell):
    for swap in (False, True):
        for ru in (False, True):
            for rv in (False, True):
                yield orient(cell, swap, ru, rv)


def join(a, b, tolerance=1e-9):
    """Return an exact join if two complete isoparametric sides coincide."""
    if not a.eligible or not b.eligible:
        return None
    t = np.linspace(0, 1, 33); qa = np.c_[t*0+1, t]; qb = np.c_[t*0, t]
    aa = list(orientations(a)); bb = list(orientations(b))
    av = [evaluate(c.surface, qa)[0] for c in aa]
    bv = [evaluate(c.surface, qb)[0] for c in bb]
    options = []
    for i, ap in enumerate(av):
        for j, bp in enumerate(bv):
            if np.linalg.norm(ap-bp, axis=1).max() > tolerance:
                continue
            sa = aa[i].surface.Copy(); sb = bb[j].surface.Copy()
            pu = max(sa.UDegree(), sb.UDegree()); pv = max(sa.VDegree(), sb.VDegree())
            sa.IncreaseDegree(pu, pv); sb.IncreaseDegree(pu, pv)
            vocabulary = {}
            for s in (sa, sb):
                for k in range(2, s.NbVKnots()):
                    key = round(s.VKnot(k), 12)
                    vocabulary[key] = max(vocabulary.get(key, 0), s.VMultiplicity(k))
            for value, mult in sorted(vocabulary.items()):
                sa.InsertVKnot(value, mult, 1e-10, False); sb.InsertVKnot(value, mult, 1e-10, False)
            pa, ua, va, da = from_occt(sa); pb, ub, vb, db = from_occt(sb)
            if pa.shape[1] != pb.shape[1] or np.max(abs(va-vb)) > 1e-9:
                continue
            error = float(np.linalg.norm(pa[-1]-pb[0], axis=1).max())
            if error > tolerance:
                continue
            # Equal parameter width per constituent keeps repeated agglomeration
            # independent of the order of pair selection.
            split = len(a.maps)/(len(a.maps)+len(b.maps))
            poles = np.concatenate([pa, pb[1:]], axis=0)
            u = np.r_[ua[:-1]*split, split+ub[pu+1:]*(1-split)]
            maps = {}
            for group, scale, shift in ((aa[i], split, 0.), (bb[j], 1-split, split)):
                transform = np.diag([scale, 1.]); translation = np.array([shift, 0.])
                maps.update({fi: (transform@m, transform@o+translation) for fi, (m, o) in group.maps.items()})
            merged = Cell(to_occt(poles, u, va, da), maps)
            options.append((poles.shape[0]*poles.shape[1], merged, error))
    if not options:
        return None
    _, merged, error = min(options, key=lambda r: r[0])
    return merged, dict(join_pole_disagreement_mm=error,
                       before_controls=sum(c.surface.NbUPoles()*c.surface.NbVPoles() for c in (a,b)),
                       after_controls=merged.surface.NbUPoles()*merged.surface.NbVPoles(),
                       closed_u=bool(merged.surface.IsUClosed()))


def export_cells(original, cells):
    """Reuse source edges and build the true merged rectangular/closed face."""
    source_faces = shapes(original, TopAbs_FACE)
    copier = BRepBuilderAPI_Copy(original, True, False)
    builder = BRep_Builder(); result = []; records = []
    for cell in cells:
        if len(cell.maps) == 1:
            result.append(TopoDS.Face_s(copier.ModifiedShape(source_faces[next(iter(cell.maps))])))
            continue
        uses = []
        for fi, (matrix, offset) in cell.maps.items():
            face = source_faces[fi]
            for edge in shapes(face, TopAbs_EDGE):
                first, last, uv = pcurve_samples(edge, face)
                uv = uv@matrix.T+offset; side = rectangle_side(uv)
                if side is None: continue
                if np.max(abs(uv-np.linspace(uv[0], uv[-1], len(uv)))) > 1e-8:
                    raise ValueError('nonlinear_retained_pcurve')
                axis, value = side; increasing = (axis == 1 and value == 0) or (axis == 0 and value == 1)
                forward = (uv[-1, 1-axis] > uv[0, 1-axis]) == increasing
                mid = uv.mean(axis=0)
                key = (mid[0] if side == (1,0) else 1+mid[1] if side == (0,1)
                       else 3-mid[0] if side == (1,1) else 4-mid[1])
                copied = TopoDS.Edge_s(copier.ModifiedShape(edge)).Oriented(TopAbs_FORWARD)
                uses.append(dict(edge=TopoDS.Edge_s(copied), pc=line_pcurve(first, last, uv[[0,-1]]),
                                 forward=bool(forward), key=float(key)))
        uses.sort(key=lambda r:r['key'])
        grouped = []
        for use in uses:
            group = next((g for g in grouped if g[0]['edge'].IsSame(use['edge'])), None)
            if group is None: grouped.append([use])
            else: group.append(use)
        for group in grouped:
            edge = group[0]['edge']; tol = BRep_Tool.Tolerance_s(edge)
            if len(group) == 1:
                builder.UpdateEdge(edge, group[0]['pc'], cell.surface, TopLoc_Location(), tol)
            elif len(group) == 2:
                if group[0]['forward'] == group[1]['forward']:
                    raise ValueError('closed_seam_orientation_conflict')
                group.sort(key=lambda r:not r['forward'])
                builder.UpdateEdge(edge, group[0]['pc'], group[1]['pc'], cell.surface, TopLoc_Location(), tol)
            else:
                raise ValueError('nonmanifold_merged_edge')
        wire = BRepBuilderAPI_MakeWire()
        for use in uses:
            wire.Add(TopoDS.Edge_s(use['edge'].Oriented(TopAbs_FORWARD if use['forward'] else TopAbs_REVERSED)))
        if not wire.IsDone(): raise ValueError('merged_wire_not_connected')
        face = BRepBuilderAPI_MakeFace(cell.surface, wire.Wire(), True).Face()
        fi = next(iter(cell.maps)); matrix, offset = cell.maps[fi]
        old = source_faces[fi]; bounds = BRepTools.UVBounds_s(old)
        q = np.array([[(bounds[0]+bounds[1])/2, (bounds[2]+bounds[3])/2]])
        _, before = evaluate(BRep_Tool.Surface_s(old), q)
        if old.Orientation() == TopAbs_REVERSED: before *= -1
        _, after = evaluate(cell.surface, q@matrix.T+offset)
        if np.sum(before*after) < 0: face = TopoDS.Face_s(face.Reversed())
        if not BRepCheck_Analyzer(face).IsValid(): raise ValueError('merged_face_invalid')
        result.append(face)
        records.append(dict(members=sorted(cell.maps), dimensions=(cell.surface.NbUPoles(),cell.surface.NbVPoles()),
                            edge_uses=len(uses), closed_seams=sum(len(g)==2 for g in grouped)))
    return shell(result), records


def shared_pairs(cells, original):
    adjacency = set()
    for edge, uses in edge_uses(shapes(original, TopAbs_FACE)):
        if len(uses) == 2: adjacency.add(tuple(sorted(uses)))
    for i, a in enumerate(cells):
        for j in range(i+1,len(cells)):
            b = cells[j]
            if any(tuple(sorted((x,y))) in adjacency for x in a.maps for y in b.maps):
                yield i,j


def compact(original, directory, maximum_candidates=64, seconds=120., protected_edges=()):
    """Greedy exact face reduction with measured STEP-byte non-increase.

    Each accepted join reduces face count. A blocked pair stays separate. The
    comparator is the written input, and the timing includes all rejected joins.
    """
    directory = Path(directory); directory.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); cells = initial_cells(original); records = []
    file = directory/'candidate.step'; write_step(file, original); current_bytes = file.stat().st_size
    input_bytes = current_bytes; selected = original; stop = 'no_economic_exact_join'; stages = dict(proposal=0., brep=0., step_write=0.)
    protected = set(protected_edges); rejected_pairs=set(); attempts=0
    forbidden_pairs = set()
    for eid, (edge, uses) in enumerate(edge_uses(shapes(original,TopAbs_FACE))):
        if eid in protected and len(uses)==2: forbidden_pairs.add(tuple(sorted(uses)))
    while len(records) < maximum_candidates and time.perf_counter()-started < seconds:
        proposals = []
        pairs=sorted(shared_pairs(cells,original),key=lambda ij:-(len(cells[ij[0]].maps)+len(cells[ij[1]].maps)))
        for i,j in pairs:
            if len(proposals)>=4:break
            if len(records)+len(proposals) >= maximum_candidates or time.perf_counter()-started >= seconds: break
            key=tuple(sorted((tuple(sorted(cells[i].maps)),tuple(sorted(cells[j].maps)))))
            if key in rejected_pairs:continue
            if any(tuple(sorted((a,b))) in forbidden_pairs for a in cells[i].maps for b in cells[j].maps): continue
            attempts+=1;t=time.perf_counter(); merged=join(cells[i],cells[j]); stages['proposal']+=time.perf_counter()-t
            if merged is None:
                rejected_pairs.add(key);records.append(dict(members=[list(v) for v in key],status='not_parameter_compatible'));continue
            cell, report=merged
            trial=[c for k,c in enumerate(cells) if k not in (i,j)]+[cell]
            before=sum(c.surface.NbUPoles()*c.surface.NbVPoles() for c in (cells[i],cells[j]))
            if report['after_controls']>before:
                rejected_pairs.add(key);records.append(dict(members=sorted(cell.maps),status='control_inflation',**report));continue
            proposals.append((trial,cell,report))
        best=None
        for trial,cell,report in proposals:
            if len(records)>=maximum_candidates or time.perf_counter()-started>=seconds:break
            record=dict(members=sorted(cell.maps),faces=len(trial),**report)
            try:
                t=time.perf_counter(); shape,topo=export_cells(original,trial); stages['brep']+=time.perf_counter()-t
                t=time.perf_counter(); write_step(file,shape); size=file.stat().st_size; stages['step_write']+=time.perf_counter()-t
                record.update(step_bytes=size,previous_step_bytes=current_bytes,closed_supports=topo)
                record['status']='eligible' if size<=current_bytes else 'file_growth_rejected'
                # Grow complete strips before starting unrelated dominoes;
                # this avoids trapping a regular grid in incompatible T ends.
                key=(-len(cell.maps),size)
                if size<=current_bytes and (best is None or key<best[0]):best=(key,size,shape,trial)
            except (ValueError,RuntimeError) as error:
                record.update(status='brep_rejected',reason=str(error))
            records.append(record)
        if best is None:break
        _,current_bytes,selected,cells=best
    if len(records)>=maximum_candidates: stop='candidate_limit'
    elif time.perf_counter()-started>=seconds:stop='time_limit'
    file.unlink(missing_ok=True)
    return selected,dict(input_faces=len(shapes(original,TopAbs_FACE)),faces=len(cells),input_step_bytes=input_bytes,
                         step_bytes=current_bytes,candidates=records,attempted_joins=attempts,stop_reason=stop,stages=stages,
                         wall_seconds=time.perf_counter()-started)
