"""Create one shared trim edge per chain and assemble fitted mesh partitions.

Only generated supports and original mesh polylines enter this module. A trim
is projected analytically into each affine graph chart, preserving its original
parameter exactly. Edge tolerance records measured support residuals, with a
fixed upper limit; sewing tolerance is never increased to close gaps.
"""
import time
from pathlib import Path
import numpy as np

from . import cad
from .mesh import dump, stats
from .partition_fit import parameters, positions, surface_poles, polyline_graph_error, spline_graph_error


def shared_chains(edges, owners, face_chart):
    chart_pair = np.sort(face_chart[owners], axis=1)
    selected = np.flatnonzero(chart_pair[:, 0] != chart_pair[:, 1])
    links = {}
    for index in selected:
        for vertex in edges[index]:
            links.setdefault(int(vertex), []).append(int(index))
    seen = set(); chains = []; chain_owners = []

    def terminal(vertex):
        use = links[vertex]
        return len(use) != 2 or not np.array_equal(chart_pair[use[0]], chart_pair[use[1]])

    def walk(start, index):
        path = [start]; here = start; pair = chart_pair[index].copy()
        while index not in seen:
            seen.add(index); a, b = edges[index]
            here = int(b if a == here else a); path.append(here)
            if terminal(here) or here == start:
                break
            index = next(item for item in links[here] if item != index)
        if path[0] == path[-1]:
            # Two ordinary edges avoid a closed edge's special pcurve semantics.
            mid = (len(path)-1)//2
            chains.extend([np.array(path[:mid+1]), np.array(path[mid:])])
            chain_owners.extend([pair, pair])
        else:
            chains.append(np.array(path)); chain_owners.append(pair)

    for vertex in sorted(links):
        if terminal(vertex):
            for index in links[vertex]:
                if index not in seen:
                    walk(vertex, index)
    for index in selected:
        if int(index) not in seen:
            walk(int(edges[index, 0]), int(index))
    if len(seen) != len(selected):
        raise ValueError('Shared network coverage is incomplete')
    return chains, chain_owners


def array(values, integer=False):
    from OCP.TColStd import TColStd_Array1OfInteger, TColStd_Array1OfReal
    result = (TColStd_Array1OfInteger if integer else TColStd_Array1OfReal)(1, len(values))
    for i, value in enumerate(values):
        result.SetValue(i+1, int(value) if integer else float(value))
    return result


def polyline(points, t):
    from OCP.Geom import Geom_BSplineCurve
    from OCP.Geom2d import Geom2d_BSplineCurve
    from OCP.TColgp import TColgp_Array1OfPnt, TColgp_Array1OfPnt2d
    from OCP.gp import gp_Pnt, gp_Pnt2d
    spatial = points.shape[1] == 3
    ps = (TColgp_Array1OfPnt if spatial else TColgp_Array1OfPnt2d)(1, len(points))
    for i, point in enumerate(points):
        ps.SetValue(i+1, (gp_Pnt if spatial else gp_Pnt2d)(*point))
    multiplicities = np.ones(len(t), dtype=int); multiplicities[[0, -1]] = 2
    return (Geom_BSplineCurve if spatial else Geom2d_BSplineCurve)(ps, array(t), array(multiplicities, True), 1)


def projected_curve(curve, rec):
    from OCP.Geom2d import Geom2d_BSplineCurve
    from OCP.TColgp import TColgp_Array1OfPnt2d
    from OCP.gp import gp_Pnt2d
    poles = parameters(np.array([curve.Pole(i).Coord() for i in range(1, curve.NbPoles()+1)]), rec)
    points = TColgp_Array1OfPnt2d(1, len(poles))
    for i, p in enumerate(poles):
        points.SetValue(i+1, gp_Pnt2d(*p))
    knots = [curve.Knot(i) for i in range(1, curve.NbKnots()+1)]
    multi = [curve.Multiplicity(i) for i in range(1, curve.NbKnots()+1)]
    return Geom2d_BSplineCurve(points, array(knots), array(multi, True), curve.Degree())


def topology(shape):
    from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_SOLID, TopAbs_SHELL
    from OCP.TopTools import TopTools_IndexedMapOfShape
    from OCP.BRep import BRep_Tool
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.BRepGProp import BRepGProp
    from OCP.GProp import GProp_GProps
    faces = cad.shapes(shape, TopAbs_FACE); indexed = TopTools_IndexedMapOfShape(); uses = []
    for face in faces:
        for edge in cad.shapes(face, TopAbs_EDGE):
            i = indexed.Add(edge)-1
            if i == len(uses):
                uses.append(0)
            if not BRep_Tool.Degenerated_s(edge):
                uses[i] += 2 if BRep_Tool.IsClosed_s(edge, face) else 1
    edges = cad.shapes(shape, TopAbs_EDGE)
    volume = GProp_GProps(); BRepGProp.VolumeProperties_s(shape, volume)
    analyzer = BRepCheck_Analyzer(shape, True)
    invalid = [i for i, face in enumerate(faces) if not analyzer.IsValid(face)]
    return dict(faces=len(faces), edges=len(edges), shells=len(cad.shapes(shape, TopAbs_SHELL)),
                solids=len(cad.shapes(shape, TopAbs_SOLID)), free_edges=sum(n == 1 for n in uses),
                multiple_edges=sum(n > 2 for n in uses), invalid_faces=invalid,
                brep_valid=analyzer.IsValid(), volume_mm3=volume.Mass(),
                maximum_edge_tolerance_mm=max((BRep_Tool.Tolerance_s(e) for e in edges), default=0.))


def checked_curve_errors(records, curves, certificates):
    """Reuse same-process extrema only after direct geometry-array equality."""
    if certificates is None:
        return None
    if len(records) != len(certificates['records']) or len(curves) != len(certificates['curves']):
        raise ValueError('Curve/support certificate count changed')
    for di, rec in enumerate(records):
        prior = certificates['records'][di]
        if any(not np.array_equal(rec[k], prior[k]) for k in ('origin', 'axes', 'lo', 'size', 'knots', 'height')):
            raise ValueError(f'Curve/support certificate geometry changed: chart {di}')
    errors = []
    for ci, curve in enumerate(curves):
        prior = certificates['curves'][ci]
        if any(not np.array_equal(curve[k], prior[k]) for k in ('poles', 'knots', 'parameters', 'chain', 'owners')):
            raise ValueError(f'Curve/support certificate geometry changed: curve {ci}')
        value = np.asarray(certificates['errors'][ci], dtype=float)
        if value.shape != (2,) or not np.isfinite(value).all() or np.any(value < 0):
            raise ValueError('Invalid continuous curve/support error certificate')
        errors.append(value.tolist())
    return errors


def assemble(v, triangles, records, face_chart, mesh_edges, mesh_owners, tolerance, out,
             identity_topology=False, segment_edges=False, smooth_boundary=0., skip_smooth_faces=(),
             write_preview=True, smooth_segments=False, shared_curve_records=None, maximum_edge_length=None,
             curve_certificates=None, native_context=None, binary_native=False):
    from OCP.BRep import BRep_Builder, BRep_Tool
    from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeEdge,
                                   BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeSolid)
    from OCP.BRepLib import BRepLib
    from OCP.TopoDS import TopoDS, TopoDS_Shell, TopoDS_Wire, TopoDS_Face
    from OCP.TopLoc import TopLoc_Location
    from OCP.gp import gp_Pnt
    start = time.perf_counter(); out = Path(out)
    chains, owners = shared_chains(mesh_edges, mesh_owners, face_chart)
    if shared_curve_records is not None:
        if smooth_boundary or segment_edges or not identity_topology:
            raise ValueError('A fitted shared skeleton requires its original canonical curve geometry')
        chains = [r['chain'] for r in shared_curve_records]
        owners = [r['owners'] for r in shared_curve_records]
    if smooth_boundary:
        if segment_edges or not identity_topology:
            raise ValueError('Smooth shared curves require canonical chain topology')
        from .joint_graph_boundary import extend_support
        from .regional_rectangle import compact_curve
        for rec in records:
            extend_support(rec, smooth_boundary*1.1)
    if segment_edges:
        pieces = [(np.array([a, b]), pair) for chain, pair in zip(chains, owners)
                  for a, b in zip(chain[:-1], chain[1:])]
        chains = [p for p, _ in pieces]; owners = [pair for _, pair in pieces]
    smooth_chain = [bool(smooth_boundary or shared_curve_records is not None)]*len(chains)
    certified_errors = checked_curve_errors(records, shared_curve_records, curve_certificates)
    if smooth_boundary and skip_smooth_faces:
        skip = set(skip_smooth_faces); pieces = []
        for chain, pair in zip(chains, owners):
            if any(int(fi) in skip for fi in pair):
                pieces.extend((np.array([a, b]), pair, False) for a, b in zip(chain[:-1], chain[1:]))
            else:
                pieces.append((chain, pair, True))
        chains = [p for p, _, _ in pieces]; owners = [pair for _, pair, _ in pieces]
        smooth_chain = [smooth for _, _, smooth in pieces]
    surfaces = [cad.surface(surface_poles(r), r['knots'], r['knots']) for r in records]
    builder = BRep_Builder(); vertices = {}; edges = []; lookup = {}; checks = []
    boundary_max = 0.
    for ci, (chain, pair) in enumerate(zip(chains, owners)):
        p = v[chain]; t = np.r_[0., np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]; t /= t[-1]
        curve_deviation = 0.
        smooth_errors = None; local_smoothing = 0.
        if shared_curve_records is not None:
            from .compact import curve as cubic_curve
            master = shared_curve_records[ci]; t = master['parameters']
            curve = cubic_curve(master['poles'], master['knots'])
            smooth_errors = (certified_errors[ci] if certified_errors is not None else
                             [spline_graph_error(master['poles'], master['knots'], records[int(fi)]) for fi in pair])
            curve_deviation = float(master.get('movement_mm', 0.))
        elif smooth_chain[ci]:
            for fraction in [1., .5, .25, .125, .0625, .015625]:
                local_smoothing = smooth_boundary*fraction
                curve, curve_deviation = compact_curve(p, t, local_smoothing, allow_rounded=True)
                curve_poles = np.array([curve.Pole(i).Coord() for i in range(1, curve.NbPoles()+1)])
                curve_knots = np.repeat([curve.Knot(i) for i in range(1, curve.NbKnots()+1)],
                                        [curve.Multiplicity(i) for i in range(1, curve.NbKnots()+1)])
                smooth_errors = [spline_graph_error(curve_poles, curve_knots, records[int(fi)]) for fi in pair]
                if max(smooth_errors) <= tolerance*.9:
                    break
        else:
            curve = polyline(p, t)
        errors = []; pcurves = []
        w = np.linspace(0., 1., 9)
        probe = (p[:-1, None]*(1-w[None, :, None])+p[1:, None]*w[None, :, None]).reshape(-1, 3)
        for side, fi in enumerate(pair):
            rec = records[int(fi)]; uv = parameters(p, rec)
            err = np.linalg.norm(positions(parameters(probe, rec), rec)-probe, axis=1)
            maximum = (smooth_errors[side] if smooth_chain[ci] else
                       polyline_graph_error(p, rec) if identity_topology else float(err.max()))
            errors.append(maximum); boundary_max = max(boundary_max, maximum)
            if maximum > tolerance*.9:
                dump(out/'boundary_failure.json', dict(curve=ci, chart=int(fi), maximum=maximum, tolerance=tolerance))
                raise ValueError(f'Curve {ci}/chart {fi} exceeds boundary tolerance: {maximum}')
            pcurves.append(projected_curve(curve, rec) if smooth_chain[ci] else polyline(uv, t))
        etol = max(1e-7, max(errors)*1.05)
        parts = [(j, j+1) for j in range(len(chain)-1)] if smooth_segments else [(0, len(chain)-1)]
        if maximum_edge_length is not None and not smooth_segments:
            if maximum_edge_length <= 0:
                raise ValueError('Maximum edge length must be positive')
            length = np.linalg.norm(np.diff(p, axis=0), axis=1).sum()
            cuts = np.unique(np.r_[0, np.searchsorted(t, np.linspace(0., 1., max(2, int(np.ceil(length/maximum_edge_length))+1))), len(chain)-1])
            cuts = cuts[cuts < len(chain)]
            parts = [(int(a), int(b)) for a, b in zip(cuts[:-1], cuts[1:])]
        for first, last in parts:
            for j in (first, last):
                vid = int(chain[j])
                if vid not in vertices:
                    vertices[vid] = BRepBuilderAPI_MakeVertex(curve.Value(float(t[j]))).Vertex()
                vertex = vertices[vid]
                if BRep_Tool.Pnt_s(vertex).Distance(curve.Value(float(t[j]))) > 1e-8:
                    raise ValueError(f'Inconsistent canonical boundary junction at source vertex {vid}')
                builder.UpdateVertex(vertex, max(BRep_Tool.Tolerance_s(vertex), etol))
            edge_curve = curve
            edge_pcurves = pcurves
            if len(parts) > 1 and len(chain) > 2:
                # Preserve the same C2 geometry while storing only this span's
                # basis. Retaining an entire chain per edge explodes STEP size.
                edge_curve = curve.Copy(); edge_curve.Segment(float(t[first]), float(t[last]))
                edge_pcurves = [pc.Copy() for pc in pcurves]
                for pc in edge_pcurves:
                    pc.Segment(float(t[first]), float(t[last]))
            maker = BRepBuilderAPI_MakeEdge(edge_curve, vertices[int(chain[first])], vertices[int(chain[last])],
                                           float(t[first]), float(t[last]))
            if not maker.IsDone():
                raise ValueError(f'Cannot construct shared curve {ci}/{first}')
            edge = maker.Edge()
            for side, fi in enumerate(pair):
                builder.UpdateEdge(edge, edge_pcurves[side], surfaces[int(fi)], TopLoc_Location(),
                                   max(1e-7, errors[side]*1.05))
            builder.Range(edge, float(t[first]), float(t[last]))
            builder.SameRange(edge, True); builder.SameParameter(edge, True)
            edge_id = len(edges); edges.append(edge)
            checks.append(dict(curve=edge_id, source_chain=ci, owners=pair.tolist(), source_segments=last-first,
                               support_error_mm=errors, source_polyline_deviation_mm=curve_deviation,
                               smoothing_budget_mm=local_smoothing, degree=curve.Degree(), smoothed=smooth_chain[ci]))
            for a, b in zip(chain[first:last], chain[first+1:last+1]):
                key = tuple(sorted((int(a), int(b))))
                if key in lookup:
                    raise ValueError('Repeated source edge in shared network')
                lookup[key] = (edge_id, int(a))
    from .source_shells import layout as shell_layout, assemble as assemble_shells
    source_component, shell_records = shell_layout(v, triangles, mesh_owners)
    shells = [TopoDS_Shell() for _ in shell_records]
    from OCP.TopTools import TopTools_IndexedMapOfShape
    chart_faces = TopTools_IndexedMapOfShape()
    for shell in shells:
        builder.MakeShell(shell)
    for fi, rec in enumerate(records):
        wires = []
        for loop in rec['loops']:
            references = []
            for a, b in zip(loop, np.roll(loop, -1)):
                ci, start_vertex = lookup[tuple(sorted((int(a), int(b))))]
                entry = (ci, int(a) == start_vertex)
                if not references or references[-1] != entry:
                    references.append(entry)
            if len(references) > 1 and references[0] == references[-1]:
                references.pop()
            if identity_topology:
                wire_value = TopoDS_Wire(); builder.MakeWire(wire_value)
                for ci, forward in references:
                    builder.Add(wire_value, edges[ci] if forward else TopoDS.Edge_s(edges[ci].Reversed()))
                wire_value.Closed(True)
            else:
                wire = BRepBuilderAPI_MakeWire()
                for ci, forward in references:
                    wire.Add(edges[ci] if forward else TopoDS.Edge_s(edges[ci].Reversed()))
                if not wire.IsDone():
                    raise ValueError(f'Disconnected shared wire on chart {fi}')
                wire_value = wire.Wire()
            uv = parameters(v[loop], rec)
            signed_area = float(np.sum(uv[:, 0]*np.roll(uv[:, 1], -1)-uv[:, 1]*np.roll(uv[:, 0], -1))/2)
            wires.append((signed_area, wire_value))
        wires.sort(key=lambda x: -x[0])
        if wires[0][0] <= 0 or any(area >= 0 for area, _ in wires[1:]):
            raise ValueError(f'Chart {fi} is not one oriented graph domain with holes')
        if identity_topology:
            face_value = TopoDS_Face(); builder.MakeFace(face_value, surfaces[fi], TopLoc_Location(), 1e-7)
            for _, wire in wires:
                builder.Add(face_value, wire)
        else:
            face = BRepBuilderAPI_MakeFace(surfaces[fi], wires[0][1], True)
            for _, wire in wires[1:]:
                face.Add(wire)
            if not face.IsDone():
                raise ValueError(f'Face construction failed for chart {fi}')
            face_value = face.Face()
        components = np.unique(source_component[rec['triangle_ids']])
        if len(components) != 1:
            raise ValueError('A fitting chart crosses separate source shells')
        builder.Add(shells[int(components[0])], face_value)
        chart_faces.Add(face_value)
    for shell in shells:
        shell.Closed(True)
    solid, expected_solids = assemble_shells(shells, shell_records)
    from OCP.TopAbs import TopAbs_FACE
    cad_face_charts = [chart_faces.FindIndex(face)-1 for face in cad.shapes(solid, TopAbs_FACE)]
    if sorted(cad_face_charts) != list(range(len(records))):
        raise ValueError('Assembled face identities do not match the original chart records')
    dump(out/'source_shells.json', dict(expected_solids=expected_solids, shells=[
        {k: val.tolist() if isinstance(val, np.ndarray) else val
         for k, val in r.items() if k not in ('triangles', 'vertices')} for r in shell_records]))
    construction_seconds = time.perf_counter()-start
    tick = time.perf_counter(); before = topology(solid); check_seconds = time.perf_counter()-tick
    if identity_topology:
        from OCP.BRepTools import BRepTools
        from OCP.BRepCheck import BRepCheck_Analyzer, BRepCheck_NoError
        from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_WIRE
        if binary_native:
            from OCP.BinTools import BinTools
            if not BinTools.Write_s(solid, str(out/'native.binbrep')):
                raise ValueError('Cannot persist binary native assembly')
        else:
            BRepTools.Write_s(solid, str(out/'native.brep'))
        analyzer = BRepCheck_Analyzer(solid, True) if before['invalid_faces'] else None
        failures = []
        for fi in before['invalid_faces']:
            face = cad.shapes(solid, TopAbs_FACE)[fi]
            if analyzer.IsValid(face):
                continue
            issues = []
            for kind, name in [(TopAbs_FACE, 'face'), (TopAbs_WIRE, 'wire'), (TopAbs_EDGE, 'edge')]:
                for index, item in enumerate(cad.shapes(face, kind)):
                    result = analyzer.Result(item)
                    statuses = list(result.Status())
                    result.InitContextIterator()
                    while result.MoreShapeInContext():
                        if result.ContextualShape().IsSame(face):
                            statuses.extend(result.StatusOnShape())
                        result.NextShapeInContext()
                    bad = sorted(set(str(s) for s in statuses if s != BRepCheck_NoError))
                    if bad:
                        issues.append(dict(kind=name, index=index, status=bad))
            failures.append(dict(face=fi, issues=issues))
        dump(out/'brep_errors.json', failures)
    dump(out/'assembly.json', dict(topology=before, curves=checks,
                                  max_curve_on_surface_mm=boundary_max, boundary_tolerance_mm=tolerance,
                                  assembly_seconds=construction_seconds, topology_seconds=check_seconds))
    write_seconds = reread_seconds = 0.
    after = before
    if write_preview:
        tick = time.perf_counter(); cad.write_step(out/'model.step', solid); write_seconds = time.perf_counter()-tick
        tick = time.perf_counter(); reread = cad.read_generated(out/'model.step'); after = topology(reread)
        reread_seconds = time.perf_counter()-tick
    result = dict(topology_before=before, topology=after, max_curve_on_surface_mm=boundary_max,
                  construction_seconds=construction_seconds, topology_seconds=check_seconds,
                  step_write_seconds=write_seconds, step_reread_check_seconds=reread_seconds,
                  step_bytes=(out/'model.step').stat().st_size if write_preview else 0,
                  step_roundtrip_performed=write_preview,
                  native_file='native.binbrep' if binary_native else 'native.brep',
                  continuous_error_reused_after_geometry_identity_check=certified_errors is not None,
                  actual_support_control_points=sum(len(r['height']) for r in records),
                  fitted_charts=len(records), trim_curves=len(edges), expected_solids=expected_solids,
                  cad_face_charts=cad_face_charts,
                  watertight=bool(after['brep_valid'] and after['solids'] == expected_solids and after['free_edges'] == 0
                                  and after['multiple_edges'] == 0 and after['maximum_edge_tolerance_mm'] <= tolerance))
    dump(out/'cad_result.json', result)
    if native_context is not None:
        native_context.update(shape=solid, topology=before, assembly=str(out.resolve()))
    return result
