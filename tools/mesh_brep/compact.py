"""Mesh-only five-patch reconstruction with compact C2 boundary curves.

Adaptive one-dimensional boundary knots are independent of the smaller shared
seam bases. Exact knot insertion couples adjacent XYZ surfaces without forcing
the central patch to inherit the outer boundary's complexity.
"""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from scipy import sparse
from scipy.interpolate import splprep
from .mesh import read_stl, charts, mean_value, distortion, sample, stats, dump
from .splines import knots, basis, tensor, joint_fit
from . import cad


def array1(values, integer=False):
    from OCP.TColStd import TColStd_Array1OfInteger, TColStd_Array1OfReal
    a = (TColStd_Array1OfInteger if integer else TColStd_Array1OfReal)(1, len(values))
    for i, value in enumerate(values):
        a.SetValue(i+1, int(value) if integer else float(value))
    return a


def curve(poles, t):
    from OCP.Geom import Geom_BSplineCurve
    from OCP.TColgp import TColgp_Array1OfPnt
    from OCP.gp import gp_Pnt
    p = TColgp_Array1OfPnt(1, len(poles))
    for i, value in enumerate(poles):
        p.SetValue(i+1, gp_Pnt(*value))
    k, m = np.unique(t, return_counts=True)
    return Geom_BSplineCurve(p, array1(k), array1(m, True), 3)


def unpack(surface):
    p = np.array([[[surface.Pole(i, j).X(), surface.Pole(i, j).Y(), surface.Pole(i, j).Z()]
                   for j in range(1, surface.NbVPoles()+1)] for i in range(1, surface.NbUPoles()+1)])
    tu = np.repeat([surface.UKnot(i) for i in range(1, surface.NbUKnots()+1)],
                   [surface.UMultiplicity(i) for i in range(1, surface.NbUKnots()+1)])
    tv = np.repeat([surface.VKnot(i) for i in range(1, surface.NbVKnots()+1)],
                   [surface.VMultiplicity(i) for i in range(1, surface.NbVKnots()+1)])
    return p, tu, tv


def side_uv(mapping, t):
    t = np.asarray(t); uv = np.empty((len(t), 2)); uv[:, mapping['axis']] = mapping['fixed']
    uv[:, 1-mapping['axis']] = 1-t if mapping['reverse'] else t
    return uv


def side_mapping(surface, boundary):
    q = np.linspace(0, 1, 9)
    target = np.array([boundary.Value(float(t)).Coord() for t in q]); options = []
    for axis in range(2):
        for fixed in [0., 1.]:
            for reverse in [False, True]:
                mapping = dict(axis=axis, fixed=fixed, reverse=reverse)
                value = np.array([surface.Value(*uv).Coord() for uv in side_uv(mapping, q)])
                options.append((np.linalg.norm(value-target, axis=1).max(), mapping))
    error, mapping = min(options, key=lambda x: x[0])
    if error > 1e-7:
        raise ValueError(f'Incompatible support boundary: {error}')
    return mapping


def constrained_curve(points, parameters, t):
    a = basis(t, parameters); cp = np.zeros((len(t)-4, 3)); cp[[0, -1]] = points[[0, -1]]
    cp[1:-1] = np.linalg.lstsq(a[:, 1:-1], points-a@cp, rcond=None)[0]
    return cp


def build(vertices, atlas, radial_controls=13, inner_controls=13, boundary_rms=.0001):
    from OCP.GeomFill import GeomFill_BSplineCurves, GeomFill_CoonsStyle
    from OCP.gp import gp_Pnt
    outer, reports = [], []
    for fi, c in enumerate(atlas[:4]):
        ch, params = c['chains'][0], c['chain_t'][0]
        (t, cp, degree), _ = splprep(vertices[ch].T, u=params, s=len(ch)*boundary_rms**2, k=3)
        cp = np.array(cp).T; cp[[0, -1]] = vertices[np.asarray(ch)[[0, -1]]]
        error = np.linalg.norm(basis(t, params)@cp-vertices[ch], axis=1)
        outer.append(curve(cp, t))
        reports.append(dict(face=fi, controls=len(cp), sample_error_mm=stats(error)))
    radial = knots(radial_controls)
    # Resolve the narrow curved strip near the outer boundary with a graded
    # radial basis, while preserving simple knots and C2 continuity.
    radial[4:-4] = np.linspace(0, 1, radial_controls-2)[1:-1]**2
    inner = knots(inner_controls)
    seams = []; seam_knots = []
    for fi, c in enumerate(atlas[:4]):
        ch = c['chains'][3][::-1]; params = 1-c['chain_t'][3][::-1]
        seams.append(curve(constrained_curve(vertices[ch], params, radial), radial)); seam_knots.append(radial)
    for fi, c in enumerate(atlas[:4]):
        ch = c['chains'][2][::-1]; params = 1-c['chain_t'][2][::-1]
        seams.append(curve(constrained_curve(vertices[ch], params, inner), inner)); seam_knots.append(inner)
    surfaces, sides = [], []
    for fi in range(4):
        cs = [outer[fi].Copy(), seams[(fi+1) % 4].Copy(), seams[fi+4].Copy(), seams[fi].Copy()]
        cs[2].Reverse(); cs[3].Reverse()
        surface = GeomFill_BSplineCurves(*cs, GeomFill_CoonsStyle).Surface()
        surfaces.append(surface)
        sides.append([dict(outer=True, mapping=side_mapping(surface, outer[fi]))]+
                     [dict(outer=False, seam=sid, mapping=side_mapping(surface, seams[sid]))
                      for sid in [(fi+1) % 4, fi+4, fi]])
    center = GeomFill_BSplineCurves(*[c.Copy() for c in seams[4:]], GeomFill_CoonsStyle).Surface()
    surfaces.append(center); sides.append([dict(outer=False, seam=sid, mapping=side_mapping(center, seams[sid])) for sid in range(4, 8)])
    return dict(surfaces=surfaces, sides=sides, outer=outer, seams=seams, seam_knots=seam_knots,
                seam_ends=[(None, i) for i in range(4)]+[(i, (i+1) % 4) for i in range(4)], boundary_fits=reports)


def variable_maps(layout):
    sizes = [len(t)-4 for t in layout['seam_knots']]
    masters = 4+sum(n-2 for n in sizes); seam_maps = []; cursor = 4
    for n, (a, b) in zip(sizes, layout['seam_ends']):
        m = np.zeros((n, masters))
        if a is not None:
            m[0, a] = 1
        m[-1, b] = 1
        for j in range(1, n-1):
            m[j, cursor] = 1; cursor += 1
        seam_maps.append(m)
    total = masters+sum((s.NbUPoles()-2)*(s.NbVPoles()-2) for s in layout['surfaces'])
    matrices = []; cursor = masters
    for surface, sides in zip(layout['surfaces'], layout['sides']):
        p, tu, tv = unpack(surface); nu, nv = p.shape[:2]; boundary = {}
        for side in sides:
            m = side['mapping']; t = tv if m['axis'] == 0 else tu; n = len(t)-4
            g = np.array([t[i+1:i+4].mean() for i in range(n)])
            embedded = np.zeros((n, masters)) if side['outer'] else np.linalg.solve(
                basis(t, g), basis(layout['seam_knots'][side['seam']], 1-g if m['reverse'] else g))@seam_maps[side['seam']]
            for k, row in enumerate(embedded):
                i = (0 if m['fixed'] == 0 else nu-1) if m['axis'] == 0 else k
                j = k if m['axis'] == 0 else (0 if m['fixed'] == 0 else nv-1)
                key = i*nv+j; row[abs(row) < 1e-12] = 0
                if key in boundary and np.max(abs(boundary[key]-row)) > 1e-9:
                    raise ValueError('Inconsistent shared corner variables')
                boundary[key] = row
        rows, cols, values = [], [], []
        for key in range(nu*nv):
            if key in boundary:
                row = boundary[key]; nz = np.flatnonzero(row)
                rows.extend([key]*len(nz)); cols.extend(nz); values.extend(row[nz])
            else:
                rows.append(key); cols.append(cursor); values.append(1.); cursor += 1
        matrices.append(sparse.csr_matrix((values, (rows, cols)), shape=(nu*nv, total)))
    return matrices


def reconstruct(stl, layout_path, out, radial_controls=13, inner_controls=13, samples=36000, seed=2910100):
    start = time.perf_counter(); out = Path(out); out.mkdir(parents=True, exist_ok=False)
    vertices, triangles = read_stl(stl); meta = json.loads(Path(layout_path).read_text(encoding='utf-8'))
    allowed = {'schema', 'units', 'triangle_labels', 'patches', 'smooth_seams', 'input_kind',
               'tolerance_mm', 'seam_tolerance_mm', 'construction_tolerance_mm'}
    if set(meta)-allowed or meta['units'] != 'mm' or not meta['smooth_seams']:
        raise ValueError('Only declared smooth mesh partitions in mm are accepted')
    for patch in meta['patches']:
        if set(patch) != {'corners', 'chains'}:
            raise ValueError('Only indexed corners and chains may be provided')
    if len(meta['patches']) != 5:
        raise ValueError('This prototype retains the supplied five-chart topology')
    read_seconds = time.perf_counter()-start
    tick = time.perf_counter(); atlas = charts(vertices, triangles, meta)
    uv_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); layout = build(vertices, atlas, radial_controls, inner_controls,
                                               boundary_rms=meta['seam_tolerance_mm']*.1)
    maps = variable_maps(layout); offsets = [unpack(s)[0].reshape(-1, 3) for s in layout['surfaces']]
    boundary_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); specs, probes, hard = [], [], []
    areas = np.array([c['audit']['area_mm2'] for c in atlas])
    for fi, c in enumerate(atlas):
        fixed_ids = np.concatenate([ch[:-1] for ch in c['chains']]); fixed = []
        for side, (ch, params) in enumerate(zip(c['chains'], c['chain_t'])):
            rec = layout['sides'][fi][side]
            reverse = fi < 4 and side in [2, 3]
            fixed.append(side_uv(rec['mapping'], (1-params if reverse else params)[:-1]))
            q = np.linspace(0, 1, 193)
            nn = np.stack([np.interp(q, params, c['normals'][ch, k]) for k in range(3)], axis=1)
            nn /= np.linalg.norm(nn, axis=1)[:, None]
            probes.append((fi, side_uv(rec['mapping'], 1-q if reverse else q), nn))
        c['uv'], residual = mean_value(vertices, c['triangles'], fixed_ids, np.concatenate(fixed))
        quality = distortion(vertices, c['triangles'], c['uv'])
        if quality['flipped'] or quality['zero']:
            raise ValueError('Spline-compatible mesh chart is invalid')
        c['distortion'] = quality
        count = max(1000, int(samples*areas[fi]/areas.sum()))
        points, nn, q, _, _ = sample(vertices, c['triangles'], count, seed+fi, c['uv'], c['normals'])
        p, tu, tv = unpack(layout['surfaces'][fi])
        specs.append(dict(uv=q, points=points, normals=nn, tu=tu, tv=tv,
                          lengths=[np.linalg.norm(p[-1, 0]-p[0, 0]), np.linalg.norm(p[0, -1]-p[0, 0])]))
        # Each inner junction has one common plane estimated from its mesh ring.
        for vid in meta['patches'][fi]['corners']:
            if vid in meta['patches'][4]['corners']:
                for direction in [[1., 0.], [0., 1.]]:
                    hard.append((fi, c['uv'][[vid]], c['normals'][[vid]], direction))
    for fi in range(4):
        a = np.array(layout['outer'][fi].DN(0., 1).Coord()); b = np.array(layout['outer'][(fi-1) % 4].DN(1., 1).Coord())
        n = np.cross(a/np.linalg.norm(a), b/np.linalg.norm(b))
        if np.linalg.norm(n) < 1e-6:
            raise ValueError('Boundary corner tangents do not determine a stable common plane')
        n /= np.linalg.norm(n)
        rec = layout['sides'][fi][3]; q = side_uv(rec['mapping'], np.array([0., 1.]))
        hard.append((fi, q[:1], n[None], q[1]-q[0]))
    correspondence_seconds = time.perf_counter()-tick
    poles, solver = joint_fit(specs, maps, offsets, probes, normal_weight=.01, fairness=1e-5,
                              seam_weight=1., quadrature_order=4, hard_tangents=hard)
    knot_vectors = [(s['tu'], s['tv']) for s in specs]
    tick = time.perf_counter()
    shape = cad.construct(poles, knot_vectors, [c['distortion']['orientation'] for c in atlas], False,
                          meta['construction_tolerance_mm'])
    cad.write_step(out/'network.step', shape); write_seconds = time.perf_counter()-tick
    tick = time.perf_counter(); reread = cad.read_generated(out/'network.step')
    topology = cad.topology(reread); checks = cad.edge_checks(reread); expression = cad.expression(reread)
    check_seconds = time.perf_counter()-tick
    for fi, (p, s, c) in enumerate(zip(poles, specs, atlas)):
        np.savez_compressed(out/f'patch_{fi}.npz', poles=p, tu=s['tu'], tv=s['tv'], vertex_uv=c['uv'], triangle_ids=c['triangle_ids'])
    report = dict(status='DiagnosticOnly', can_apply=False, mode='given-partition pure mesh C2 network with independent boundary and seam bases',
                  inputs=[str(Path(stl).resolve()), str(Path(layout_path).resolve())], radial_controls=radial_controls,
                  inner_controls=inner_controls, samples=samples, seed=seed, boundary_fits=layout['boundary_fits'],
                  solver=solver, topology=topology, cad_checks=checks, expression=expression, uv=[c['distortion'] for c in atlas],
                  sides=layout['sides'],
                  timing_seconds=dict(read_index=read_seconds, mesh_uv=uv_seconds, boundary_and_shared_maps=boundary_seconds,
                                      correspondence=correspondence_seconds, matrix_assembly=solver['matrix_assembly_seconds'],
                                      solve=solver['solve_seconds'], brep_write=write_seconds, reread_checks=check_seconds),
                  wall_seconds=time.perf_counter()-start)
    dump(out/'reconstruction.json', report)
    print(json.dumps({k: report[k] for k in ['topology', 'expression', 'wall_seconds']}), flush=True)
    return report


def main():
    p = argparse.ArgumentParser()
    for name in ['stl', 'layout', 'output']:
        p.add_argument('--'+name, required=True)
    p.add_argument('--radial-controls', type=int, default=13); p.add_argument('--inner-controls', type=int, default=13)
    a = p.parse_args(); reconstruct(a.stl, a.layout, a.output, a.radial_controls, a.inner_controls)


if __name__ == '__main__':
    main()
