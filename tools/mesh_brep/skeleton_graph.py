"""Fit a compact regional skeleton before constructing its trimmed supports.

Labels are immutable. Internal rings isolate boundary detail from large smooth
interiors. Shared cubic curves are fitting constraints, rather than trims added
after an independent fit. Graph supports cannot fold in their own projection.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, replace
import json
from pathlib import Path
import time
import numpy as np
from scipy import sparse
from scipy.sparse.linalg import spsolve
from scipy.interpolate import BSpline

from .mesh import dump, stats, vertex_normals, chain_parameters
from .partition_fit import (Config, load_partition, incidence, frame, boundary_edges,
                            loops, split_faces, projected_boundary_valid, parameters,
                            spline_graph_error, positions)
from .partition_fan import disk_domains
from .regional_rectangle import corners_for_loop, curve_polyline_error
from .regional_compact import CircularChart
from .partition_cad import shared_chains
from .splines import knots, tensor


def stable_frame(v, f):
    initial = frame(v, f)
    o, axes, _, _, _, _, dot = initial
    cross = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    normal = cross/np.linalg.norm(cross, axis=1)[:, None]
    candidates = [axes[2], cross.sum(axis=0), normal.sum(axis=0)]
    candidates = [x/np.linalg.norm(x) for x in candidates if np.linalg.norm(x) > 1e-14]
    direction = max(candidates, key=lambda x: float(np.min(normal@x)))
    if np.min(normal@direction) <= .15 and len(f) > 1:
        conditioned = frame(v, f, normal)
        if conditioned[-1].min() > np.min(normal@direction):
            return conditioned
    if np.min(normal@direction) <= dot.min()+1e-10:
        return initial
    axes = axes.copy(); axes[2] = direction
    tangent = axes[0]-np.dot(axes[0], direction)*direction
    if np.linalg.norm(tangent) < 1e-10:
        tangent = axes[1]-np.dot(axes[1], direction)*direction
    axes[0] = tangent/np.linalg.norm(tangent); axes[1] = np.cross(direction, axes[0])
    xyz = (v-o)@axes.T; lo = xyz[:, :2].min(axis=0); size = np.ptp(xyz[:, :2], axis=0)
    return o, axes, lo, size, (xyz[:, :2]-lo)/size, xyz[:, 2], normal@direction


def normal_error(cp, kt, points, t, normals):
    """Cubic/linear residual extrema against both incident source planes."""
    from numpy.polynomial.chebyshev import chebvander, chebder, chebroots, chebval
    cuts = np.unique(np.r_[kt, t]); nodes = np.cos(np.arange(4)*np.pi/3)
    q = cuts[:-1, None]+np.diff(cuts)[:, None]*(nodes+1)/2
    xyz = BSpline(kt, cp, 3)(q.ravel())
    target = np.stack([np.interp(q.ravel(), t, points[:, k]) for k in range(3)], axis=1)
    segment = np.clip(np.searchsorted(t, (cuts[:-1]+cuts[1:])/2)-1, 0, len(t)-2)
    residual = np.einsum('std,snd->stn', (xyz-target).reshape(-1, 4, 3), normals[segment])
    coeff = np.linalg.solve(chebvander(nodes, 3), residual.transpose(1, 0, 2).reshape(4, -1))
    maximum, where = 0., 0.
    for i, c in enumerate(coeff.T):
        roots = chebroots(chebder(c)); roots = roots.real[(abs(roots.imag) < 1e-8) & (abs(roots.real) <= 1)]
        q = np.r_[-1., 1., roots]; values = abs(chebval(q, c)); j = int(np.argmax(values))
        if values[j] > maximum:
            index = i//normals.shape[1]; maximum = float(values[j])
            where = float(cuts[index]+(q[j]+1)*np.diff(cuts)[index]/2)
    return maximum, where


def fit_boundary(points, t, budget, normals=None, normal_budget=.003):
    # Positive B-spline approximation of the control polygon cannot introduce
    # the least-squares curve's overshooting endpoint loops. Endpoint spans
    # retain the source outgoing directions at topological junctions.
    breaks = np.unique(np.r_[np.linspace(0, 1, 5), t[1], t[-2]])
    probe = np.unique((t[:-1, None]+np.diff(t)[:, None]*np.linspace(0, 1, 5)).ravel())
    target = np.stack([np.interp(probe, t, points[:, k]) for k in range(3)], axis=1)
    if normals is not None:
        segment = np.clip(np.searchsorted(t, probe, side='right')-1, 0, len(t)-2)
    # A long feature chain can contain more than 100 localized bends. Budget
    # refinement by source complexity before retaining an exact C0 fallback.
    for _ in range(max(100, 4*len(points))):
        kt = np.r_[np.zeros(3), breaks, np.ones(3)]; n = len(kt)-4
        g = np.array([kt[i+1:i+4].mean() for i in range(n)])
        cp = np.stack([np.interp(g, t, points[:, k]) for k in range(3)], axis=1)
        delta = BSpline(kt, cp, 3)(probe)-target
        residual = np.linalg.norm(delta, axis=1); ratio = residual/budget
        if normals is not None:
            plane = abs(np.einsum('nd,nkd->nk', delta, normals[segment])).max(axis=1)
            ratio = np.maximum(ratio, plane/normal_budget)
        where = float(probe[np.argmax(ratio)])
        if ratio.max() <= 1:
            error, where = curve_polyline_error(cp, kt, points, t)
            if error <= budget:
                if normals is None:
                    return cp, kt, error
                peak, where = normal_error(cp, kt, points, t, normals)
                if peak <= normal_budget:
                    return cp, kt, error
        i = int(np.clip(np.searchsorted(breaks, where)-1, 0, len(breaks)-2))
        midpoint = (breaks[i]+breaks[i+1])/2
        if midpoint in breaks:
            break
        breaks = np.sort(np.r_[breaks, midpoint])
    # A failed cubic approximation is retained as an explicit exact source
    # polyline; its C0 knots remain visible in the saved network evidence.
    cp = np.concatenate([points[i]+np.linspace(0, 1, 4)[:, None]*(points[i+1]-points[i])
                         if i == 0 else points[i]+np.linspace(0, 1, 4)[1:, None]*(points[i+1]-points[i])
                         for i in range(len(points)-1)])
    kt = np.repeat(t, np.r_[4, np.full(len(t)-2, 3), 4])
    return cp, kt, 0.


def initial_domains(v, f, labels, ring_threshold=2000, minimum_cosine=.08, conditioning_distance=None):
    domains, _, edges, owners = disk_domains(v, f, labels)
    graph = sparse.coo_matrix((np.ones(owners.size), (owners.ravel(), owners[:, ::-1].ravel())),
                             shape=(len(f), len(f))).tocsr()
    seeds = []
    for d in domains:
        ids = d['triangles']
        if len(ids) >= ring_threshold and len(d['loop']) >= 4:
            d['corners'] = corners_for_loop(v, d['loop']); chart = CircularChart(v, f, d)
            q = chart.uv[chart.tri].mean(axis=1)
            part = (np.floor((np.arctan2(q[:, 1], q[:, 0]) % (2*np.pi))/(np.pi/2))).astype(int)
            part[np.abs(q).sum(axis=1) < .65] = 4
            part = connected_ring(part, graph[ids][:, ids])
            for label in np.unique(part):
                selected = ids[part == label]
                count, cc = sparse.csgraph.connected_components(graph[selected][:, selected], directed=False)
                seeds.extend((d['region'], selected[cc == k]) for k in range(count))
        else:
            seeds.append((d['region'], ids))
    return regular_domains(v, f, graph, seeds, minimum_cosine, conditioning_distance), graph, edges, owners


def connected_ring(part, graph):
    """Absorb rasterized cut slivers inside the same supplied region.

    Keep each intended ring sector's largest component, then grow those five
    connected domains across the remaining cut cells. No GUI label is read or
    modified by this operation; every changed cell is inside this one region.
    """
    result = np.full(len(part), -1, dtype=int)
    for label in np.unique(part):
        ids = np.flatnonzero(part == label)
        count, cc = sparse.csgraph.connected_components(graph[ids][:, ids], directed=False)
        main = int(np.argmax(np.bincount(cc))); result[ids[cc == main]] = label
    while np.any(result < 0):
        changed = 0
        for i in np.flatnonzero(result < 0):
            neighbors = result[graph.indices[graph.indptr[i]:graph.indptr[i+1]]]
            neighbors = neighbors[neighbors >= 0]
            if len(neighbors):
                result[i] = int(np.argmax(np.bincount(neighbors))); changed += 1
        if not changed:
            raise ValueError('Ring cut has a disconnected source component')
    return result


def regular_domains(v, f, graph, seeds, minimum_cosine=.08, conditioning_distance=None):
    result = []
    while seeds:
        region, ids = seeds.pop(0); used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids])
        accepted = False
        try:
            bd = boundary_edges(tri); found = loops(bd)
            _, _, _, _, uv, height, dot = stable_frame(v[used], tri)
            conditioned = dot.min() > minimum_cosine
            if conditioning_distance is not None:
                # Tiny departures from a plane do not justify preemptive
                # subdivision solely for an isolated grazing source normal.
                conditioned |= dot.min() > .08 and abs(height).max() <= conditioning_distance
            accepted = len(found) >= 1 and conditioned and projected_boundary_valid(uv, bd)
        except ValueError:
            pass
        if accepted:
            result.append(dict(region=region, triangle_ids=ids, vertex_ids=used))
        elif len(ids) <= 1:
            raise ValueError('Degenerate seed domain')
        else:
            seeds.extend((region, part) for part in split_faces(v, f, ids, graph))
    return result


def skeleton(v, f, domains, edges, owners, smoothing, normal_budget=.003, curve_cache=None, boundary_network=None,
             diagnostics=None):
    assigned = np.full(len(f), -1, dtype=int)
    for i, d in enumerate(domains):
        assigned[d['triangle_ids']] = i
    chains, pairs = shared_chains(edges, owners, assigned); curves, samples, errors, chain_normals = [], [], [], []
    if boundary_network is not None:
        chains, pairs = boundary_network.split_chains(chains, pairs)
    fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    fn /= np.linalg.norm(fn, axis=1)[:, None]; edge_keys = edges[:, 0]*len(v)+edges[:, 1]
    for ci, chain in enumerate(chains):
        keys = np.minimum(chain[:-1], chain[1:])*len(v)+np.maximum(chain[:-1], chain[1:])
        nn = fn[owners[np.searchsorted(edge_keys, keys)]]; chain_normals.append(nn)
        if boundary_network is not None:
            continue
        t = chain_parameters(v, chain); cached = None if curve_cache is None else curve_cache.get(tuple(map(int, chain)))
        if cached is None:
            cp, kt, error = fit_boundary(v[chain], t, smoothing, nn, normal_budget)
        else:
            cp, kt, error = cached['poles'], cached['knots'], float(cached['movement_mm'])
        cuts = np.unique(np.r_[t, kt]); q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
        curves.append(dict(poles=cp, knots=kt, chain=chain, parameters=t, movement_mm=error)); samples.append(BSpline(kt, cp, 3)(q)); errors.append(error)
        if ci % 500 == 0:
            print(dict(stage='boundary_fit', done=ci+1, total=len(chains)), flush=True)
    if boundary_network is not None:
        curves, samples, errors = boundary_network.build(chains, chain_normals)
    # Check all incident projected wires together before any fitting. Reducing
    # a curve budget always changes its two owners together.
    from shapely.geometry import Polygon
    for attempt in range(14 if boundary_network is not None else 9):
        invalid = set()
        for i, c in enumerate(curves):
            if boundary_network is None:
                continue
            certified = c.get('source_tube_certificate_version') == 1
            limit = float(c.get('source_tube_limit_mm', normal_budget)) if certified else normal_budget
            error = c['source_tube_certified_max_mm'] if certified else c['source_plane_error_mm']
            if c['movement_mm'] > smoothing*(1+1e-6) or error > limit*(1+1e-6):
                invalid.add(i)
        lookup = {}; exact_frames = []; intersection_details = []
        for ci, c in enumerate(curves):
            for a, b in zip(c['chain'][:-1], c['chain'][1:]):
                lookup[tuple(sorted((int(a), int(b))))] = (ci, int(a))
        for di, d in enumerate(domains):
            used = d['vertex_ids']; tri = np.searchsorted(used, f[d['triangle_ids']]); o, ax, lo, size, _, _, _ = stable_frame(v[used], tri)
            polygons, involved = [], set()
            for loop in loops(boundary_edges(tri)):
                refs = []
                for a, b in zip(used[loop], np.roll(used[loop], -1)):
                    ci, av = lookup[tuple(sorted((int(a), int(b))))]; item = (ci, int(a) == av)
                    if not refs or refs[-1] != item:
                        refs.append(item)
                if len(refs) > 1 and refs[0] == refs[-1]:
                    refs.pop()
                xyz = np.concatenate([(samples[ci] if forward else samples[ci][::-1])[:-1] for ci, forward in refs])
                polygon = np.round((xyz-o)@ax[:2].T, 12)
                from .boundary_checks import preserves_winding
                if not preserves_winding((v[used[loop]]-o)@ax[:2].T,polygon):
                    invalid.update(ci for ci,_ in refs)
                    intersection_details.append(dict(kind='reversed_loop',domain=list(map(int,d['triangle_ids'][:1])),
                                                     curves=[ci for ci,_ in refs]))
                polygons.append(polygon); involved.update(ci for ci, _ in refs)
            polygons.sort(key=lambda p: -abs(np.sum(p[:, 0]*np.roll(p[:, 1], -1)-p[:, 1]*np.roll(p[:, 0], -1))))
            if not Polygon(polygons[0], polygons[1:]).is_valid:
                invalid.update(involved)
            exact_frames.append((tuple(map(int, d['triangle_ids'])), involved,
                                 dict(origin=o, axes=ax, lo=lo, size=size)))
        if not invalid and boundary_network is not None:
            from .boundary_checks import intersections
            from .compact import curve as native_curve
            native_curves = [native_curve(c['poles'], c['knots']) for c in curves]
            for domain_key, involved, check_frame in exact_frames:
                signature = {tuple(map(int, curves[i]['chain'])): (curves[i]['poles'], curves[i]['knots']) for i in involved}
                old = boundary_network.exact_cache.get(domain_key)
                if (old is not None and old.keys() == signature.keys() and all(
                        np.array_equal(old[k][0], signature[k][0]) and np.array_equal(old[k][1], signature[k][1]) for k in old)):
                    continue
                details = []
                bad = intersections(curves, involved, check_frame, native_curves, details)
                intersection_details.extend(dict(domain=list(domain_key[:1]), frame={k: x.tolist() for k, x in check_frame.items()},
                                                 **item) for item in details)
                invalid.update(bad)
                if not bad:
                    boundary_network.exact_cache[domain_key] = signature
            if invalid:
                print(dict(stage='continuous_boundary_intersection', affected_curves=sorted(invalid)), flush=True)
        if not invalid:
            if curve_cache is not None:
                curve_cache.update({tuple(map(int, c['chain'])): c for c in curves})
            return assigned, curves, pairs, samples, errors
        print(dict(stage='boundary_crossing_guard', attempt=attempt, affected_curves=len(invalid)), flush=True)
        if diagnostics is not None:
            diagnostic = Path(diagnostics); diagnostic.mkdir(exist_ok=True)
            dump(diagnostic/'intersections.json', dict(attempt=attempt, curves=sorted(invalid), details=intersection_details))
            for ci in invalid:
                np.savez_compressed(diagnostic/f'{ci:05d}.npz', **curves[ci])
        if boundary_network is not None:
            boundary_network.tighten([curves[ci] for ci in invalid])
            curves, samples, errors = boundary_network.build(chains, chain_normals)
            continue
        for ci in invalid:
            chain = chains[ci]; t = chain_parameters(v, chain)
            cp, kt, error = fit_boundary(v[chain], t, smoothing*2**(-attempt-1), chain_normals[ci], normal_budget)
            q = np.unique(np.r_[t, np.linspace(0, 1, max(33, len(kt)*4))])
            curves[ci] = dict(poles=cp, knots=kt, chain=chain, parameters=t, movement_mm=error)
            samples[ci] = BSpline(kt, cp, 3)(q); errors[ci] = error
    raise ValueError('Shared skeleton has a projected crossing')


def fit_support(task, *, max_iterations=32, solver_seconds=12., solver_retry=True, seconds=None,
                acceptance_distance=None, correction_seconds=None, retain_failed=False, quick_penalty=False,
                normal_rms_goal=None):
    if max_iterations < 1 or not np.isfinite(solver_seconds) or solver_seconds <= 0:
        raise ValueError('Invalid support fitting budget')
    if seconds is not None and (not np.isfinite(seconds) or seconds <= 0):
        raise ValueError('Invalid support wall-clock budget')
    if normal_rms_goal is not None and (not np.isfinite(normal_rms_goal) or normal_rms_goal <= 0):
        raise ValueError('Invalid normal-quality objective')
    started = time.perf_counter()
    di, v, f, normals, boundary, curves, config, *options = task
    physical_distance = config.distance if acceptance_distance is None else acceptance_distance
    if not np.isfinite(physical_distance) or physical_distance < config.distance:
        raise ValueError('Acceptance distance must contain the fitting reserve')
    if correction_seconds is not None and (not np.isfinite(correction_seconds) or correction_seconds <= 0):
        raise ValueError('Invalid short correction budget')
    support_solver = options[0] if options else 'least-squares'
    boundary_normals = options[1] if len(options) > 1 else None
    seed = options[2] if len(options) > 2 else None
    preserve_frame = bool(options[3]) if len(options) > 3 else False
    if support_solver != 'bounded' and (config.area_weighted or config.normal_angle_limit is not None or config.fairness_order != 2):
        raise ValueError('Area weights, normal limits and higher-order fairness require the bounded support solver')
    o, ax, lo, size, uv, heights, dot = stable_frame(v, f)
    if preserve_frame:
        if seed is None:
            raise ValueError('A preserved support frame requires a native reference record')
        o, ax = seed['origin'].copy(), seed['axes'].copy()
        fn = np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        fn /= np.linalg.norm(fn,axis=1)[:,None]
        dot = fn@ax[2]
    if dot.min() <= .05:
        return di, None, dict(accepted=False, reason='non_graph', history=[])
    # Extents include the smoothed boundary, not just the old staircase.
    all_xy = np.r_[(v-o)@ax[:2].T, (boundary-o)@ax[:2].T]
    lo = all_xy.min(axis=0); size = np.ptp(all_xy, axis=0)
    margin = config.boundary/size
    lo -= margin*size; size *= 1+2*margin
    if preserve_frame and np.all(lo >= seed['lo']) and np.all(lo+size <= seed['lo']+seed['size']):
        lo, size = seed['lo'].copy(), seed['size'].copy()
    points = np.r_[v, v[f].mean(axis=1)]
    q = ((points-o)@ax[:2].T-lo)/size; z = (points-o)@ax[2]
    bq = ((boundary-o)@ax[:2].T-lo)/size; bz = (boundary-o)@ax[2]
    boundary_normal_q = bq.copy()
    boundary_n = None if boundary_normals is None else boundary_normals@ax.T
    normal_points = v if normals.ndim == 2 else v[f].reshape(-1, 3)
    normal_q = ((normal_points-o)@ax[:2].T-lo)/size
    n = normals.reshape(-1, 3)@ax.T; good = n[:, 2] > .15
    grad = -n[good, :2]/n[good, 2, None]*size
    position_weights = normal_weights = None
    if config.area_weighted:
        area2 = np.linalg.norm(np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]]), axis=1)
        vertex_area = np.bincount(f.ravel(), weights=np.repeat(area2/3, 3), minlength=len(v))
        # Half of each triangle's mass at its centroid and half at its corners.
        # Refinement probes remain hard constraints without changing the target measure.
        position_weights = np.r_[vertex_area/2, area2/2]
        normal_weights = vertex_area if normals.ndim == 2 else np.repeat(area2/3, 3)
    # STEP import may choose either incident pcurve. Reserve a two-sided gap
    # budget instead of allowing each support almost the whole edge tolerance.
    seam_limit = config.boundary*.4
    t = knots(9); history = []; boundary_enrichments = 0; interior_enrichments = 0; probe_cache = {}
    # A moved trim usually needs the resolution already established for this
    # source domain. Reuse that adaptive basis as the initial trial instead of
    # repeatedly rediscovering it from nine controls. No old coefficients or
    # feasibility claims are copied; the complete fitter gates still run.
    if seed is not None and 9 <= len(seed['knots'])-4 <= max(config.controls):
        t = np.array(seed['knots'], copy=True)
    bending_cache = {}; polygon = None; normal_enrichments = 0; normal_probe_cache = {}; uniform_retry = False
    if config.fairness_domain == 'source':
        if support_solver != 'bounded':
            raise ValueError('Source-domain fairness requires the bounded support solver')
        if config.fairness_order != 2:
            raise ValueError('Source-domain fairness currently supports second derivatives only')
        from .trimmed_fairness import source_polygon
        polygon = source_polygon(q[:len(v)], f)
    elif config.fairness_domain != 'rectangle':
        raise ValueError('Unknown fairness domain')
    best = None; best_score = float('inf'); source_locator = None
    quality_reference = None; quality_event = None; quality_started = None
    physical_mode = acceptance_distance is not None
    source_normal = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    source_normal /= np.linalg.norm(source_normal, axis=1)[:, None]

    def nearest_source(candidate, points):
        nonlocal source_locator
        from .mesh import Locator
        if source_locator is None:
            source_locator = Locator(v, f)
        return source_locator.closest(positions(points, candidate))

    def surface_quality(candidate):
        from .surface_jets import jets
        predicted, shape = jets(candidate, v[f].mean(axis=1))
        target = (normals[f] if normals.ndim == 2 else normals).mean(axis=1)
        target /= np.linalg.norm(target, axis=1)[:, None]
        area = np.linalg.norm(np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]]), axis=1)
        angle = np.degrees(np.arccos(np.clip(np.einsum('ij,ij->i', predicted, target), -1., 1.)))
        return dict(normal_rms_degrees=float(np.sqrt(np.dot(area, angle**2)/area.sum())),
                    curvature_rms_per_mm=float(np.sqrt(np.dot(area, np.sum(shape**2, axis=(1, 2)))/area.sum())))

    def finish(reason):
        """A single short repair, never recursive fitting or knot insertion."""
        if quality_reference is not None:
            # This is a quality proposal after an already validated fit, not
            # a reason to split or start a second repair. Retain the known
            # feasible geometry when the one richer solve is not useful.
            quality_event.update(accepted=False, reason=reason, seconds=time.perf_counter()-quality_started)
            return di, quality_reference, dict(accepted=True, history=history, quality_proposal=quality_event)
        info = dict(accepted=False, reason=reason, history=history)
        if retain_failed and best is not None:
            info['rejected_candidate'] = dict(best)
        if correction_seconds is None or best is None or config.normal_angle_limit is not None:
            return di, None, info
        candidate = dict(best); tick = time.perf_counter(); kt = candidate['knots']
        bb = tensor(bq, kt, kt)
        de = float(nearest_source(candidate, q)[1].max(initial=0.))
        be = float(abs(bb@candidate['height']-bz).max(initial=0.))
        minor = de <= physical_distance*1.2 and be <= seam_limit*1.2
        info['failure_class'] = 'minor_residual' if minor else 'unresolved_support'
        if not minor:
            return di, None, info
        from .support_probe import probes, VERSION
        iq, iz = probes(candidate, v, f)
        extrema = [spline_graph_error(c['poles'], c['knots'], candidate, return_parameter=True) for c in curves]
        extra = np.array([BSpline(c['knots'], c['poles'], 3)(where)
                          for c, (_, where) in zip(curves, extrema)]).reshape(-1, 3)
        eq = ((extra-o)@ax[:2].T-lo)/size; ez = (extra-o)@ax[2]
        cq = np.r_[q, iq]
        cbq, cbz = np.r_[bq, eq], np.r_[bz, ez]
        ca, cb = tensor(cq, kt, kt), tensor(cbq, kt, kt)
        def check_proposal():
            de = float(nearest_source(candidate, cq)[1].max(initial=0.))
            boundary_error = max((spline_graph_error(c['poles'], c['knots'], candidate) for c in curves), default=0.)
            return de, boundary_error
        de, boundary_error = check_proposal()
        original = dict(candidate); original_boundary = boundary_error
        repair = dict(method='physical_tolerance_recheck', attempts=0, unchanged_basis=True)
        if de >= physical_distance or boundary_error >= seam_limit:
            from .bounded_support import correct
            from .surface_jets import support_span_parameters
            # Align constraints to this support's knot crossings before the
            # only correction; original curve samples miss narrow spans.
            dense = np.concatenate([BSpline(c['knots'], c['poles'], 3)(support_span_parameters(
                dict(c, parameters=c.get('parameters', np.array([0., 1.]))), [candidate], quadrature=True))
                for c in curves]) if curves else np.empty((0, 3))
            dbq = ((dense-o)@ax[:2].T-lo)/size; dbz = (dense-o)@ax[2]
            cb = tensor(np.r_[cbq, dbq], kt, kt); cbz = np.r_[cbz, dbz]
            near, _, ids, _ = nearest_source(candidate, cq)
            nn = source_normal[ids]; cosine = nn@ax[2]
            signed = np.einsum('ij,ij->i', positions(cq, candidate)-near, nn)
            target = ca@candidate['height']-signed/cosine
            corrected, repair = correct(candidate['height'], ca, target, cb, cbz, physical_distance, seam_limit,
                seconds=correction_seconds,
                data_limits=physical_distance/cosine)
            repair['method'] = 'single_coefficient_projection'
            if corrected is not None:
                candidate['height'] = corrected; de, boundary_error = check_proposal()
                if de >= physical_distance or boundary_error >= seam_limit:
                    from .distance_projection import damp
                    clipped, clipping = damp(original, candidate, cq, v, f, physical_distance,
                                             original_boundary, boundary_error, seam_limit, source_locator)
                    repair['coefficient_step'] = clipping
                    if clipped is not None:
                        candidate = clipped; de, boundary_error = check_proposal()
        passed = de < physical_distance and boundary_error < seam_limit
        repair.update(seconds=time.perf_counter()-tick, distance_max_mm=de,
                      continuous_boundary_max_mm=boundary_error, accepted=passed)
        info['short_repair'] = repair
        if not passed:
            info['failure_class'] = ('minor_residual' if de <= physical_distance*1.2 and boundary_error <= seam_limit*1.2
                                     else 'geometric_excursion')
            return di, None, info
        candidate.update(continuous_boundary_error_mm=boundary_error, interior_probe_version=VERSION,
                         interior_probe_count=len(iq), interior_probe_max_mm=float(nearest_source(candidate, iq)[1].max(initial=0.)),
                         interior_probe_metric='finite_source_euclidean', sampled_source_max_mm=de,
                         distance_reserve_met=de < config.distance, short_repair=True)
        info.pop('rejected_candidate', None)
        return di, candidate, dict(info, accepted=True, reason='short_repair_validated')

    for iteration in range(max_iterations):
        if seconds is not None and time.perf_counter()-started >= seconds:
            return finish('support_time_budget')
        if quality_reference is not None:
            quality_event['attempts'] = 1
        count = len(t)-4; a, b = tensor(q, t, t), tensor(bq, t, t)
        uq = np.unique(t); solver = None
        if support_solver == 'bounded':
            from .bounded_support import solve, FAIRNESS_VERSION
            if polygon is not None and tuple(t) not in bending_cache:
                from .trimmed_fairness import bending_matrix
                bending_cache[tuple(t)] = bending_matrix(polygon, t, size)
            coeff, solver = solve(a, z, b, bz, normal_q, n, t, size, config, seam_limit,
                                  normal_limit=config.normal_angle_limit,
                                  boundary_q=boundary_normal_q, boundary_normals=boundary_n,
                                  bending=bending_cache.get(tuple(t)),
                                  position_weights=position_weights, normal_weights=normal_weights,
                                  solver_seconds=solver_seconds, equilibrated_retry=solver_retry,
                                  retain_inexact=physical_mode, quick_penalty=quick_penalty)
        elif support_solver == 'least-squares':
            blocks = [a/np.sqrt(len(q)), b*np.sqrt(3000/len(bq))]; rhs = [z/np.sqrt(len(q)), bz*np.sqrt(3000/len(bq))]
            for axis in range(2):
                weight = np.sqrt(config.normal_weight/max(1, good.sum()))*min(size)/size[axis]
                op = tensor(normal_q[good], t, t, int(axis == 0), int(axis == 1))
                blocks.append(op*weight); rhs.append(grad[:, axis]*weight)
            g = np.unique(np.r_[(uq[:-1]+uq[1:])/2, np.linspace(0, 1, count)])
            fq = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
            for du, dv in [(2, 0), (1, 1), (0, 2)]:
                weight = np.sqrt(config.fairness/len(fq))/(size[0]**du*size[1]**dv)
                blocks.append(tensor(fq, t, t, du, dv)*weight); rhs.append(np.zeros(len(fq)))
            matrix = sparse.vstack(blocks, format='csr'); target = np.concatenate(rhs)
            h = (matrix.T@matrix).tocsc(); reg = max(1., h.diagonal().max())*1e-14
            coeff = spsolve(h+sparse.eye(count*count, format='csc')*reg, matrix.T@target)
        else:
            raise ValueError('Unknown support solver')
        error = abs(a@coeff-z); be = abs(b@coeff-bz)
        normal_error = None
        if config.normal_angle_limit is not None:
            gradient = np.column_stack([tensor(normal_q[good], t, t, int(axis == 0), int(axis == 1))@coeff/size[axis]
                                        for axis in range(2)])
            predicted = np.c_[-gradient, np.ones(len(gradient))]
            predicted /= np.linalg.norm(predicted, axis=1)[:, None]
            normal_error = np.degrees(np.arccos(np.clip(np.sum(predicted*n[good], axis=1), -1, 1)))
        rec = dict(origin=o, axes=ax, lo=lo, size=size, knots=t, height=coeff)
        if support_solver == 'bounded':
            rec['fairness_version'] = FAIRNESS_VERSION
        metric_error = (nearest_source(rec, q)[1] if np.isfinite(coeff).all() else np.full(len(q), np.inf)) if physical_mode else error
        score = max(float(metric_error.max())/physical_distance, float(be.max())/seam_limit)
        previous_best, previous_score = best, best_score
        if np.isfinite(coeff).all() and score < best_score:
            best, best_score = dict(rec), score
        item = dict(controls=count, distance_max=float(error.max()), boundary_max=float(be.max()),
                    basis_strategy='uniform_retry' if uniform_retry else 'adaptive'); history.append(item)
        if seed is not None:
            item['initial_basis_controls'] = len(seed['knots'])-4
            item['native_reference_frame'] = preserve_frame
        if normal_error is not None:
            item['sampled_normal_max_degrees'] = float(normal_error.max(initial=0.))
        if solver is not None:
            item['solver'] = solver
        if physical_mode:
            item['finite_source_sample_max_mm'] = float(metric_error.max())
        normal_pass = normal_error is None or normal_error.max(initial=0.) <= config.normal_angle_limit*(1+1e-5)
        if (physical_mode or solver is None or solver['solved']) and normal_pass and metric_error.max() < physical_distance and be.max() < seam_limit*(1. if physical_mode else .8):
            extrema = [spline_graph_error(c['poles'], c['knots'], rec, return_parameter=True) for c in curves]
            maximum = max(x[0] for x in extrema)
            item['continuous_boundary_max'] = maximum
            score = max(score, maximum/seam_limit)
            best, best_score = (dict(rec), score) if score < previous_score else (previous_best, previous_score)
            if maximum < seam_limit:
                if support_solver == 'bounded':
                    from .support_probe import probes, VERSION
                    key = tuple(t)
                    if key not in probe_cache:
                        probe_cache[key] = probes(rec, v, f)
                    iq, iz = probe_cache[key]
                    ie = nearest_source(rec, iq)[1] if physical_mode else abs(tensor(iq, t, t)@coeff-iz)
                    peak = float(ie.max(initial=0.)); item['interior_probe_max_mm'] = peak
                    score = max(score, peak/physical_distance)
                    best, best_score = (dict(rec), score) if score < previous_score else (previous_best, previous_score)
                    if peak >= physical_distance:
                        if quality_reference is not None:
                            return finish('quality_interior_rejected')
                        # Enrich the actual violated positions before adding
                        # knots or splitting a fixed-label fitting domain.
                        if interior_enrichments < 12:
                            worst = np.argsort(ie)[-64:]; worst = worst[ie[worst] >= physical_distance*.98]
                            q = np.r_[q, iq[worst]]; z = np.r_[z, iz[worst]]
                            if position_weights is not None:
                                position_weights = np.r_[position_weights, np.zeros(len(worst))]
                            interior_enrichments += 1; item['interior_enrichment'] = interior_enrichments
                            continue
                        return finish('interior_excursion')
                    if config.normal_angle_limit is not None:
                        from .support_probe import normal_targets
                        if key not in normal_probe_cache:
                            normal_probe_cache[key] = normal_targets(rec, v, f, normals, iq)
                        target = normal_probe_cache[key]
                        usable = target[:, 2] > .15
                        nq, nn = iq[usable], target[usable]
                        gradient = np.column_stack([tensor(nq, t, t, int(axis == 0), int(axis == 1))@coeff/size[axis]
                                                    for axis in range(2)])
                        predicted = np.c_[-gradient, np.ones(len(gradient))]
                        predicted /= np.linalg.norm(predicted, axis=1)[:, None]
                        angles = np.degrees(np.arccos(np.clip(np.sum(predicted*nn, axis=1), -1, 1)))
                        normal_peak = float(angles.max(initial=0.))
                        item['interior_normal_max_degrees'] = normal_peak
                        if normal_peak > config.normal_angle_limit*(1+1e-5):
                            if quality_reference is not None:
                                return finish('quality_normal_limit_rejected')
                            if normal_enrichments >= 12:
                                return di, None, dict(accepted=False, reason='interior_normal_excursion', history=history)
                            worst = np.argsort(angles)[-64:]
                            worst = worst[angles[worst] > config.normal_angle_limit]
                            if normal_weights is None:
                                normal_weights = np.ones(len(n))
                            normal_q = np.r_[normal_q, nq[worst]]; n = np.r_[n, nn[worst]]
                            normal_weights = np.r_[normal_weights, np.zeros(len(worst))]
                            good = n[:, 2] > .15
                            normal_enrichments += 1
                            item['interior_normal_enrichment'] = normal_enrichments
                            continue
                        rec.update(interior_normal_probe_version=1, interior_normal_probe_count=len(nq),
                                   interior_normal_max_degrees=normal_peak,
                                   sampled_normal_limit_degrees=config.normal_angle_limit)
                    rec.update(interior_probe_version=VERSION, interior_probe_count=len(iq), interior_probe_max_mm=peak)
                    if physical_mode:
                        rec['interior_probe_metric'] = 'finite_source_euclidean'
                    if config.boundary_normal_weight > 0:
                        from .boundary_normals import VERSION as normal_version
                        rec['boundary_normal_target_version'] = normal_version
                rec['continuous_boundary_error_mm'] = maximum
                if physical_mode:
                    rec['distance_reserve_met'] = max(float(metric_error.max()), item.get('interior_probe_max_mm', 0.)) < config.distance
                    rec['sampled_source_max_mm'] = float(metric_error.max())
                if normal_rms_goal is not None:
                    quality = surface_quality(rec); item['surface_quality'] = quality
                    rec.update(fit_normal_rms_degrees=quality['normal_rms_degrees'],
                               fit_curvature_rms_per_mm=quality['curvature_rms_per_mm'])
                    if quality_reference is not None:
                        quality_event['after'] = quality
                        before = quality_event['before']
                        if (quality['normal_rms_degrees'] >= before['normal_rms_degrees']*.999
                                or quality['curvature_rms_per_mm'] > before['curvature_rms_per_mm']*1.05+1e-8):
                            return finish('quality_gain_rejected')
                        quality_event.update(accepted=True, seconds=time.perf_counter()-quality_started)
                        return di, rec, dict(accepted=True, history=history, quality_proposal=quality_event)
                    if quality['normal_rms_degrees'] > normal_rms_goal and iteration+1 < max_iterations:
                        # One nested refinement of the largest spans adds
                        # capacity for smooth normals without a new CAD face.
                        # No quality-driven enrichment loop follows this solve.
                        extra_count = min(8, max(config.controls)-count, len(uq)-1)
                        if extra_count > 0:
                            selected = np.argsort(np.diff(uq), kind='stable')[-extra_count:]
                            quality_reference = dict(rec); quality_started = time.perf_counter()
                            quality_event = dict(attempts=0, before=quality, normal_rms_goal_degrees=normal_rms_goal,
                                controls_before=count, controls_after=count+extra_count, new_CAD_faces=0)
                            t = np.sort(np.r_[t, (uq[selected]+uq[selected+1])/2])
                            continue
                return di, rec, dict(accepted=True, history=history)
            if quality_reference is not None:
                return finish('quality_boundary_rejected')
            if boundary_enrichments < 12:
                # An extremum can sit between every original boundary sample,
                # especially on an adaptively knotted surface. Add that actual
                # point to the same domain before spending another CAD face.
                extra = np.array([BSpline(c['knots'], c['poles'], 3)(where)
                                  for c, (value, where) in zip(curves, extrema) if value >= seam_limit])
                eq = ((extra-o)@ax[:2].T-lo)/size; ez = (extra-o)@ax[2]
                actual = abs(tensor(eq, t, t)@coeff-ez)
                item['direct_peak_residual'] = float(actual.max())
                bq = np.r_[bq, np.repeat(eq, 16, axis=0)]; bz = np.r_[bz, np.repeat(ez, 16)]
                boundary_enrichments += 1; item['boundary_enrichment'] = boundary_enrichments
                continue
        if quality_reference is not None:
            return finish('quality_sample_rejected')
        if count >= max(config.controls):
            # Residual-driven refinement can cluster knots into tiny spans.
            # Retry the same coefficient budget with a uniform C2 basis before
            # rejecting the chart; all enriched geometric constraints remain.
            if support_solver == 'bounded' and not uniform_retry and not np.array_equal(t, knots(count)):
                t = knots(count); uniform_retry = True
                continue
            break
        # Refine where current residuals are worst; keep a simple-knot C2 basis.
        all_q = np.r_[q, bq]; ratio = np.r_[metric_error/physical_distance, be/(seam_limit*.8)]
        if normal_error is not None:
            all_q = np.r_[all_q, normal_q[good]]
            ratio = np.r_[ratio, normal_error/config.normal_angle_limit]
        order = np.argsort(ratio)[::-1]; new = []
        for k in order[:min(128, len(order))]:
            for value in all_q[k]:
                span = int(np.clip(np.searchsorted(uq, value)-1, 0, len(uq)-2))
                midpoint = float((uq[span]+uq[span+1])/2)
                if uq[span+1]-uq[span] > 1e-4 and midpoint not in new:
                    new.append(midpoint)
                if len(new) >= min(6, max(config.controls)-count):
                    break
            if len(new) >= min(6, max(config.controls)-count):
                break
        if not new:
            break
        t = np.sort(np.r_[t, new])
    return finish('fitting_budget')


def reconstruct(partition, stl, output, smoothing=.04, workers=4, regions=None, rounds=4,
                face_limit=None, config=None, from_fit=None, boundary_mode='chain',
                normal_field='vertex', normal_filter_steps=0, boundary_cache=None,
                support_solver='least-squares', refit_supports=False, boundary_plane_budget=None, repair_charts=(),
                refit_regions=None, master_config=None, fitting_fraction=1.,
                fit_iterations=32, solver_seconds=12., fit_seconds=None, round_seconds=None,
                correction_seconds=None, split_minor_residuals=True, solver_retry=True,
                minimum_graph_cosine=.08, quick_penalty=False, normal_rms_goal=None):
    if not 0 < fitting_fraction <= 1:
        raise ValueError('Invalid fitting distance reserve')
    if not np.isfinite(minimum_graph_cosine) or not 0 < minimum_graph_cosine < 1:
        raise ValueError('Invalid graph conditioning bound')
    if master_config is not None and (from_fit is not None or boundary_cache is not None or regions is not None):
        raise ValueError('Automatic master planning starts from STL and labels, without prior candidates')
    out = Path(output); out.mkdir(parents=True, exist_ok=False); start = time.perf_counter()
    v, f, labels, topo = load_partition(partition, stl); normal = vertex_normals(v, f)
    selected_refit = set(refit_regions or ())
    if selected_refit and (from_fit is None or regions is not None or not selected_refit.issubset(set(labels))):
        raise ValueError('Selective whole-model refitting requires a prior fit and existing source regions')
    fit_cache, curve_cache = {}, {}
    def stamp(di, curves, pairs):
        return {tuple(map(int, c['chain'])): (c['poles'], c['knots'])
                for c, pair in zip(curves, pairs) if di in pair}
    def remember(rec, signatures):
        fit_cache[tuple(sorted(map(int, rec['triangle_ids'])))] = (rec, signatures)
    if from_fit is None:
        domains, graph, edges, owners = initial_domains(v, f, labels, minimum_cosine=minimum_graph_cosine,
            conditioning_distance=config.distance*fitting_fraction if config is not None else None)
    else:
        source = Path(from_fit); previous = json.loads((source/'run.json').read_text(encoding='utf-8'))
        if previous.get('network_fairing') or previous.get('network_guard') or previous.get('curve_route_reconstruction'):
            raise ValueError('Movable-junction caches require network_support for refitting or graph_reassemble for exact replay; rebuilding the source-edge skeleton would reset their junctions')
        required_refit = set(previous.get('required_refit_regions', ()))
        if previous.get('requires_support_refit') and not (refit_supports or (required_refit and required_refit.issubset(selected_refit))):
            raise ValueError('Changed source-surface boundaries require explicit support refitting')
        if Path(previous['partition']).resolve() != Path(partition).resolve() or Path(previous['stl']).resolve() != Path(stl).resolve():
            raise ValueError('Resume fit uses different declared input files')
        old_config = Config(**previous['config'])
        old_plane_budget = previous.get('source_plane_budget_mm', old_config.distance*.1)
        if boundary_plane_budget is None:
            boundary_plane_budget = old_plane_budget
        elif boundary_plane_budget != old_plane_budget:
            raise ValueError('Resume caching requires the same source-plane movement budget')
        checked = ('distance', 'boundary') if refit_supports else ('distance', 'boundary', 'normal_weight', 'fairness', 'boundary_normal_weight', 'normal_angle_limit', 'fairness_domain', 'area_weighted', 'fairness_order')
        if config is not None and any(getattr(config, k) != getattr(old_config, k) for k in checked):
            raise ValueError('Resume caching requires unchanged fitting tolerances and weights')
        if smoothing != previous['smoothing_mm']:
            raise ValueError('Resume caching requires the same smoothing movement limit')
        for key, value, default in [('boundary_mode', boundary_mode, 'chain'),
                                    ('normal_field', normal_field, 'vertex'),
                                    ('normal_filter_steps', normal_filter_steps, 0),
                                    ('support_solver', support_solver, 'least-squares')]:
            if previous.get(key, default) != value:
                raise ValueError('Resume caching requires unchanged boundary and normal algorithms')
        config = old_config if config is None else config
        assignment = np.load(source/'chart_index.npz')['face_chart']
        old_curves = [dict(np.load(p)) for p in sorted((source/'curves').glob('*.npz'))]
        for curve in old_curves:
            if curve.get('source_tube_certificate_version') == 1:
                limit = float(curve.get('source_tube_limit_mm',old_plane_budget))
                if not np.isfinite(limit) or not 0 < limit < config.distance:
                    raise ValueError('Cached source tube exceeds the declared fitting-distance budget')
        old_pairs = [c['owners'] for c in old_curves]
        curve_cache = {tuple(map(int, c['chain'])): c for c in old_curves
                       if np.max(np.unique(c['knots'][4:-4], return_counts=True)[1], initial=0) < 3}
        old_rows = {r['chart']: r for r in json.loads((source/'fitting.json').read_text(encoding='utf-8'))}
        pending_checks = []; pending_records = {}
        for path in sorted((source/'charts').glob('*.npz')):
            rec = dict(np.load(path)); history = old_rows[int(path.stem)]['history']
            if support_solver == 'bounded':
                from .bounded_support import FAIRNESS_VERSION
                if int(rec.get('fairness_version', 0)) != FAIRNESS_VERSION:
                    continue
                if config.normal_angle_limit is not None and (
                    int(rec.get('interior_normal_probe_version',0)) != 1 or
                    float(rec.get('interior_normal_max_degrees',float('inf'))) > config.normal_angle_limit*(1+1e-5)):
                    continue
            measured = rec.get('continuous_boundary_error_mm', history[-1].get('continuous_boundary_max', float('inf')) if history else float('inf'))
            if measured < config.boundary*.4 and not refit_supports and int(rec['region']) not in selected_refit:
                if config.boundary_normal_weight > 0:
                    from .boundary_normals import VERSION as normal_version
                    if int(rec.get('boundary_normal_target_version', 0)) != normal_version:
                        continue
                rec['continuous_boundary_error_mm'] = float(measured)
                if support_solver == 'bounded' and int(rec.get('interior_probe_version', 0)) != 1:
                    di = int(path.stem); used = rec['vertex_ids']; pending_records[di] = rec
                    pending_checks.append((di, rec, v[used], np.searchsorted(used, f[rec['triangle_ids']])))
                else:
                    remember(rec, stamp(int(path.stem), old_curves, old_pairs))
        if pending_checks:
            from .support_probe import check
            rejected = []
            with ProcessPoolExecutor(max_workers=workers) as pool:
                for di, info in pool.map(check, pending_checks):
                    rec = pending_records[di]; rec.update(info)
                    if info['interior_probe_max_mm'] < config.distance:
                        remember(rec, stamp(di, old_curves, old_pairs))
                    else:
                        rejected.append(dict(chart=di, **info))
            dump(out/'resume_interior_rejections.json', rejected)
            print(dict(stage='resume_interior_validation', checked=len(pending_checks), rejected=len(rejected)), flush=True)
        edges, owners = incidence(f)
        graph = sparse.coo_matrix((np.ones(owners.size), (owners.ravel(), owners[:, ::-1].ravel())), shape=(len(f), len(f))).tocsr()
        seeds = []
        for di in np.unique(assignment):
            ids = np.flatnonzero(assignment == di); region = np.unique(labels[ids])
            if len(region) != 1:
                raise ValueError('Resume chart crosses supplied labels')
            parts = split_faces(v, f, ids, graph) if di in previous['failed_charts'] and len(ids) > 1 else [ids]
            seeds.extend((int(region[0]), part) for part in parts)
        domains = regular_domains(v, f, graph, seeds, minimum_graph_cosine,
                                  config.distance*fitting_fraction if config is not None else None)
    config = config or Config(distance=.03, boundary=.005, controls=(65,), normal_weight=.0001, fairness=1e-8)
    plane_budget = config.distance*.1 if boundary_plane_budget is None else boundary_plane_budget
    repair_charts = set(map(int, repair_charts))
    if repair_charts and (from_fit is None or boundary_mode != 'fair-chains'
                          or min(repair_charts) < 0 or max(repair_charts) >= len(domains)):
        raise ValueError('Boundary repair requires valid source-fit chart IDs and fair-chains mode')
    requested_repairs = sorted(repair_charts)
    if repair_charts:
        if previous['failed_charts']:
            raise ValueError('Finish pending domain refinements before repairing cached chart boundaries')
        from .feature_boundary import coupled_repair_charts
        repair_charts = set(coupled_repair_charts(old_curves, repair_charts))
    if not np.isfinite(plane_budget) or not 0 < plane_budget < config.distance:
        raise ValueError('Source-plane budget must be positive and below the source-distance limit')
    if normal_field == 'crease-fans':
        from .source_normals import crease_fan_normals
        normal = crease_fan_normals(v, f, config.feature_angle, (edges, owners), normal_filter_steps)
    elif normal_field != 'vertex':
        raise ValueError('Unknown normal field')
    metadata = dict(status='running', algorithm='shared-skeleton-regional-graph-v2', partition=str(Path(partition).resolve()),
                    stl=str(Path(stl).resolve()), source_CAD_read=False, commercial_engine_used=False,
                    config=asdict(config), smoothing_mm=smoothing, workers=workers, input_topology=topo,
                    face_limit=face_limit, resumed_from=None if from_fit is None else str(Path(from_fit).resolve()),
                    support_boundary_limit_mm=config.boundary*.4,
                    boundary_weight=3000. if support_solver == 'least-squares' else None,
                    sampled_boundary_limit_mm=config.boundary*.4*.65 if support_solver == 'bounded' else None,
                    boundary_mode=boundary_mode, normal_field=normal_field, normal_filter_steps=normal_filter_steps,
                    support_solver=support_solver, refit_supports=refit_supports, source_plane_budget_mm=plane_budget,
                    boundary_repair_charts=sorted(repair_charts))
    metadata['refit_regions'] = sorted(selected_refit)
    metadata['requested_boundary_repair_charts'] = requested_repairs
    metadata['cold_start'] = from_fit is None and boundary_cache is None
    metadata['master_planning_before_fitting'] = master_config is not None
    metadata['fitting_distance_mm'] = config.distance*fitting_fraction
    metadata['minimum_graph_cosine'] = minimum_graph_cosine
    metadata['conditioning_plane_distance_mm'] = config.distance*fitting_fraction
    metadata['quick_penalty_proposal'] = quick_penalty
    metadata['normal_rms_goal_degrees'] = normal_rms_goal
    metadata['fitting_budget'] = dict(iterations=fit_iterations, solver_seconds=solver_seconds,
                                    support_seconds=fit_seconds, round_seconds=round_seconds, layout_rounds=rounds,
                                    correction_seconds=correction_seconds, split_minor_residuals=split_minor_residuals,
                                    solver_retry=solver_retry)
    if config.boundary_normal_weight > 0:
        from .boundary_normals import VERSION as normal_version
        metadata['boundary_normal_target_mode'] = f'nearest-source-smooth-fans-v{normal_version}'
    dump(out/'run.json', metadata)
    boundary_network = None
    if boundary_mode in ('fair-network', 'fair-chains'):
        from .feature_boundary import FeatureBoundaryNetwork
        boundary_network = FeatureBoundaryNetwork(v, f, labels, edges, owners, smoothing, plane_budget,
            workers=workers, cache=boundary_cache or out/'complete_interfaces', complete=boundary_mode == 'fair-network')
        boundary_network.seed(curve_cache.values())
        for c in curve_cache.values():
            if repair_charts.intersection(map(int, c['owners'])):
                key = tuple(map(int, c['chain']))
                boundary_network.force_positive.add(key); boundary_network.tangent_locked.add(key)
                boundary_network.internal_cache.pop(key, None)
    elif boundary_mode != 'chain':
        raise ValueError('Unknown boundary mode')
    for iteration in range(rounds):
        if face_limit is not None and len(domains) > face_limit:
            raise ValueError('Representation exceeded the supplied face budget')
        stage = out/f'round_{iteration}'; stage.mkdir(); (stage/'charts').mkdir(); (stage/'curves').mkdir()
        assignment, curves, pairs, boundary_samples, errors = skeleton(v, f, domains, edges, owners, smoothing, plane_budget,
            curve_cache, boundary_network, stage/'boundary_diagnostics')
        if master_config is not None:
            from .global_network import plan_before_fit
            curves, pairs, boundary_samples, planned = plan_before_fit(
                stage, metadata, domains, curves, pairs, assignment, labels, master_config)
            metadata['master_network'] = planned
        print(dict(stage='network', iteration=iteration, domains=len(domains), curves=len(curves)), flush=True)
        normal_targets = None
        if config.boundary_normal_weight > 0:
            from .boundary_normals import targets
            normal_targets = targets(v, f, normal, assignment, edges, owners, curves, pairs)
        tasks = []; signatures = {}; records = []; reused = 0
        for di, d in enumerate(domains):
            if regions is not None and d['region'] not in regions:
                continue
            selected = np.flatnonzero(np.any(np.asarray(pairs) == di, axis=1)); used = d['vertex_ids']
            signatures[di] = stamp(di, curves, pairs)
            cached = fit_cache.get(tuple(sorted(map(int, d['triangle_ids']))))
            if cached is not None:
                rec, old = cached; current = signatures[di]
                if old.keys() == current.keys() and all(np.array_equal(old[k][0], current[k][0]) and np.array_equal(old[k][1], current[k][1]) for k in current):
                    np.savez_compressed(stage/'charts'/f'{di:05d}.npz', **rec)
                    records.append(dict(chart=di, region=d['region'], accepted=True, reused=True, history=[])); reused += 1
                    continue
            target_normals = normal[used] if normal.ndim == 2 else normal[d['triangle_ids']]
            boundary_normals = None if normal_targets is None else np.concatenate([
                normal_targets[i][list(pairs[i]).index(di)] for i in selected])
            tasks.append((di, v[used], np.searchsorted(used, f[d['triangle_ids']]), target_normals,
                          np.concatenate([boundary_samples[i] for i in selected]), [curves[i] for i in selected],
                          replace(config, distance=config.distance*fitting_fraction), support_solver, boundary_normals))
        failed = []
        print(dict(stage='cache', reused=reused, refit=len(tasks)), flush=True)
        from functools import partial
        fitter = partial(fit_support, max_iterations=fit_iterations, solver_seconds=solver_seconds, seconds=fit_seconds,
                         solver_retry=solver_retry, correction_seconds=correction_seconds,
                         acceptance_distance=config.distance if correction_seconds is not None else None,
                         retain_failed=correction_seconds is not None, quick_penalty=quick_penalty,
                         normal_rms_goal=normal_rms_goal)
        # A round deadline terminates unfinished workers instead of waiting for
        # an increasingly expensive local problem during executor shutdown.
        import multiprocessing as mp
        pool = mp.Pool(workers); round_start = time.perf_counter(); completed = set()
        try:
            results = pool.imap_unordered(fitter, tasks, chunksize=1)
            for index in range(len(tasks)):
                remaining = None if round_seconds is None else round_seconds-(time.perf_counter()-round_start)
                if remaining is not None and remaining <= 0:
                    break
                try:
                    di, rec, info = results.next(timeout=remaining)
                except mp.TimeoutError:
                    break
                completed.add(di)
                candidate = info.pop('rejected_candidate', None)
                if candidate is not None:
                    rejected = stage/'rejected_charts'; rejected.mkdir(exist_ok=True)
                    np.savez_compressed(rejected/f'{di:05d}.npz', **dict(candidate, **domains[di]))
                d = domains[di]; records.append(dict(chart=di, region=d['region'], **info))
                if rec is None:
                    failed.append(di)
                else:
                    rec.update(**d); np.savez_compressed(stage/'charts'/f'{di:05d}.npz', **rec)
                    remember(rec, signatures[di])
                if index % 50 == 0:
                    print(dict(stage='fit', done=index+1, total=len(tasks), failed=len(failed)), flush=True)
                dump(stage/'fit_progress.json', dict(completed=len(completed), total=len(tasks), failed=failed,
                                                    seconds=time.perf_counter()-round_start))
        finally:
            pool.terminate(); pool.join()
        for task in tasks:
            di = task[0]
            if di not in completed:
                failed.append(di)
                records.append(dict(chart=di, region=domains[di]['region'], accepted=False,
                                    reason='round_time_budget', history=[]))
        records.sort(key=lambda row: row['chart'])
        dump(stage/'fitting.json', records)
        for ci, (curve, pair) in enumerate(zip(curves, pairs)):
            np.savez_compressed(stage/'curves'/f'{ci:05d}.npz', **dict(curve, owners=pair))
        np.savez_compressed(stage/'chart_index.npz', face_chart=assignment, source_labels=labels)
        metadata.update(source_fit=str(stage.resolve()), fitted_charts=len(domains), failed_charts=failed,
                        partial_regions=regions, boundary_movement_mm=stats(errors), seconds=time.perf_counter()-start,
                        reused_charts=reused, refitted_charts=len(tasks), complete_source_coverage=regions is None and not failed,
                        partition_labels_unchanged=all(np.all(labels[d['triangle_ids']] == d['region']) for d in domains))
        metadata['source_surface_tube_curves'] = sum(bool(c.get('source_tube_certificate_version') == 1) for c in curves)
        dump(stage/'run.json', metadata); dump(out/'run.json', metadata)
        if not failed:
            metadata['status'] = 'local_diagnostic' if regions is not None else 'fitted'
            dump(stage/'run.json', metadata); dump(out/'run.json', metadata)
            return metadata
        unresolved_minor = [r['chart'] for r in records if not r['accepted'] and r.get('failure_class') == 'minor_residual']
        substantial = sorted(set(failed)-set(unresolved_minor))
        if not split_minor_residuals and unresolved_minor and (not substantial or iteration+1 >= rounds):
            metadata.update(status='failed', unresolved_minor_charts=unresolved_minor,
                            stop_reason='Short corrections exhausted; minor residuals do not trigger subdivision')
            dump(stage/'run.json', metadata); dump(out/'run.json', metadata)
            raise ValueError('Unresolved minor residuals after one short correction')
        if iteration+1 >= rounds:
            break
        if unresolved_minor and not split_minor_residuals:
            # A substantial failure already requires this one layout pass.
            # Carry remaining tiny failures with it, without adding a pass or
            # any further coefficient-correction attempts just for those faces.
            metadata['minor_residuals_in_required_replan'] = dict(iteration=iteration,
                minor_charts=unresolved_minor, triggering_charts=substantial, additional_layout_rounds=0)
            dump(stage/'run.json', metadata); dump(out/'run.json', metadata)
        next_domains = []; split_seeds = []
        for di, d in enumerate(domains):
            if di not in failed:
                next_domains.append(d); continue
            ids = d['triangle_ids']
            if len(ids) <= 1:
                raise ValueError(f'Single triangle cannot meet the shared skeleton: {di}')
            for part in split_faces(v, f, ids, graph):
                split_seeds.append((d['region'], part))
        domains = next_domains+regular_domains(v, f, graph, split_seeds, minimum_graph_cosine,
                                              config.distance*fitting_fraction)
    metadata['status'] = 'failed'; dump(out/'run.json', metadata)
    raise ValueError('Regional skeleton refinement limit exceeded')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--partition', required=True); p.add_argument('--stl', required=True)
    p.add_argument('--output', required=True); p.add_argument('--smoothing', type=float, default=.04)
    p.add_argument('--workers', type=int, default=4); p.add_argument('--regions', type=int, nargs='+')
    p.add_argument('--face-limit', type=int); p.add_argument('--rounds', type=int, default=4)
    p.add_argument('--fairness', type=float, default=1e-8); p.add_argument('--normal-weight', type=float, default=.0001)
    p.add_argument('--from-fit')
    p.add_argument('--boundary-mode', choices=['chain', 'fair-network', 'fair-chains'], default='chain')
    p.add_argument('--normal-field', choices=['vertex', 'crease-fans'], default='vertex')
    p.add_argument('--normal-filter-steps', type=int, default=0)
    p.add_argument('--boundary-cache')
    p.add_argument('--support-solver', choices=['least-squares', 'bounded'], default='least-squares')
    p.add_argument('--refit-supports', action='store_true')
    p.add_argument('--refit-regions', type=int, nargs='+')
    p.add_argument('--boundary-plane-budget', type=float)
    p.add_argument('--boundary-normal-weight', type=float, default=0.)
    p.add_argument('--normal-angle-limit', type=float)
    p.add_argument('--fairness-domain', choices=['rectangle', 'source'], default='rectangle')
    p.add_argument('--area-weighted', action='store_true')
    p.add_argument('--fairness-order', type=int, choices=[2, 3], default=2)
    p.add_argument('--repair-charts', type=int, nargs='+', default=[])
    a = p.parse_args()
    try:
        reconstruct(a.partition, a.stl, a.output, a.smoothing, a.workers, a.regions,
                    a.rounds, a.face_limit, Config(distance=.03, boundary=.005, controls=(65,),
                    normal_weight=a.normal_weight, fairness=a.fairness, boundary_normal_weight=a.boundary_normal_weight,
                    normal_angle_limit=a.normal_angle_limit, fairness_domain=a.fairness_domain,
                    area_weighted=a.area_weighted, fairness_order=a.fairness_order)
                    if a.from_fit is None or a.refit_supports else None,
                    from_fit=a.from_fit, boundary_mode=a.boundary_mode,
                    normal_field=a.normal_field, normal_filter_steps=a.normal_filter_steps, boundary_cache=a.boundary_cache,
                    support_solver=a.support_solver, refit_supports=a.refit_supports,
                    boundary_plane_budget=a.boundary_plane_budget, repair_charts=a.repair_charts,
                    refit_regions=a.refit_regions)
    except (Exception, KeyboardInterrupt) as error:
        path = Path(a.output)/'run.json'
        if path.exists():
            report = json.loads(path.read_text(encoding='utf-8'))
            report.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                          error=f'{type(error).__name__}: {error}')
            dump(path, report)
        raise
