"""Boundary-first compact regional atlas, using only the supplied STL labels.

The outer network is fitted once. Four annular charts and a central chart
separate a region's boundary complexity from its smooth interior. Topological
junctions on an outer side do not introduce additional supporting surfaces.
"""
import argparse
import json
from pathlib import Path
import time

import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline

from . import cad
from .compact import curve, unpack, side_mapping, side_uv, variable_maps
from .mesh import dump, stats, Locator, mean_value, vertex_normals, chain_parameters
from .partition_fit import load_partition
from .partition_fan import disk_domains, coons_domain
from .partition_cad import shared_chains
from .regional_rectangle import corners_for_loop, compact_curve, concatenate
from .splines import knots, basis, tensor, joint_fit


class CircularChart:
    """A strictly convex boundary avoids collapsing STL boundary triangles."""
    def __init__(self, v, f, domain):
        self.global_ids = domain['vertices']; self.xyz = v[self.global_ids]
        self.tri = np.searchsorted(self.global_ids, f[domain['triangles']])
        loop = domain['loop']; corners = domain['corners']
        fixed = np.empty((len(loop), 2)); self.sides = []
        for side in range(4):
            first, last = int(corners[side]), int(corners[(side+1) % 4])
            if last <= first:
                last += len(loop)
            indices = np.arange(first, last+1) % len(loop)
            t = chain_parameters(v, loop[indices])
            angle = (side+t)*np.pi/2
            fixed[indices] = np.c_[np.cos(angle), np.sin(angle)]
            # The boundary of the actual piecewise-linear UV mesh is used,
            # rather than an analytic circle outside that mesh.
            values = fixed[indices].copy()
            self.sides.append(lambda s, t=t, values=values:
                              np.stack([np.interp(s, t, values[:, k]) for k in range(2)], axis=1))
        self.uv, residual = mean_value(self.xyz, self.tri, np.searchsorted(self.global_ids, loop), fixed)
        q = self.uv[self.tri]; x, y = q[:, 1]-q[:, 0], q[:, 2]-q[:, 0]
        det = x[:, 0]*y[:, 1]-x[:, 1]*y[:, 0]
        if np.any(det <= 0):
            raise ValueError('Circular regional parameterization is not injective')
        self.normals = vertex_normals(self.xyz, self.tri)
        self.lookup = Locator(np.c_[self.uv, np.zeros(len(self.uv))], self.tri)
        self.report = dict(residual=residual, minimum_uv_determinant=float(det.min()))

    def query(self, uv):
        _, distance, ids, bary = self.lookup.closest(np.c_[uv, np.zeros(len(uv))])
        if distance.max() > 1e-7:
            raise ValueError(f'Compact layout leaves its discrete UV domain: {distance.max()}')
        tri = self.tri[ids]
        xyz = np.einsum('ij,ijk->ik', bary, self.xyz[tri])
        normal = np.einsum('ij,ijk->ik', bary, self.normals[tri])
        normal /= np.maximum(np.linalg.norm(normal, axis=1)[:, None], 1e-30)
        return xyz, normal


def prepare(v, f, labels, smoothing):
    domains, assigned, edges, owners = disk_domains(v, f, labels)
    corner_vertices = set()
    for d in domains:
        if len(d['loop']) >= 4:
            d['corners'] = corners_for_loop(v, d['loop'])
            corner_vertices.update(d['loop'][d['corners']])
    source, pairs = shared_chains(edges, owners, assigned)
    chains, adjacent, curves, lookup, errors = [], [], [], {}, []
    for chain, pair in zip(source, pairs):
        cuts = np.r_[0, np.flatnonzero(np.isin(chain[1:-1], list(corner_vertices)))+1, len(chain)-1]
        for first, last in zip(cuts[:-1], cuts[1:]):
            piece = chain[first:last+1]; t = chain_parameters(v, piece)
            value, error = compact_curve(v[piece], t, smoothing)
            ci = len(chains); chains.append(piece); adjacent.append(pair); curves.append(value); errors.append(error)
            for a, b in zip(piece[:-1], piece[1:]):
                lookup[tuple(sorted((int(a), int(b))))] = (ci, int(a))
    return domains, assigned, chains, adjacent, curves, lookup, errors


def outer_sides(v, domain, chains, curves, lookup):
    loop = domain['loop']; sides, references = [], []
    for side in range(4):
        first, last = int(domain['corners'][side]), int(domain['corners'][(side+1) % 4])
        if last <= first:
            last += len(loop)
        segment = loop[np.arange(first, last+1) % len(loop)]; refs = []
        for a, b in zip(segment[:-1], segment[1:]):
            ci, av = lookup[tuple(sorted((int(a), int(b))))]; ref = (ci, int(a) == av)
            if not refs or refs[-1] != ref:
                refs.append(ref)
        oriented, lengths = [], []
        for ci, forward in refs:
            c = curves[ci].Copy()
            if not forward:
                c.Reverse()
            oriented.append(c); lengths.append(float(np.linalg.norm(np.diff(v[chains[ci]], axis=0), axis=1).sum()))
        joined, ranges = concatenate(oriented, lengths)
        sides.append(joined); references.append((refs, ranges))
    return sides, references


def fit_ring(chart, outer, controls=17, inner_scale=.45, normal_weight=.01, fairness=1e-5,
             directed=True):
    from OCP.GeomFill import GeomFill_BSplineCurves, GeomFill_CoonsStyle
    radial = knots(controls); radial[4:-4] = np.linspace(0, 1, controls-2)[1:-1]**2
    inner = knots(controls); seams, seam_uv = [], []
    ends = np.array([fn(np.array([0.]))[0] for fn in chart.sides]); center = ends*inner_scale
    t = np.unique(np.r_[np.linspace(0, 1, 257), np.linspace(0, .1, 65)])
    for i in range(8):
        a, b = (ends[i], center[i]) if i < 4 else (center[i-4], center[(i-3) % 4])
        fn = lambda s, a=a, b=b: (1-s[:, None])*a+s[:, None]*b
        xyz, _ = chart.query(fn(t)); kt = radial if i < 4 else inner
        mat = basis(kt, t); cp = np.zeros((len(kt)-4, 3)); cp[[0, -1]] = xyz[[0, -1]]
        cp[1:-1] = np.linalg.lstsq(mat[:, 1:-1], xyz-mat@cp, rcond=None)[0]
        seams.append(curve(cp, kt)); seam_uv.append(fn)
    surfaces, sides, uv_sides = [], [], []
    for fi in range(4):
        cs = [outer[fi].Copy(), seams[(fi+1) % 4].Copy(), seams[fi+4].Copy(), seams[fi].Copy()]
        cs[2].Reverse(); cs[3].Reverse()
        surface = GeomFill_BSplineCurves(*cs, GeomFill_CoonsStyle).Surface(); surfaces.append(surface)
        sides.append([dict(outer=True, mapping=side_mapping(surface, outer[fi]))]+[
            dict(outer=False, seam=sid, mapping=side_mapping(surface, seams[sid]))
            for sid in [(fi+1) % 4, fi+4, fi]])
        uv_sides.append([chart.sides[fi]]+[seam_uv[sid] for sid in [(fi+1) % 4, fi+4, fi]])
    central = GeomFill_BSplineCurves(*[c.Copy() for c in seams[4:]], GeomFill_CoonsStyle).Surface()
    surfaces.append(central); sides.append([dict(outer=False, seam=sid, mapping=side_mapping(central, seams[sid])) for sid in range(4, 8)])
    uv_sides.append(seam_uv[4:])
    layout = dict(surfaces=surfaces, sides=sides, outer=outer, seams=seams,
                  seam_knots=[radial]*4+[inner]*4,
                  seam_ends=[(None, i) for i in range(4)]+[(i, (i+1) % 4) for i in range(4)])
    maps = variable_maps(layout); specs, offsets, probes, hard = [], [], [], []
    reports = []
    origin = chart.xyz.mean(axis=0)
    for fi, (surface, uv_curves, records) in enumerate(zip(surfaces, uv_sides, sides)):
        cp, tu, tv = unpack(surface); mappings = [s['mapping'] for s in records]
        # Include all knot spans as well as a uniform grid.
        gu = np.unique(np.r_[np.linspace(.0001, .9999, 49), (np.unique(tu)[:-1]+np.unique(tu)[1:])/2])
        gv = np.unique(np.r_[np.linspace(.0001, .9999, 49), (np.unique(tv)[:-1]+np.unique(tv)[1:])/2])
        q = np.stack(np.meshgrid(gu, gv, indexing='ij'), axis=-1).reshape(-1, 2)
        def target_values(q):
            xyz, normals = chart.query(coons_domain(q, uv_curves, mappings))
            if fi < 4:
                m = mappings[0]; t = q[:, 1-m['axis']]
                if m['reverse']:
                    t = 1-t
                original, _ = chart.query(uv_curves[0](t))
                boundary = np.array([outer[fi].Value(float(x)).Coord() for x in t])
                r = abs(q[:, m['axis']]-m['fixed'])
                xyz += (boundary-original)*(1-3*r*r+2*r*r*r)[:, None]
            return xyz, normals
        xyz, normals = target_values(q)
        lengths = [np.linalg.norm(cp[-1]-cp[0], axis=1).mean(), np.linalg.norm(cp[:, -1]-cp[:, 0], axis=1).mean()]
        specs.append(dict(uv=q, points=xyz-origin, normals=normals, tu=tu, tv=tv, lengths=lengths))
        offsets.append(cp.reshape(-1, 3)-origin)
        for fn, record in zip(uv_curves, records):
            t = np.linspace(.001, .999, 129); _, n = chart.query(fn(t))
            seam_q = side_uv(record['mapping'], t)
            if not directed:
                probes.append((fi, seam_q, n))
        if directed:
            # Plane constraints alone admit a zero or reversed transverse
            # derivative. These directed derivatives retain the mesh map's
            # orientation, including the boundary's shared displacement.
            derivative_q = np.r_[q, np.concatenate([side_uv(m, np.linspace(0., 1., 193)) for m in mappings])]
            specs[-1]['derivative_targets'] = []
            for axis in range(2):
                step = np.eye(2)[axis]*1e-4
                plus, minus = np.clip(derivative_q+step, 0, 1), np.clip(derivative_q-step, 0, 1)
                pa, _ = target_values(plus); pb, _ = target_values(minus)
                derivative = (pa-pb)/(plus[:, axis]-minus[:, axis])[:, None]
                specs[-1]['derivative_targets'].append((derivative_q, int(axis == 0), int(axis == 1), derivative))
    poles, solver = joint_fit(specs, maps, offsets, probes, normal_weight=normal_weight,
                              fairness=fairness, seam_weight=.03 if directed else 1., quadrature_order=4, hard_tangents=hard)
    patches = []
    for fi, (p, spec) in enumerate(zip(poles, specs)):
        p = p+origin; q = spec['uv']; tu, tv = spec['tu'], spec['tv']
        a = tensor(q, tu, tv); xyz = a@p.reshape(-1, 3)
        du = tensor(q, tu, tv, 1, 0)@p.reshape(-1, 3); dv = tensor(q, tu, tv, 0, 1)@p.reshape(-1, 3)
        nn = np.cross(du, dv); nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
        dot = np.sum(nn*spec['normals'], axis=1); sign = 1 if np.median(dot) > 0 else -1
        reports.append(dict(patch=fi, distance_mm=stats(np.linalg.norm(xyz-(spec['points']+origin), axis=1)),
                            normal_degrees=stats(np.degrees(np.arccos(np.clip(dot*sign, -1, 1)))),
                            opposed_samples=int((dot*sign < 0).sum()), samples=len(q)))
        patches.append(dict(poles=p, tu=tu, tv=tv, orientation=sign))
    return patches, layout, dict(patches=reports, solver=solver)


def probe(partition, stl, output, regions, smoothing=.03, controls=17):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); start = time.perf_counter()
    v, f, labels, topology = load_partition(partition, stl)
    domains, assignment, chains, owners, curves, lookup, errors = prepare(v, f, labels, smoothing)
    dump(out/'network.json', dict(domains=len(domains), curves=len(curves), movement_mm=stats(errors),
                                  smoothing_mm=smoothing, source_CAD_read=False, commercial_engine_used=False))
    selected = [(i, d) for i, d in enumerate(domains) if d['region'] in regions]; results = []
    for di, domain in selected:
        path = out/f'region_{domain["region"]}_domain_{di}'; path.mkdir()
        chart = CircularChart(v, f, domain); outer, references = outer_sides(v, domain, chains, curves, lookup)
        patches, layout, info = fit_ring(chart, outer, controls)
        shape = cad.construct([p['poles'] for p in patches], [(p['tu'], p['tv']) for p in patches],
                              [p['orientation'] for p in patches], False, 1e-6)
        cad.write_step(path/'regional.step', shape)
        info.update(region=domain['region'], domain=di, chart=chart.report,
                    topology=cad.topology(cad.read_generated(path/'regional.step')),
                    actual_control_points=sum(p['poles'].shape[0]*p['poles'].shape[1] for p in patches))
        dump(path/'run.json', info)
        for i, p in enumerate(patches):
            np.savez_compressed(path/f'patch_{i}.npz', **p)
        results.append(info); print(info, flush=True)
    dump(out/'run.json', dict(status='local_diagnostic', partition=str(Path(partition).resolve()), stl=str(Path(stl).resolve()),
                              smoothing_mm=smoothing, controls=controls, results=results, seconds=time.perf_counter()-start))


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--partition', required=True); p.add_argument('--stl', required=True)
    p.add_argument('--output', required=True); p.add_argument('--regions', type=int, nargs='+', default=[179])
    p.add_argument('--smoothing', type=float, default=.03); p.add_argument('--controls', type=int, default=17)
    a = p.parse_args(); probe(a.partition, a.stl, a.output, a.regions, a.smoothing, a.controls)
