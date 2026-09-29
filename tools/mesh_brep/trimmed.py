"""Mesh-only C2 supports with floating outer trims and shared internal curves.

The supplied partition is retained. A small support strip outside open boundaries
lets their geometric complexity live in the trimming curve, not tensor knots.
All fitting positions, normals and UV corrections come from the indexed mesh.
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree
from .mesh import read_stl, charts, sample, side_uv, distortion, stats, dump, mean_value, vertex_normals
from .splines import knots, basis, tensor, evaluate, shared_maps, joint_fit
from . import cad


def quadratic_normals(vertices, triangles, neighbors=24):
    """Local quadratic mesh jets reduce the bias of one-sided boundary normals."""
    from .mesh import vertex_normals
    first = vertex_normals(vertices, triangles)
    distance, ids = cKDTree(vertices).query(vertices, k=min(neighbors, len(vertices)))
    normal = np.empty_like(first)
    for i, (n, adjacent, d) in enumerate(zip(first, ids, distance)):
        axis = np.eye(3)[np.argmin(abs(n))]
        x = np.cross(n, axis); x /= np.linalg.norm(x); y = np.cross(n, x)
        delta = vertices[adjacent]-vertices[i]
        u, v, h = delta@x, delta@y, delta@n
        scale = max(d[-1], 1e-10); u /= scale; v /= scale
        a = np.c_[u*0+1, u, v, u*u, u*v, v*v]
        weight = np.exp(-2*(d/scale)**2)
        c = np.linalg.lstsq(a*weight[:, None], h*weight, rcond=1e-10)[0]
        q = n-x*c[1]/scale-y*c[2]/scale
        normal[i] = q/np.linalg.norm(q)
    return normal


def project(poles, tu, tv, points, uv, iterations=6):
    """Safeguarded closest-point updates on a newly fitted support only."""
    uv = uv.copy(); flat = poles.reshape(-1, 3)
    for _ in range(iterations):
        initial_uv = uv.copy()
        p = tensor(uv, tu, tv)@flat
        du = tensor(uv, tu, tv, 1, 0)@flat; dv = tensor(uv, tu, tv, 0, 1)@flat
        d = p-points
        a = np.sum(du*du, axis=1); b = np.sum(du*dv, axis=1); c = np.sum(dv*dv, axis=1)
        r = np.sum(d*du, axis=1); s = np.sum(d*dv, axis=1)
        det = np.maximum(a*c-b*b, 1e-25)
        step = np.c_[(c*r-b*s)/det, (a*s-b*r)/det]
        step *= np.minimum(1., .04/np.maximum(np.linalg.norm(step, axis=1), 1e-20))[:, None]
        old = np.sum(d*d, axis=1)
        for _ in range(6):
            candidate = np.clip(uv-step, 0., 1.)
            error = np.sum((tensor(candidate, tu, tv)@flat-points)**2, axis=1)
            accepted = error <= old+1e-24
            uv[accepted] = candidate[accepted]
            step[accepted] = 0.; step[~accepted] *= .5
        if np.max(np.linalg.norm(uv-initial_uv, axis=1)) < 1e-12:
            break
    return uv


def pcurve(points, parameters):
    from OCP.TColgp import TColgp_HArray1OfPnt2d
    from OCP.TColStd import TColStd_HArray1OfReal
    from OCP.Geom2dAPI import Geom2dAPI_Interpolate
    from OCP.gp import gp_Pnt2d
    ps = TColgp_HArray1OfPnt2d(1, len(points)); ts = TColStd_HArray1OfReal(1, len(points))
    for i, (q, t) in enumerate(zip(points, parameters)):
        ps.SetValue(i+1, gp_Pnt2d(*q)); ts.SetValue(i+1, float(t))
    fit = Geom2dAPI_Interpolate(ps, ts, False, 1e-12); fit.Perform()
    if not fit.IsDone():
        raise RuntimeError('Trim interpolation failed')
    return fit.Curve()


def export(poles, knot_vectors, atlas, uses, margin, output):
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeVertex, BRepBuilderAPI_MakeWire
    from OCP.BRepLib import BRepLib
    from OCP.TopoDS import TopoDS_Shell, TopoDS
    from OCP.TopLoc import TopLoc_Location
    from OCP.gp import gp_Pnt
    surfaces = [cad.surface(p, tu, tv) for p, (tu, tv) in zip(poles, knot_vectors)]
    builder = BRep_Builder(); shell = TopoDS_Shell(); builder.MakeShell(shell)
    vertices = {}; edges = {}
    for key, entries in uses.items():
        e = entries[0]; fi, side = e['face'], e['side']; chart = atlas[fi]
        chain = chart['chains'][side]
        uv = chart['uv'][chain]
        params = chart['chain_t'][side]
        for vid, q in [(chain[0], uv[0]), (chain[-1], uv[-1])]:
            if vid not in vertices:
                point = surfaces[fi].Value(*q)
                vertices[vid] = BRepBuilderAPI_MakeVertex(point).Vertex()
        if len(entries) == 1:
            pc = pcurve(uv, params)
            edge = BRepBuilderAPI_MakeEdge(pc, surfaces[fi], vertices[chain[0]], vertices[chain[-1]], 0., 1.).Edge()
            if not BRepLib.BuildCurve3d_s(edge, 1e-8):
                raise RuntimeError('Cannot approximate the new trim in 3D')
        else:
            # Exact shared support isocurve. Parameter speed is common on both sides.
            q0, q1 = uv[0], uv[-1]
            axis = int(abs(q1[0]-q0[0]) > abs(q1[1]-q0[1]))
            curve = surfaces[fi].UIso(float(q0[0])) if axis == 0 else surfaces[fi].VIso(float(q0[1]))
            varying = 1-axis; a, b = float(q0[varying]), float(q1[varying])
            va, vb = vertices[chain[0]], vertices[chain[-1]]
            if a > b:
                a, b, va, vb = b, a, vb, va
            edge = BRepBuilderAPI_MakeEdge(curve, va, vb, a, b).Edge()
            # Edge orientation follows the first chart's chain.
            if q1[varying] < q0[varying]:
                edge = TopoDS.Edge_s(edge.Reversed())
            for entry in entries:
                f, s = entry['face'], entry['side']; ch = atlas[f]['chains'][s]
                q = atlas[f]['uv'][[ch[0], ch[-1]]]
                if ch[0] != chain[0]:
                    q = q[::-1]
                if q1[varying] < q0[varying]:
                    q = q[::-1]
                pc = pcurve(q, [a, b])
                builder.UpdateEdge(edge, pc, surfaces[f], TopLoc_Location(), 1e-7)
        edges[key] = (edge, chain[0])
    for fi, (surface, chart) in enumerate(zip(surfaces, atlas)):
        wire = BRepBuilderAPI_MakeWire()
        for chain in chart['chains']:
            edge, start = edges[tuple(sorted([chain[0], chain[-1]]))]
            wire.Add(edge if chain[0] == start else TopoDS.Edge_s(edge.Reversed()))
        if not wire.IsDone():
            raise RuntimeError('Trimmed wire is disconnected')
        face = BRepBuilderAPI_MakeFace(surface, wire.Wire(), True).Face()
        if chart['distortion']['orientation'] < 0:
            face = TopoDS.Face_s(face.Reversed())
        builder.Add(shell, face)
    BRepLib.SameParameter_s(shell, 1e-7, True)
    cad.write_step(output, shell)
    return shell


def reconstruct(stl, layout, out, controls=25, samples=36000, seed=2910100, iterations=8, margin=.08, graded=False):
    start = time.perf_counter(); out = Path(out); out.mkdir(parents=True, exist_ok=False)
    vertices, triangles = read_stl(stl); metadata = json.loads(Path(layout).read_text(encoding='utf-8'))
    allowed = {'schema', 'units', 'triangle_labels', 'patches', 'smooth_seams', 'input_kind',
               'tolerance_mm', 'seam_tolerance_mm', 'construction_tolerance_mm'}
    if set(metadata)-allowed or metadata['units'] != 'mm' or not metadata['smooth_seams']:
        raise ValueError('Only declared smooth mesh partitions in mm are accepted')
    # This experiment keeps the existing central quad and its four neighbors.
    if len(metadata['patches']) != 5:
        raise ValueError('This prototype requires the declared five-chart disk layout')
    read_seconds = time.perf_counter()-start
    tick = time.perf_counter(); atlas = charts(vertices, triangles, metadata)
    _, _, uses, _ = shared_maps(vertices, atlas, controls)
    outer = {e[0]['face']: e[0]['side'] for e in uses.values() if len(e) == 1}
    if set(outer) != set(range(4)) or any(s != 0 for s in outer.values()):
        raise ValueError('Expected four outer chains on side zero and a central patch')
    outer_corners = {v for c in atlas[:4] for v in [c['chains'][0][0], c['chains'][0][-1]]}
    inner_corners = set(metadata['patches'][4]['corners'])
    for c in atlas[:4]:
        c['uv'][:, 1] = margin+(1-margin)*c['uv'][:, 1]
    uv_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); normals = vertex_normals(vertices, triangles)
    normal_seconds = time.perf_counter()-tick
    maps, offsets, uses, _ = shared_maps(vertices, atlas, controls, fixed_vertex_ids=inner_corners)
    t = knots(controls)
    radial = np.r_[np.zeros(4), margin/2, margin,
                   margin+(1-margin)*np.linspace(0, 1, controls-4)[1:-1]**1.5, np.ones(4)] if graded else t
    knot_vectors = [(t, radial)]*4+[(t, t)]
    areas = np.array([c['audit']['area_mm2'] for c in atlas])
    counts = np.maximum(1000, (samples*areas/areas.sum()).astype(int)); histories = []
    correction_seconds = 0.; assembly_seconds = 0.; solve_seconds = 0.
    def mapped_side(fi, side, q):
        uv = side_uv(side, q)
        if fi < 4:
            uv[:, 1] = margin+(1-margin)*uv[:, 1]
        return uv
    for iteration in range(iterations):
        specs = []; probes = []; hard_tangents = []; hard_positions = []
        for fi, c in enumerate(atlas):
            tu, tv = knot_vectors[fi]
            points, nn, uv, _, _ = sample(vertices, c['triangles'], int(counts[fi]), seed+fi, c['uv'], normals)
            lengths = [sum(np.linalg.norm(np.diff(vertices[ch], axis=0), axis=1)) for ch in c['chains']]
            spec = dict(uv=uv, points=points, normals=nn, tu=tu, tv=tv,
                        lengths=[(lengths[0]+lengths[2])/2, (lengths[1]+lengths[3])/2])
            if fi < 4:
                ids = c['chains'][0]
                # First establish the smooth support. A stiff boundary penalty
                # at the initial square UV would imprint the trim into it.
                spec.update(boundary_points=vertices[ids], boundary_uv=c['uv'][ids],
                            boundary_weight=.01 if iteration < 2 else 100.)
                hard_positions.append((fi, c['uv'][[ids[0]]], vertices[[ids[0]]]))
            specs.append(spec)
            for chain in c['chains']:
                vid = chain[0]; uv0 = c['uv'][[vid]]
                hard_tangents.extend([(fi, uv0, normals[[vid]], [1., 0.]), (fi, uv0, normals[[vid]], [0., 1.])])
        for entries in uses.values():
            if len(entries) != 2:
                continue
            q = np.linspace(0., 1., 193); e = entries[0]; c = atlas[e['face']]
            ct = c['chain_t'][e['side']]; ch = c['chains'][e['side']]
            nn = np.stack([np.interp(q, ct, normals[ch, k]) for k in range(3)], axis=1)
            nn /= np.linalg.norm(nn, axis=1)[:, None]
            for entry in entries:
                fi, side = entry['face'], entry['side']; chain = atlas[fi]['chains'][side]
                params = q if chain[0] == ch[0] else 1-q
                probes.append((fi, mapped_side(fi, side, params), nn))
        poles, solver = joint_fit(specs, maps, offsets, probes, normal_weight=.01, fairness=1e-5,
                                   seam_weight=1., quadrature_order=4,
                                   hard_tangents=hard_tangents, hard_positions=hard_positions)
        assembly_seconds += solver['matrix_assembly_seconds']; solve_seconds += solver['solve_seconds']
        record = dict(iteration=iteration, solver=solver, boundary_mm=[])
        tick = time.perf_counter()
        for fi, c in enumerate(atlas):
            tu, tv = knot_vectors[fi]
            # Move the outer trim, then solve the connected mesh chart again.
            # Independent vertex projections fold sliver triangles and stall a
            # global line search; their displacement is not a parameterization.
            free = np.asarray(c['chains'][0][1:-1], dtype=int) if fi < 4 else np.array([], dtype=int)
            if iteration+1 < iterations and len(free):
                before = c['uv'].copy()
                c['uv'][free] = project(poles[fi], tu, tv, vertices[free], c['uv'][free])
                boundary_ids = np.concatenate([ch[:-1] for ch in c['chains']])
                c['uv'], _ = mean_value(vertices, c['triangles'], boundary_ids, c['uv'][boundary_ids])
                quality = distortion(vertices, c['triangles'], c['uv'])
                # Backtrack the full chart update to preserve mesh orientation.
                for backtrack in range(24):
                    if not quality['flipped'] and not quality['zero']:
                        break
                    c['uv'] = (c['uv']+before)/2
                    quality = distortion(vertices, c['triangles'], c['uv'])
                else:
                    c['uv'] = before
                    record.setdefault('rejected_chart_updates', []).append(fi)
                record.setdefault('chart_step_scale', []).append(2.**(-backtrack))
            if fi < 4:
                ids = c['chains'][0]; values = tensor(c['uv'][ids], tu, tv)@poles[fi].reshape(-1, 3)
                record['boundary_mm'].append(stats(np.linalg.norm(values-vertices[ids], axis=1)))
        correction_seconds += time.perf_counter()-tick; histories.append(record)
        dump(out/'iterations.json', histories)
        print(json.dumps(dict(iteration=iteration, boundary_max_mm=[r['max'] for r in record['boundary_mm']])), flush=True)
    tick = time.perf_counter(); export(poles, knot_vectors, atlas, uses, margin, out/'network.step')
    construct_write_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); shape = cad.read_generated(out/'network.step')
    topo = cad.topology(shape); checks = cad.edge_checks(shape); expression = cad.expression(shape)
    check_seconds = time.perf_counter()-tick
    for fi, (p, c) in enumerate(zip(poles, atlas)):
        tu, tv = knot_vectors[fi]
        np.savez_compressed(out/f'patch_{fi}.npz', poles=p, tu=tu, tv=tv, vertex_uv=c['uv'], triangle_ids=c['triangle_ids'])
    report = dict(status='DiagnosticOnly', can_apply=False, mode='given-partition mesh-only floating-trim C2 reconstruction',
                  inputs=[str(Path(stl).resolve()), str(Path(layout).resolve())], controls=controls, samples=int(counts.sum()),
                  seed=seed, margin=margin, graded=graded, iterations=histories, topology=topo, cad_checks=checks, expression=expression,
                  normal_source='Area-weighted oriented triangle one-ring; no spatial-neighbor welding or jet extrapolation',
                  uv=[distortion(vertices, c['triangles'], c['uv']) for c in atlas],
                  timing_seconds=dict(read_index=read_seconds, uv=uv_seconds, mesh_normals=normal_seconds,
                                      correspondence_correction=correction_seconds, matrix_assembly=assembly_seconds,
                                      solve=solve_seconds, brep_write=construct_write_seconds, reread_checks=check_seconds),
                  wall_seconds=time.perf_counter()-start)
    dump(out/'reconstruction.json', report)
    return report


def main():
    p = argparse.ArgumentParser()
    for name in ['stl', 'layout', 'output']:
        p.add_argument('--'+name, required=True)
    p.add_argument('--controls', type=int, default=25); p.add_argument('--iterations', type=int, default=8)
    p.add_argument('--graded', action='store_true')
    a = p.parse_args(); r = reconstruct(a.stl, a.layout, a.output, a.controls, iterations=a.iterations, graded=a.graded)
    print(json.dumps({k: r[k] for k in ['topology', 'expression', 'wall_seconds']}), flush=True)


if __name__ == '__main__':
    main()
