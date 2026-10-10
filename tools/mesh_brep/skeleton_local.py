"""Export actual trimmed regional STEP faces from a partial skeleton fit."""
import argparse
import json
from pathlib import Path
import numpy as np
from OCP.BRep import BRep_Builder, BRep_Tool
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeEdge
from OCP.TopoDS import TopoDS, TopoDS_Wire, TopoDS_Face, TopoDS_Compound
from OCP.TopLoc import TopLoc_Location
from . import cad
from .compact import curve
from .partition_fit import surface_poles, boundary_edges, loops, parameters, spline_graph_error
from .partition_cad import projected_curve, topology
from .mesh import dump


def export(root, output, regions=None, smooth_segments=False):
    root, out = Path(root), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8')); data = np.load(meta['partition'])
    v, f = data['mesh_vertex_original'], data['mesh_triangle']
    records = {int(p.stem): dict(np.load(p)) for p in sorted((root/'charts').glob('*.npz'))}
    if regions is not None:
        records = {i: r for i, r in records.items() if int(r['region']) in regions}
    requested = set(regions if regions is not None else meta.get('partial_regions') or data['face_instance_id'])
    for region in requested:
        expected = np.flatnonzero(data['face_instance_id'] == region)
        selected = [r['triangle_ids'] for r in records.values() if int(r['region']) == region]
        actual = np.sort(np.concatenate(selected)) if selected else np.array([], dtype=int)
        if not np.array_equal(actual, expected):
            raise ValueError(f'Region {region} has missing or duplicated source coverage; an intermediate failed chart must not be omitted')
    surfaces = {i: cad.surface(surface_poles(r), r['knots'], r['knots']) for i, r in records.items()}
    builder = BRep_Builder(); vertices, edges, lookup = {}, {}, {}
    for path in sorted((root/'curves').glob('*.npz')):
        ci = int(path.stem); rec = dict(np.load(path)); owners = [int(i) for i in rec['owners'] if int(i) in records]
        if not owners:
            continue
        ch = rec['chain']; c = curve(rec['poles'], rec['knots']); t = rec['parameters']
        errors = {fi: spline_graph_error(rec['poles'], rec['knots'], records[fi]) for fi in owners}
        pcurves = {fi: projected_curve(c, records[fi]) for fi in owners}
        tolerance = max(1e-7, max(errors.values())*1.05)
        parts = [(j, j+1) for j in range(len(ch)-1)] if smooth_segments else [(0, len(ch)-1)]
        for first, last in parts:
            a, b = map(int, ch[[first, last]])
            for vi, value in [(a, t[first]), (b, t[last])]:
                p = c.Value(float(value))
                if vi not in vertices:
                    vertices[vi] = BRepBuilderAPI_MakeVertex(p).Vertex()
                elif BRep_Tool.Pnt_s(vertices[vi]).Distance(p) > 1e-8:
                    raise ValueError(f'Inconsistent canonical boundary junction at source vertex {vi}')
                builder.UpdateVertex(vertices[vi], max(BRep_Tool.Tolerance_s(vertices[vi]), tolerance))
            cc = c.Copy(); cc.Segment(float(t[first]), float(t[last]))
            edge = BRepBuilderAPI_MakeEdge(cc, vertices[a], vertices[b], float(t[first]), float(t[last])).Edge()
            for fi in owners:
                pc = pcurves[fi].Copy(); pc.Segment(float(t[first]), float(t[last]))
                builder.UpdateEdge(edge, pc, surfaces[fi], TopLoc_Location(), max(1e-7, errors[fi]*1.05))
            builder.SameRange(edge, True); builder.SameParameter(edge, True)
            edge_id = len(edges); edges[edge_id] = edge
            for av, bv in zip(ch[first:last], ch[first+1:last+1]):
                lookup[tuple(sorted((int(av), int(bv))))] = edge_id, int(av)
    regional = {}
    for fi, rec in records.items():
        used = rec['vertex_ids']; tri = np.searchsorted(used, f[rec['triangle_ids']]); wires = []
        for lp in loops(boundary_edges(tri)):
            loop = used[lp]; refs = []
            for a, b in zip(loop, np.roll(loop, -1)):
                ci, av = lookup[tuple(sorted((int(a), int(b))))]; item = ci, int(a) == av
                if not refs or refs[-1] != item:
                    refs.append(item)
            if len(refs) > 1 and refs[0] == refs[-1]:
                refs.pop()
            wire = TopoDS_Wire(); builder.MakeWire(wire)
            for ci, forward in refs:
                builder.Add(wire, edges[ci] if forward else TopoDS.Edge_s(edges[ci].Reversed()))
            wire.Closed(True); q = parameters(v[loop], rec)
            area = np.sum(q[:, 0]*np.roll(q[:, 1], -1)-q[:, 1]*np.roll(q[:, 0], -1)); wires.append((area, wire))
        face = TopoDS_Face(); builder.MakeFace(face, surfaces[fi], TopLoc_Location(), 1e-7)
        for _, wire in sorted(wires, key=lambda x: -x[0]):
            builder.Add(face, wire)
        region = int(rec['region'])
        if region not in regional:
            shape = TopoDS_Compound(); builder.MakeCompound(shape); regional[region] = shape
        builder.Add(regional[region], face)
    report = []
    for region, shape in regional.items():
        path = out/f'region_{region:04d}.step'; cad.write_step(path, shape)
        result = topology(cad.read_generated(path)); report.append(dict(region=region, **result))
    dump(out/'run.json', dict(source_fit=str(root.resolve()), scope='Open regional STEP diagnostics', regions=report,
                              complete_requested_region_source_coverage=True, smooth_segments=smooth_segments))
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', type=int, nargs='+')
    p.add_argument('--smooth-segments', action='store_true')
    a = p.parse_args(); print(export(a.run, a.output, a.regions, a.smooth_segments))
