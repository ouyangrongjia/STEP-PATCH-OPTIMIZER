"""Reconstruct feature-level master curves across incidental chart junctions.

Topological uses remain explicit: every child is an exact restriction of one
master spline. A chart junction is not an interpolation condition on that
master. Branches share the moved endpoint, and candidate wires/supports must
still be checked before assembly.
"""
from collections import defaultdict
from dataclasses import dataclass, asdict
import argparse
import json
from pathlib import Path
import shutil

import numpy as np
from scipy.interpolate import BSpline

from .mesh import dump, Locator
from .splines import knots, sparse_basis
from .network_fairing import incidence
from .boundary_quality import measures
from .regional_rectangle import curve_polyline_error
from .surface_boundary import certify_tube


@dataclass
class Config:
    movement: float = .12
    tube: float = .012
    corner_degrees: float = 45.
    direction_span: float = .25
    minimum_length: float = .5
    maximum_controls: int = 16
    fairness_length: float = .25
    interface_kind: str = 'regional'


def route_graph(curves, regions, vertices, config, selected_regions=None, allowed_curves=None, fixed_nodes=()):
    """Continue the same region interface through chart-only subdivisions.

    Secants over a physical span replace noisy endpoint derivatives. Ambiguous
    branches and region junctions stop a route. Closed interfaces remain
    explicit, so they cannot accidentally acquire a fixed artificial corner.
    """
    groups = defaultdict(list)
    for ci, c in sorted(curves.items()):
        if allowed_curves is not None and ci not in allowed_curves:
            continue
        pair = tuple(sorted(regions[int(i)] for i in c['owners']))
        if selected_regions is not None and not set(pair).intersection(selected_regions):
            continue
        if (pair[0] == pair[1]) == (config.interface_kind == 'internal'):
            groups[pair].append(ci)
    routes = []
    for pair, ids in sorted(groups.items()):
        local = {ci: curves[ci] for ci in ids}
        uses = incidence(local); continuation = {}
        for vi, ends in uses.items():
            if len(ends) != 2 or vi in fixed_nodes:
                continue
            directions = []
            for ci, end in ends:
                points = vertices[local[ci]['chain']]
                if end:
                    points = points[::-1]
                s = np.r_[0., np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
                d = np.array([np.interp(min(config.direction_span, s[-1]), s, points[:, k]) for k in range(3)])-points[0]
                directions.append(d/max(np.linalg.norm(d), 1e-30))
            if np.dot(*directions) <= -np.cos(np.radians(config.corner_degrees)):
                a, b = ends; continuation[a] = b; continuation[b] = a
        remaining = set(ids)
        while remaining:
            starts = [(ci, end) for ci in sorted(remaining) for end in (0, 1) if (ci, end) not in continuation]
            current = starts[0] if starts else (min(remaining), 0)
            chain = []; members = []; closed = not starts
            while current[0] in remaining:
                ci, start = current; c = local[ci]
                path = c['chain'] if start == 0 else c['chain'][::-1]
                chain.extend(path.tolist() if not chain else path[1:].tolist())
                members.append((ci, start == 0)); remaining.remove(ci)
                next_use = continuation.get((ci, 1-start))
                if next_use is None:
                    break
                current = next_use
            # A closed spline needs periodic fitting; preserve it explicitly
            # instead of cutting it into artificial fixed-end curves here.
            routes.append(dict(regions=list(pair), members=members, chain=np.array(chain), closed=closed))
    return routes


def restrict(poles, knot, first, last, forward=True):
    """Exact cubic B-spline restriction, reparameterized to [0, 1]."""
    if not 0 <= first < last <= 1:
        raise ValueError('Invalid master interval')
    b = BSpline(knot, poles, 3)
    for u in (first, last):
        count = int(np.count_nonzero(b.t == u))
        if count < 4:
            b = b.insert_knot(u, 4-count)
    begin = int(np.searchsorted(b.t, first, side='left'))
    end = int(np.searchsorted(b.t, last, side='right'))
    t = (b.t[begin:end]-first)/(last-first)
    p = b.c[begin:begin+len(t)-4].copy()
    if not forward:
        p, t = p[::-1].copy(), 1-t[::-1]
    return p, t


def fit_master(points, endpoints, locator, config):
    """Try increasing low-complexity bases, never interpolate old junctions."""
    length = np.linalg.norm(np.diff(points, axis=0), axis=1)
    s = np.r_[0., np.cumsum(length)]; s /= s[-1]
    total = float(length.sum())
    sample = np.unique(np.r_[(s[:-1, None]+np.diff(s)[:, None]*np.linspace(0, 1, 5)).ravel(),
                             np.linspace(0, 1, max(65, int(total/.03)))])
    target = np.column_stack([np.interp(sample, s, points[:, k]) for k in range(3)])
    origin = target.mean(axis=0); target -= origin
    trials = []
    for count in (4, 5, 6, 8, 10, 12, 16, 24, 32):
        if count > config.maximum_controls:
            continue
        kt = knots(count); b = sparse_basis(kt, sample).toarray()
        q, w = np.polynomial.legendre.leggauss(4); cut = np.unique(kt)
        q = (cut[:-1, None]+np.diff(cut)[:, None]*(q+1)/2).ravel()
        w = (np.diff(cut)[:, None]*w/2).ravel()
        d2 = sparse_basis(kt, q, 2).toarray()
        mass = np.r_[np.diff(sample)[0]/2, (sample[2:]-sample[:-2])/2, np.diff(sample)[-1]/2]
        h = b.T@(mass[:, None]*b)+(min(config.fairness_length, total*.25)/total)**4*(d2.T@(w[:, None]*d2))
        rhs = b.T@(mass[:, None]*target)
        cp = np.zeros((count, 3)); cp[[0, -1]] = endpoints-origin
        cp[1:-1] = np.linalg.solve(h[1:-1, 1:-1], rhs[1:-1]-h[1:-1][:, [0, -1]]@cp[[0, -1]])
        cp += origin
        movement, _ = curve_polyline_error(cp, kt, points, s)
        item = dict(controls=count, movement_mm=float(movement)); trials.append(item)
        if movement > config.movement:
            item['reason'] = 'movement'; continue
        # Fit the low-rank master to the source surface, not to the sawtooth
        # boundary itself. Only closest-point normal residuals enter this
        # projection; tangential motion remains controlled by the route data.
        projection = []
        fixed = np.r_[np.arange(3), np.arange(3*(count-1), 3*count)]
        free = np.setdiff1d(np.arange(3*count), fixed)
        for step in range(12):
            xyz = b@cp
            nearest, distance, ids, _ = locator.closest(xyz)
            projection.append(float(distance.max()))
            if distance.max() < config.tube*.85:
                break
            tri = locator.vertices[locator.triangles[ids]]
            normal = np.cross(tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0])
            normal /= np.linalg.norm(normal, axis=1)[:, None]
            active = distance > config.tube*.1
            normal[active] = (xyz[active]-nearest[active])/distance[active, None]
            op = (b[:, :, None]*normal[:, None, :]).reshape(len(b), 3*count)
            strength = min(100., 3.*2**(step//3))
            hh = np.kron(h, np.eye(3))+strength*(op.T@(mass[:, None]*op))
            rr = rhs.ravel()+strength*op.T@(mass*np.sum(normal*(nearest-origin), axis=1))
            new = (cp-origin).ravel()
            new[free] = np.linalg.solve(hh[np.ix_(free, free)], rr[free]-hh[np.ix_(free, fixed)]@new[fixed])
            cp = new.reshape(-1, 3)+origin
        item['projection_max_history_mm'] = projection
        movement, _ = curve_polyline_error(cp, kt, points, s)
        item['movement_mm'] = float(movement)
        if movement > config.movement:
            item['reason'] = 'projected_movement'; continue
        cert = certify_tube(cp, kt, locator, config.tube)
        cert.pop('violating_parameters', None); item['source_tube'] = cert
        if not cert['passed']:
            item['reason'] = 'source_tube'; continue
        # Bound forward progress along the macroscopic source route. A small
        # data residual does not by itself rule out a stationary point or hook.
        velocity = BSpline(kt, cp, 3)(sample, 1)
        span = min(config.direction_span/total, .2)
        ahead, behind = np.minimum(sample+span, 1.), np.maximum(sample-span, 0.)
        reference = np.column_stack([np.interp(ahead, s, points[:, k])-np.interp(behind, s, points[:, k]) for k in range(3)])
        progress = np.einsum('ij,ij->i', velocity, reference)
        if np.any(progress <= 0):
            item['reason'] = 'backtracking'; continue
        return dict(poles=cp, knots=kt, parameters=s, movement_mm=movement,
                    source_tube_certified_max_mm=cert['certified_max_mm']), trials
    return None, trials


def reconstruct(source, output, selected_regions=None, config=None):
    config = config or Config()
    if (min(config.movement, config.tube, config.minimum_length, config.direction_span) <= 0
            or config.maximum_controls < 4 or not 0 < config.corner_degrees < 90
            or config.interface_kind not in ('regional', 'internal')):
        raise ValueError('Invalid curve route configuration')
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if not meta.get('requires_support_refit'):
        meta['force_refit_charts'] = []
    if config.tube >= meta['config']['distance']:
        raise ValueError('Curve source tube must fit inside the original surface distance budget')
    z = np.load(meta['partition']); v, f, labels = (z[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    charts = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    regions = {di: int(c['region']) for di, c in charts.items()}
    allowed = None; fixed = set()
    previous_masters = list((root/'routes').glob('*.npz')) if (root/'routes').exists() else []
    offset_id = max((int(p.stem) for p in previous_masters), default=-1)+1
    if config.interface_kind == 'internal':
        from .seam_fairing import source_angles
        angles = source_angles(v, f)
        allowed = {ci for ci, c in curves.items() if 'route_id' not in c and
                   max(angles[tuple(sorted((a, b)))] for a, b in zip(c['chain'][:-1], c['chain'][1:])) <= meta['config']['feature_angle']}
        fixed = {int(vi) for c in curves.values() if 'route_id' in c for vi in c['chain'][[0, -1]]}
    routes = route_graph(curves, regions, v, config, selected_regions, allowed, fixed)
    decisions = []; locators = {}; changed = {}; anchors = {}; masters = []
    for ri, route in enumerate(routes):
        if ri % 20 == 0:
            print(dict(stage='master_fit', route=ri, total=len(routes), accepted=len(masters)), flush=True)
        ri += offset_id
        chain = route['chain']; points = v[chain]
        length = float(np.linalg.norm(np.diff(points, axis=0), axis=1).sum())
        row = dict(route=ri, regions=route['regions'], members=route['members'], closed=route['closed'], length_mm=length)
        decisions.append(row)
        if route['closed'] or length < config.minimum_length:
            row.update(accepted=False, reason='closed' if route['closed'] else 'short'); continue
        pair = tuple(route['regions'])
        if pair not in locators:
            ids = np.flatnonzero(np.isin(labels, pair)); used = np.unique(f[ids])
            locators[pair] = Locator(v[used], np.searchsorted(used, f[ids]))
        first, direction = route['members'][0]; last, last_direction = route['members'][-1]
        ends = np.array([curves[first]['poles'][0 if direction else -1], curves[last]['poles'][-1 if last_direction else 0]])
        master, trials = fit_master(points, ends, locators[pair], config)
        row['trials'] = trials
        if master is None:
            row.update(accepted=False, reason='no_low_complexity_fit'); continue
        row.update(accepted=True, controls=len(master['poles']), before_turn_degrees=sum(measures(curves[ci])['total_tangent_turn_degrees'] for ci, _ in route['members']),
                   after=measures(master))
        masters.append(dict(master, route=ri, chain=chain))
        offset = 0
        for ci, forward in route['members']:
            c = curves[ci]; start = master['parameters'][offset]; offset += len(c['chain'])-1; stop = master['parameters'][offset]
            cp, kt = restrict(master['poles'], master['knots'], start, stop, forward)
            child = dict(c, poles=cp, knots=kt, route_id=ri, route_interval=np.array([start, stop]),
                         route_forward=forward, method='feature_route_restriction', movement_budget_mm=config.movement,
                         source_tube_limit_mm=config.tube, source_tube_certificate_version=1,
                         source_tube_certified_max_mm=master['source_tube_certified_max_mm'])
            t = master['parameters'][offset-len(c['chain'])+1:offset+1]
            child['parameters'] = (t-start)/(stop-start) if forward else (stop-t[::-1])/(stop-start)
            changed[ci] = child
            for vi, pt in zip(c['chain'][[0, -1]], cp[[0, -1]]):
                if int(vi) in anchors and np.linalg.norm(anchors[int(vi)]-pt) > 1e-8:
                    raise ValueError('Master routes disagree at a junction')
                anchors[int(vi)] = pt
    # Coordinate every remaining branch with the shared master junction.
    for ci, c in curves.items():
        if ci in changed:
            continue
        delta = np.array([anchors.get(int(vi), old)-old for vi, old in zip(c['chain'][[0, -1]], c['poles'][[0, -1]])])
        if np.max(np.linalg.norm(delta, axis=1)) <= 1e-12:
            continue
        if 'route_id' in c:
            raise ValueError('A new route attempted to move an existing master junction')
        g = np.array([c['knots'][i+1:i+4].mean() for i in range(len(c['poles']))])
        changed[ci] = dict(c, poles=c['poles']+(1-g[:, None])*delta[0]+g[:, None]*delta[1],
                           method='route_branch_endpoint_transport', source_tube_certificate_version=0)
    for name in ('charts', 'curves'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    if previous_masters:
        shutil.copytree(root/'routes', out/'routes')
    else:
        (out/'routes').mkdir()
    for master in masters:
        np.savez_compressed(out/'routes'/f"{master['route']:05d}.npz", **master)
    for ci, c in changed.items():
        c['movement_mm'], _ = curve_polyline_error(c['poles'], c['knots'], v[c['chain']], c['parameters'])
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **c)
    report = dict(config=asdict(config), selected_regions=selected_regions, routes=decisions,
                  accepted_routes=len(masters), previous_master_routes=len(previous_masters), changed_curves=sorted(changed),
                  source_CAD_read=False, commercial_engine_used=False,
                  topological_junctions_removed=0,
                  scope='Geometric master curves span existing chart subdivisions; branch uses remain explicit.')
    dump(out/'curve_routes.json', report)
    affected = sorted({int(di) for c in changed.values() for di in c['owners']})
    history = meta.get('curve_route_reconstruction_history', [])
    if meta.get('curve_route_reconstruction'):
        history = history+[meta['curve_route_reconstruction']]
    meta.update(status='boundary_candidate', source_fit=str(out.resolve()), requires_support_refit=bool(affected),
                required_refit_charts=affected, curve_route_reconstruction=report, can_apply=False)
    meta['curve_route_reconstruction_history'] = history
    dump(out/'run.json', meta)
    print(dict(stage='curve_routes', accepted_routes=len(masters), changed_curves=len(changed), affected_charts=len(affected)), flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--regions', type=int, nargs='+'); p.add_argument('--controls', type=int, default=16)
    p.add_argument('--movement', type=float, default=.12); p.add_argument('--tube', type=float, default=.012)
    p.add_argument('--internal-only', action='store_true')
    a = p.parse_args()
    reconstruct(a.run, a.output, a.regions, Config(maximum_controls=a.controls, movement=a.movement, tube=a.tube,
                interface_kind='internal' if a.internal_only else 'regional'))
