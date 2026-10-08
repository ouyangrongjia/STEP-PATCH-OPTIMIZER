"""Generalize the compact shared-curve experiment to fixed mesh regions.

Disk charts use positive mean-value coordinates. A fan in that domain is
quadrangulated inside each original region; shared XYZ curves are immutable
and embedded into each incident Coons support. All fitting is mesh-only.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import time
import numpy as np
from scipy import sparse
from scipy.interpolate import BSpline, splprep

from .mesh import (Locator, dump, mean_value, distortion, vertex_normals, stats,
                   chain_parameters)
from .splines import basis, knots, joint_fit, tensor
from .compact import curve, unpack, side_mapping, side_uv
from .partition_fit import load_partition, incidence, boundary_edges, loops, split_faces
from .partition_cad import shared_chains


@dataclass
class FanConfig:
    curve_tolerance: float = .002
    minimum_controls: int = 7
    interior_controls: int = 7
    samples_per_patch: int = 625
    normal_weight: float = .01
    fairness: float = 1e-5
    seam_weight: float = 1.
    construction_tolerance: float = 1e-7
    max_patches: int = 12000
    maximum_chart_stretch: float = 50.
    interior_mode: str = 'coarse_correction'
    regularize_correction: bool = True


def disk_domains(v, f, labels):
    edges, owners = incidence(f)
    graph = sparse.coo_matrix((np.ones(owners.size), (owners.ravel(), owners[:, ::-1].ravel())),
                              shape=(len(f), len(f))).tocsr()
    domains = []; assigned = np.full(len(f), -1, dtype=np.int64)
    for region in np.unique(labels):
        source_ids = np.flatnonzero(labels == region)
        count, component = sparse.csgraph.connected_components(graph[source_ids][:, source_ids], directed=False)
        queue = [(source_ids[component == i], 0) for i in range(count)]
        while queue:
            ids, depth = queue.pop(0); used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids])
            try:
                loop = loops(boundary_edges(tri))
                if len(loop) != 1:
                    raise ValueError('Multiple loops')
            except ValueError:
                if depth >= 24 or len(ids) <= 1:
                    raise ValueError(f'Cannot make a disk inside region {region}')
                queue.extend((part, depth+1) for part in split_faces(v, f, ids, graph))
                continue
            loop = used[loop[0]]
            length = np.linalg.norm(v[np.roll(loop, -1)]-v[loop], axis=1)
            domains.append(dict(region=int(region), triangles=ids, vertices=used, loop=loop,
                                perimeter=float(length.sum())))
            assigned[ids] = len(domains)-1
    if np.any(assigned < 0):
        raise ValueError('A triangle was not assigned to a fitting domain')
    return domains, assigned, edges, owners


def _fit_curve_once(points, parameters, tolerance, minimum=4, end_normals=None):
    """Adaptive C2 cubic curve; common endpoints remain exact."""
    points = np.asarray(points); parameters = np.asarray(parameters)
    if len(points) == 2:
        p = points[0]+np.linspace(0., 1., 4)[:, None]*(points[1]-points[0])
        return curve(p, knots(4)), 0.
    for scale in (1., .1, .01, 0.):
        degree = min(3, len(points)-1)
        (t, cp, _), _ = splprep(points.T, u=parameters, s=len(points)*(tolerance*.1)**2*scale, k=degree)
        cp = np.asarray(cp).T
        if degree != 3:
            # Degree elevation preserves the polynomial exactly.
            from OCP.Geom import Geom_BSplineCurve
            from OCP.TColgp import TColgp_Array1OfPnt
            from OCP.gp import gp_Pnt
            from .partition_cad import array
            p = TColgp_Array1OfPnt(1, len(cp))
            for i, value in enumerate(cp):
                p.SetValue(i+1, gp_Pnt(*value))
            unique, multi = np.unique(t, return_counts=True)
            c = Geom_BSplineCurve(p, array(unique), array(multi, True), degree)
            c.IncreaseDegree(3)
            cp = np.array([c.Pole(i).Coord() for i in range(1, c.NbPoles()+1)])
            t = np.repeat([c.Knot(i) for i in range(1, c.NbKnots()+1)], [c.Multiplicity(i) for i in range(1, c.NbKnots()+1)])
        cp[[0, -1]] = points[[0, -1]]
        if end_normals is not None:
            a = basis(t, parameters)
            free = a[:, 1:-1]
            target = points-a[:, [0, -1]]@cp[[0, -1]]
            h = free.T@free+np.eye(free.shape[1])*1e-12
            coeff = np.linalg.solve(h, free.T@target)
            constraint = np.zeros((2, 3, len(coeff)))
            constraint[0, :, 0] = end_normals[0]
            constraint[1, :, -1] = end_normals[1]
            c = constraint.reshape(2, -1)
            inverse = np.stack([np.linalg.solve(h, constraint[:, k, :].T) for k in range(3)], axis=0).reshape(-1, 2)
            value = np.sum(cp[[0, -1]]*end_normals, axis=1)
            correction = inverse@np.linalg.lstsq(c@inverse, c@coeff.T.ravel()-value, rcond=1e-12)[0]
            cp[1:-1] = coeff-correction.reshape(3, -1).T
        c = curve(cp, t)
        for knot in np.linspace(0., 1., minimum-2)[1:-1]:
            c.InsertKnot(float(knot), 1, 1e-12, False)
        probe_t = (parameters[:-1, None]*(1-np.linspace(0., 1., 5))+
                   parameters[1:, None]*np.linspace(0., 1., 5)).ravel()
        target = np.stack([np.interp(probe_t, parameters, points[:, k]) for k in range(3)], axis=1)
        predicted = np.array([c.Value(float(x)).Coord() for x in probe_t])
        error = float(np.linalg.norm(predicted-target, axis=1).max())
        if error <= tolerance:
            return c, error
    raise ValueError(f'C2 curve cannot approximate this chain within {tolerance}: {error}')


def fit_curve(points, parameters, tolerance, minimum=4, end_normals=None):
    """Resolve polyline corners before splitting a shared boundary chain.

    Interpolating only the original vertices can overshoot a short segment
    between two long ones. Refinement adds samples on the same polyline; it
    does not move vertices or change region labels.
    """
    points = np.asarray(points); parameters = np.asarray(parameters)
    last = None
    for subdivisions in (1, 4, 12):
        if subdivisions == 1:
            sample_t, xyz = parameters, points
        else:
            fractions = np.linspace(0., 1., subdivisions+1)
            sample_t = np.unique((parameters[:-1, None]*(1-fractions)+
                                  parameters[1:, None]*fractions).ravel())
            xyz = np.stack([np.interp(sample_t, parameters, points[:, k]) for k in range(3)], axis=1)
        try:
            return _fit_curve_once(xyz, sample_t, tolerance, minimum, end_normals)
        except ValueError as error:
            last = error
    raise last


def half_curve(c, a, b):
    from .partition_cad import array
    result = c.Copy(); result.Segment(float(a), float(b), 1e-12)
    values = [(result.Knot(i)-a)/(b-a) for i in range(1, result.NbKnots()+1)]
    values[0], values[-1] = 0., 1.
    result.SetKnots(array(values))
    return result


class MeshChart:
    def __init__(self, v, f, domain):
        used = domain['vertices']; self.xyz = v[used]
        self.tri = np.searchsorted(used, f[domain['triangles']])
        loop = np.searchsorted(used, domain['loop'])
        lengths = np.linalg.norm(self.xyz[np.roll(loop, -1)]-self.xyz[loop], axis=1)
        a = np.r_[0., np.cumsum(lengths[:-1])]/lengths.sum()*2*np.pi
        fixed = np.c_[np.cos(a), np.sin(a)]
        self.uv, residual = mean_value(self.xyz, self.tri, loop, fixed)
        quality = distortion(self.xyz, self.tri, self.uv)
        if quality['flipped'] or quality['zero']:
            raise ValueError(f'MVC chart is not injective: {quality}')
        self.normals = domain.get('reference_normals')
        if self.normals is None:
            self.normals = vertex_normals(self.xyz, self.tri)
        self.locator = Locator(np.c_[self.uv, np.zeros(len(self.uv))], self.tri)
        self.global_ids = used
        self.report = dict(uv=quality, solve_residual=residual)

    def query(self, uv):
        _, distance, ids, bary = self.locator.closest(np.c_[uv, np.zeros(len(uv))])
        if distance.max() > 1e-7:
            raise ValueError(f'Layout leaves the actual discrete UV domain: {distance.max()}')
        p = np.einsum('ij,ijk->ik', bary, self.xyz[self.tri[ids]])
        n = np.einsum('ij,ijk->ik', bary, self.normals[self.tri[ids]])
        n /= np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-30)
        return p, n


def regular_domains(v, f, labels, config):
    """Split only fitting domains; original region labels remain unchanged."""
    initial, _, edges, owners = disk_domains(v, f, labels)
    graph = sparse.coo_matrix((np.ones(owners.size), (owners.ravel(), owners[:, ::-1].ravel())),
                              shape=(len(f), len(f))).tocsr()
    region_normals = {}
    for region in np.unique(labels):
        tri = f[labels == region]; used = np.unique(tri)
        region_normals[int(region)] = (used, vertex_normals(v[used], np.searchsorted(used, tri)))
    queue = [(d, 0) for d in initial]; accepted = []; assigned = np.full(len(f), -1, dtype=np.int64)
    while queue:
        domain, depth = queue.pop(0); used = domain['vertices']
        normal_ids, normals = region_normals[domain['region']]
        domain['reference_normals'] = normals[np.searchsorted(normal_ids, used)]
        chart = None
        try:
            chart = MeshChart(v, f, domain)
        except ValueError:
            pass
        if chart is not None and chart.report['uv']['stretch_ratio']['max'] <= config.maximum_chart_stretch:
            domain['chart'] = chart; domain['split_depth'] = depth
            assigned[domain['triangles']] = len(accepted); accepted.append(domain)
            if len(accepted)+len(queue) > config.max_patches//3:
                raise ValueError('Regular fitting domains already exceed the minimum patch budget')
            continue
        ids = domain['triangles']
        if depth >= 24 or len(ids) <= 1:
            raise ValueError(f'Cannot obtain a regular chart in region {domain["region"]}')
        for part in split_faces(v, f, ids, graph):
            used = np.unique(f[part]); boundary = loops(boundary_edges(np.searchsorted(used, f[part])))
            if len(boundary) != 1:
                raise ValueError('A regularity split created a nondisk domain')
            loop = used[boundary[0]]
            length = np.linalg.norm(v[np.roll(loop, -1)]-v[loop], axis=1).sum()
            queue.append((dict(region=domain['region'], triangles=part, vertices=used,
                               loop=loop, perimeter=float(length)), depth+1))
    if np.any(assigned < 0):
        raise ValueError('Regular chart construction lost source triangles')
    return accepted, assigned, edges, owners


def prepare_network(v, domains, assigned, edges, owners, config):
    source, adjacent = shared_chains(edges, owners, assigned)
    result, curves, errors = [], [], []
    queue = list(zip(source, adjacent))
    while queue:
        chain, pair = queue.pop(0)
        t = chain_parameters(v, chain)
        length = float(np.linalg.norm(np.diff(v[chain], axis=0), axis=1).sum())
        limit = min(domains[int(i)]['perimeter'] for i in pair)/4
        if length > limit and len(chain) > 2:
            middle = int(np.clip(np.searchsorted(t, .5), 1, len(chain)-2))
            queue[0:0] = [(chain[:middle+1], pair), (chain[middle:], pair)]
            continue
        try:
            curve_value, error = fit_curve(v[chain], t, config.curve_tolerance)
        except ValueError:
            if len(chain) <= 2:
                raise
            middle = len(chain)//2
            queue[0:0] = [(chain[:middle+1], pair), (chain[middle:], pair)]
            continue
        result.append(chain); curves.append(curve_value); errors.append(error)
    # Canonical endpoint IDs are used for topology, never proximity merging.
    lookup = {}
    for ci, chain in enumerate(result):
        for a, b in zip(chain[:-1], chain[1:]):
            lookup[tuple(sorted((int(a), int(b))))] = (ci, int(a))
    return result, curves, lookup, errors


def coons_domain(uv, sides, mappings):
    values = {}; corners = {}
    for edge_uv, mapping in zip(sides, mappings):
        varying = 1-mapping['axis']; q = uv[:, varying]
        t = 1-q if mapping['reverse'] else q
        values[(mapping['axis'], mapping['fixed'])] = edge_uv(t)
        for x in (0., 1.):
            location = side_uv(mapping, np.array([x]))[0]
            corners[tuple(location)] = edge_uv(np.array([x]))[0]
    u, w = uv[:, 0, None], uv[:, 1, None]
    linear = (1-u)*(1-w)*corners[(0., 0.)]+u*(1-w)*corners[(1., 0.)]+u*w*corners[(1., 1.)]+(1-u)*w*corners[(0., 1.)]
    return ((1-w)*values[(1, 0.)]+w*values[(1, 1.)]+
            (1-u)*values[(0, 0.)]+u*values[(0, 1.)]-linear)


def fit_patch(curves, curve_uv, chart, config, diagnostic_path=None):
    from OCP.GeomFill import GeomFill_BSplineCurves, GeomFill_CoonsStyle
    from .partition_cad import array
    support = GeomFill_BSplineCurves(*[c.Copy() for c in curves], GeomFill_CoonsStyle).Surface()
    # OCCT may return 1 +/- machine epsilon after curve reversal. The fitting
    # basis has a closed [0, 1] domain; keep its endpoint knots exactly there.
    for get, count, setter in [(support.UKnot, support.NbUKnots(), support.SetUKnots),
                               (support.VKnot, support.NbVKnots(), support.SetVKnots)]:
        t = np.array([get(i) for i in range(1, count+1)])
        t = (t-t[0])/(t[-1]-t[0]); t[0], t[-1] = 0., 1.; setter(array(t))
    coarse = knots(config.interior_controls)
    for value in np.unique(coarse)[1:-1]:
        support.InsertUKnot(float(value), 1, 1e-12, False)
        support.InsertVKnot(float(value), 1, 1e-12, False)
    mappings = [side_mapping(support, c) for c in curves]
    p, tu, tv = unpack(support); nu, nv = p.shape[:2]
    def refinement(target):
        refined = BSpline(coarse, np.eye(config.interior_controls), 3)
        for value in np.unique(target[4:-4]):
            required=int(np.sum(abs(target-value)<1e-12))
            present=int(np.sum(abs(refined.t-value)<1e-12))
            if required>present:
                refined = refined.insert_knot(value,required-present)
        if len(refined.t) != len(target) or np.max(abs(refined.t-target)) > 1e-12:
            raise ValueError('Interior basis is not nested in the shared support')
        return sparse.csr_matrix(refined.c[:, 2:-2])
    # Boundary complexity does not set the number of interior unknowns.
    # Knot insertion embeds this correction exactly and leaves all four sides
    # unchanged. Actual exported control points are still reported separately.
    bulk = sparse.kron(refinement(tu), refinement(tv), format='csr')
    ii, jj = np.indices((nu, nv))
    strip = np.flatnonzero(((ii == 1) | (ii == nu-2) | (jj == 1) | (jj == nv-2))
                          & (ii > 0) & (ii < nu-1) & (jj > 0) & (jj < nv-1))
    tangent_strip = sparse.csr_matrix((np.ones(len(strip)), (strip, np.arange(len(strip)))),
                                     shape=(nu*nv, len(strip)))
    mapping = sparse.hstack([tangent_strip, bulk], format='csr')
    if config.interior_mode == 'full':
        free = np.flatnonzero((ii > 0) & (ii < nu-1) & (jj > 0) & (jj < nv-1))
        mapping = sparse.csr_matrix((np.ones(len(free)), (free, np.arange(len(free)))),
                                    shape=(nu*nv, len(free)))
    elif config.interior_mode != 'coarse_correction':
        raise ValueError(f'Unknown regional interior basis: {config.interior_mode}')
    # Tensor samples include each knot span so narrow boundary spans are seen.
    gu = np.unique(np.r_[np.linspace(.001, .999, int(np.sqrt(config.samples_per_patch))),
                         (np.unique(tu)[1:]+np.unique(tu)[:-1])/2])
    gv = np.unique(np.r_[np.linspace(.001, .999, int(np.sqrt(config.samples_per_patch))),
                         (np.unique(tv)[1:]+np.unique(tv)[:-1])/2])
    uv = np.stack(np.meshgrid(gu, gv, indexing='ij'), axis=-1).reshape(-1, 2)
    target_uv = coons_domain(uv, curve_uv, mappings)
    xyz, normal = chart.query(target_uv)
    # Determine orientation from the mesh domain, independent of Coons axes.
    eps = 1e-5; center = np.array([[.5, .5]])
    dx = coons_domain(center+[eps, 0], curve_uv, mappings)-coons_domain(center-[eps, 0], curve_uv, mappings)
    dy = coons_domain(center+[0, eps], curve_uv, mappings)-coons_domain(center-[0, eps], curve_uv, mappings)
    sign = 1 if dx[0, 0]*dy[0, 1]-dx[0, 1]*dy[0, 0] > 0 else -1
    lengths = [max(np.linalg.norm(p[-1, :]-p[0, :], axis=1).mean(), 1e-8),
               max(np.linalg.norm(p[:, -1]-p[:, 0], axis=1).mean(), 1e-8)]
    origin = p[0, 0].copy()
    spec = dict(uv=uv, points=xyz-origin, normals=normal, tu=tu, tv=tv, lengths=lengths,
                regularize_correction=config.regularize_correction, local_tangent_scaling=True)
    probes = []; oriented = []
    for fn, m in zip(curve_uv, mappings):
        t = np.linspace(.001, .999, 33)
        _, n = chart.query(fn(t)); seam_uv = side_uv(m, t)
        probes.append((0, seam_uv, n))
        for axis in range(2):
            step = np.eye(2)[axis]*1e-5
            plus = np.clip(seam_uv+step, 0., 1.); minus = np.clip(seam_uv-step, 0., 1.)
            pa, _ = chart.query(coons_domain(plus, curve_uv, mappings))
            pb, _ = chart.query(coons_domain(minus, curve_uv, mappings))
            direction = pa-pb
            direction -= n*np.sum(direction*n, axis=1)[:, None]
            direction /= (plus[:, axis]-minus[:, axis])[:, None]
            oriented.append((seam_uv, int(axis == 0), int(axis == 1), direction))
    spec['derivative_targets'] = oriented
    length_scale = min(lengths)
    # Directed cross-boundary derivatives prevent the sign ambiguity of a
    # normal-plane equation. Their scale comes from the mesh map; a Coons
    # support may have a vanishing transverse derivative before fitting.
    fitted, solver = joint_fit([spec], [mapping], [p.reshape(-1, 3)-origin], [],
                              normal_weight=config.normal_weight*length_scale**2,
                              fairness=config.fairness*length_scale**4,
                              seam_weight=config.seam_weight*length_scale**2, quadrature_order=4)
    poles = fitted[0]+origin
    error = np.linalg.norm(tensor(uv, tu, tv)@poles.reshape(-1, 3)-xyz, axis=1)
    if error.max() > np.linalg.norm(np.ptp(xyz, axis=0)):
        if diagnostic_path is not None:
            np.savez_compressed(diagnostic_path, initial_poles=p, tu=tu, tv=tv,
                                uv=uv, points=xyz, normals=normal, lengths=lengths,
                                seam_uv=np.concatenate([x[1] for x in probes]),
                                seam_normals=np.concatenate([x[2] for x in probes]),
                                mapping_data=mapping.data, mapping_indices=mapping.indices,
                                mapping_indptr=mapping.indptr, mapping_shape=mapping.shape)
        raise ValueError(f'Gross patch distortion exceeds its input extent: {error.max()} mm')
    return poles, tu, tv, sign, mappings, dict(samples=len(xyz), controls=[nu, nv],
                                            training_distance_mm=stats(error), solver=solver)


def fit_domain(v, f, domain, di, chains, boundary, lookup, config, diagnostic_path=None):
    """Fit one domain while preserving its original region and global curves."""
    patches = []; graph_curves = {}; graph_endpoints = {}; reports = []
    def register(key, c, endpoints):
        if key not in graph_curves:
            graph_curves[key] = c; graph_endpoints[key] = endpoints
        return key

    chart = domain.get('chart')
    if chart is None:
        chart = MeshChart(v, f, domain)
    references = []
    loop = domain['loop']
    for a, b in zip(loop, np.roll(loop, -1)):
        ci, av = lookup[tuple(sorted((int(a), int(b))))]
        ref = (ci, int(a) == av)
        if not references or references[-1] != ref:
            references.append(ref)
    if len(references) > 1 and references[0] == references[-1]:
        references.pop()
    center, center_n = chart.query(np.array([[0., 0.]]))
    rim = []; qsample = np.linspace(0., 1., 129)
    qsample = np.where(qsample <= .5, 2*qsample*qsample, 1-2*(1-qsample)**2)
    for ci, forward in references:
        chain = chains[ci] if forward else chains[ci][::-1]
        t = chain_parameters(v, chain)
        uv = chart.uv[np.searchsorted(chart.global_ids, chain)]
        cu = lambda s, t=t, uv=uv: np.stack([np.interp(s, t, uv[:, k]) for k in range(2)], axis=1)
        c = boundary[ci].Copy()
        if not forward:
            c.Reverse()
        endpoint_uv = cu(np.array([.5]))[0]
        path = (1-qsample[:, None])*endpoint_uv
        xyz, normal = chart.query(path)
        blend = 1-3*qsample**2+2*qsample**3
        displacement = np.array(c.Value(.5).Coord())-xyz[0]
        xyz += blend[:, None]*displacement
        xyz[-1] = center[0]
        rc, _ = fit_curve(xyz, qsample, config.curve_tolerance, config.minimum_controls,
                          end_normals=np.array([normal[0], center_n[0]]))
        radial_key = register(('radial', di, ci), rc, (('mid', ci), ('center', di)))
        radial_uv = lambda s, endpoint_uv=endpoint_uv: (1-s[:, None])*endpoint_uv
        rim.append((ci, forward, chain, c, cu, rc, radial_uv, radial_key))
    # One quadrilateral per boundary corner. Both halves of every rim
    # curve are shared globally; midpoint-to-center curves stay inside
    # their original region. This avoids three quads per fan sector.
    for si, current in enumerate(rim):
        previous = rim[si-1]
        ci, forward, chain, outer, outer_uv, radial, radial_uv, radial_key = current
        pci, pforward, pchain, pouter, pouter_uv, pradial, pradial_uv, pradial_key = previous
        first = half_curve(outer, 0., .5)
        last = half_curve(pouter, .5, 1.)
        back = pradial.Copy(); back.Reverse()
        quad = [first, radial, back, last]
        quv = [lambda s, fn=outer_uv: fn(s*.5), radial_uv,
               lambda s, fn=pradial_uv: fn(1-s), lambda s, fn=pouter_uv: fn(.5+s*.5)]
        segment_keys = []
        for edge_id, direction, fraction, value, endpoints in [
            (ci, forward, 0, first, (('mesh', int(chain[0])), ('mid', ci))),
            (pci, pforward, 1, last, (('mid', pci), ('mesh', int(chain[0]))))]:
            key = ('boundary', edge_id, 'half', fraction if direction else 1-fraction)
            canonical = value.Copy()
            if not direction:
                canonical.Reverse(); endpoints = endpoints[::-1]
            register(key, canonical, endpoints); segment_keys.append(key)
        curve_keys = [segment_keys[0], radial_key, pradial_key, segment_keys[1]]
        curve_directions = [forward, True, False, pforward]
        poles, tu, tv, sign, mappings, report = fit_patch(quad, quv, chart, config, diagnostic_path)
        pi = len(patches)
        patches.append(dict(poles=poles, tu=tu, tv=tv, orientation=sign,
                            curves=curve_keys, directions=curve_directions, mappings=mappings,
                            region=domain['region'], domain=di))
        reports.append(dict(patch=pi, region=domain['region'], domain=di, **report))
    return patches, graph_curves, graph_endpoints, reports


def reconstruct(partition, stl, output, config):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); (out/'patches').mkdir()
    started = time.perf_counter()
    run = dict(status='running', algorithm='fixed-region-shared-curve-fan-v11', config=asdict(config),
               partition=str(Path(partition).resolve()), stl=str(Path(stl).resolve()), source_CAD_read=False)
    dump(out/'run.json', run)
    try:
        v, f, labels, topo = load_partition(partition, stl)
        domains, assigned, edges, owners = regular_domains(v, f, labels, config)
        chains, boundary, lookup, errors = prepare_network(v, domains, assigned, edges, owners, config)
        run.update(input_topology=topo, input_regions=len(np.unique(labels)), fitting_domains=len(domains),
                   boundary_curve_error_mm=stats(errors), preparation_seconds=time.perf_counter()-started)
        if 2*len(chains) > config.max_patches:
            raise ValueError(f'Declared patch budget exceeded before fitting: {2*len(chains)} > {config.max_patches}')
        print(json.dumps(dict(stage='domains', regions=run['input_regions'], domains=len(domains), curves=len(chains))), flush=True)
        patches = []; graph_curves = {}; graph_endpoints = {}; reports = []

        for di, domain in enumerate(domains):
            fitted, curves, endpoints, domain_reports = fit_domain(
                v, f, domain, di, chains, boundary, lookup, config, out/'failed_fit_inputs.npz')
            for key, curve_value in curves.items():
                graph_curves.setdefault(key, curve_value)
                if key in graph_endpoints and graph_endpoints[key] != endpoints[key]:
                    raise ValueError('Shared curve endpoint identity changed between domains')
                graph_endpoints[key] = endpoints[key]
            for rec, report in zip(fitted, domain_reports):
                pi = len(patches)
                np.savez_compressed(out/'patches'/f'{pi:05d}.npz', poles=rec['poles'], tu=rec['tu'], tv=rec['tv'],
                                    region=rec['region'], domain=di, orientation=rec['orientation'])
                report['patch'] = pi; patches.append(rec); reports.append(report)
            print(json.dumps(dict(stage='fit', domain=di, region=domain['region'], patches=len(patches), seconds=time.perf_counter()-started)), flush=True)
        np.savez_compressed(out/'source_domains.npz', face_domain=assigned, source_labels=labels,
                            domain_region=np.array([d['region'] for d in domains]))
        dump(out/'fitting.json', dict(patches=reports, partition_labels_unchanged=True,
                                     domain_parameterization=[dict(region=d['region'], depth=d['split_depth'],
                                                                    **d['chart'].report) for d in domains],
                                     complete_source_triangle_coverage=True, seconds=time.perf_counter()-started))
        from .partition_fan_cad import assemble
        result = assemble(patches, graph_curves, graph_endpoints, config.construction_tolerance, out)
        run.update(result, status='DiagnosticOnly', can_apply=False)
    except Exception as error:
        run.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        run['wall_seconds'] = time.perf_counter()-started; dump(out/'run.json', run)


def main():
    p = argparse.ArgumentParser(); p.add_argument('--partition', required=True); p.add_argument('--stl', required=True)
    p.add_argument('--output', required=True); p.add_argument('--curve-tolerance', type=float, default=.002)
    p.add_argument('--minimum-controls', type=int, default=7); p.add_argument('--samples', type=int, default=625)
    p.add_argument('--interior-controls', type=int, default=7)
    a = p.parse_args(); reconstruct(a.partition, a.stl, a.output,
                                    FanConfig(curve_tolerance=a.curve_tolerance,
                                              minimum_controls=a.minimum_controls, interior_controls=a.interior_controls,
                                              samples_per_patch=a.samples))


if __name__ == '__main__':
    main()
