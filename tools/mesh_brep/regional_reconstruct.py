"""Reconstruct a closed, labeled mesh using shared Bezier boundary ribbons.

The input is STL and a matching indexed region array. Region wedges constrain
simplification; quad pairing is strictly inside each supplied label. Every
physical edge has one cubic curve and a common normal field on smooth seams.
Faces use polynomial Bezier supports, converted exactly to STEP B-splines.
No source CAD geometry is read.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from math import comb, factorial
from pathlib import Path
import time

import numpy as np

from .mesh import Locator, dump, stats, vertex_normals
from .partition_fit import load_partition, incidence
from .partition_network import simplify


@dataclass
class Config:
    simplify_error: float = .1
    target_triangles: int = 2000
    degree: int = 5
    feature_angle: float = 30.
    normal_weight: float = .08
    seam_weight: float = 1.
    correction_weight: float = .002
    tolerance: float = 1e-7
    grid: int = 9


def bernstein(n, t, derivative=0):
    t = np.asarray(t)
    if derivative:
        previous = bernstein(n-1, t, derivative-1)
        return n*(np.c_[np.zeros(len(t)), previous]-np.c_[previous, np.zeros(len(t))])
    return np.stack([comb(n, i)*t**i*(1-t)**(n-i) for i in range(n+1)], axis=1)


def tensor(n, uv, du=0, dv=0):
    return np.einsum('ni,nj->nij', bernstein(n, uv[:, 0], du),
                     bernstein(n, uv[:, 1], dv)).reshape(len(uv), -1)


def elevate(cp, n):
    while len(cp) <= n:
        degree = len(cp)
        middle = np.array([(i/degree)*cp[i-1]+(1-i/degree)*cp[i]
                           for i in range(1, degree)]).reshape(-1, cp.shape[1])
        cp = np.r_[cp[:1], middle, cp[-1:]]
    return cp


def quad_layout(v, f, labels, excluded_edges=None, matching='greedy'):
    if matching not in {'greedy','maximum'}:
        raise ValueError(f'Unknown quad matching policy: {matching}')
    edges, owners = incidence(f); candidates = []
    excluded_edges=set() if excluded_edges is None else excluded_edges
    for ei, (x, y) in enumerate(owners):
        if labels[x] != labels[y] or tuple(edges[ei]) in excluded_edges:
            continue
        shared = set(edges[ei]); i = next(k for k in range(3) if f[x, k] not in shared)
        q = [f[x, i], f[x, (i+1) % 3], next(k for k in f[y] if k not in shared), f[x, (i+2) % 3]]
        p = v[q]; n = np.cross(p[1]-p[0], p[3]-p[0]); n /= np.linalg.norm(n)
        lengths = np.linalg.norm(np.roll(p, -1, axis=0)-p, axis=1)
        sine = -np.cross(np.roll(p, -1, axis=0)-p, p-np.roll(p, 1, axis=0))@n/(lengths*np.roll(lengths, 1))
        other = np.cross(p[2]-p[1], p[3]-p[1]); other /= np.linalg.norm(other)
        if sine.min() > .12 and n@other > .4:
            candidates.append((float(sine.min()*lengths.min()/lengths.max()), int(x), int(y), q))
    if matching == 'maximum' and candidates:
        # Solve the binary matching problem, including odd cycles. Greedy
        # pairing can strand two triangles that a different pairing joins.
        from scipy import sparse
        from scipy.optimize import Bounds, LinearConstraint, milp
        rows = np.array([[x, y] for _, x, y, _ in candidates]).ravel()
        cols = np.repeat(np.arange(len(candidates)), 2)
        a = sparse.coo_matrix((np.ones(len(rows)), (rows, cols)),
                              shape=(len(f), len(candidates))).tocsc()
        quality = np.array([score for score, _, _, _ in candidates])
        solved = milp(-1-quality/(len(candidates)+1), integrality=np.ones(len(candidates)),
                      bounds=Bounds(0, 1), constraints=LinearConstraint(a, 0, 1),
                      options={'time_limit': 60.})
        if not solved.success:
            raise ValueError(f'Quad matching did not converge: {solved.message}')
        candidates = [c for c, selected in zip(candidates, solved.x > .5) if selected]
    used = set(); result = []
    for _, x, y, q in sorted(candidates, reverse=True):
        if x not in used and y not in used:
            used.update([x, y]); result.append(dict(vertices=[int(k) for k in q], region=int(labels[x]), source=[x, y]))
    result.extend(dict(vertices=tri.tolist(), region=int(labels[i]), source=[i])
                  for i, tri in enumerate(f) if i not in used)
    return result


def make_network(v, layout, normals, feature_angle):
    uses = {}
    for pi, p in enumerate(layout):
        p['edges'] = []
        for a, b in zip(p['vertices'], np.roll(p['vertices'], -1)):
            key = tuple(sorted((int(a), int(b))))
            uses.setdefault(key, []).append(pi); p['edges'].append(key)
    network = {}
    for key, owners in uses.items():
        if len(owners) != 2:
            raise ValueError('An edge lost its two incident regions')
        regions = [layout[i]['region'] for i in owners]
        ns = np.array([[normals[r][k] for k in key] for r in regions])
        dot = np.sum(ns[0]*ns[1], axis=1)
        sharp = regions[0] != regions[1] and float(dot.min()) < np.cos(np.radians(feature_angle))
        common = ns.sum(axis=0); common /= np.maximum(np.linalg.norm(common, axis=1)[:, None], 1e-30)
        chord = v[key[1]]-v[key[0]]; tangents = []
        for j in range(2):
            if sharp:
                ridge = np.cross(ns[0, j], ns[1, j]); ridge /= max(np.linalg.norm(ridge), 1e-30)
                tangent = ridge*np.dot(ridge, chord)
            else:
                tangent = chord-common[j]*np.dot(common[j], chord)
            tangents.append(tangent)
        cp = np.array([v[key[0]], v[key[0]]+tangents[0]/3,
                       v[key[1]]-tangents[1]/3, v[key[1]]])
        line = v[key[0]]+np.linspace(0, 1, 4)[:, None]*chord
        network[key] = dict(cp=cp, line=line, owners=owners, sharp=sharp,
                            normals={r: ns[i] if sharp else common for i, r in enumerate(regions)}, scale=1.)
    return network


def triangular_net(corners, boundary, degree):
    # Cubic triangular PN interior; boundary coefficients are shared globally.
    cp = {(3, 0, 0): corners[0], (0, 3, 0): corners[1], (0, 0, 3): corners[2],
          (2, 1, 0): boundary[0][1], (1, 2, 0): boundary[0][2],
          (0, 2, 1): boundary[1][1], (0, 1, 2): boundary[1][2],
          (1, 0, 2): boundary[2][1], (2, 0, 1): boundary[2][2]}
    cp[(1, 1, 1)] = sum(boundary[i][1]+boundary[i][2] for i in range(3))/4-corners.sum(axis=0)/6
    for n in range(4, degree+1):
        updated = {}
        for a in range(n+1):
            for b in range(n-a+1):
                c = n-a-b; value = np.zeros(3)
                for j, k in enumerate((a, b, c)):
                    if k:
                        old = [a, b, c]; old[j] -= 1
                        value += k/n*cp[tuple(old)]
                updated[(a, b, c)] = value
        cp = updated
    keys = list(cp); transform = np.zeros(((degree+1)**2, len(keys)))
    for j, key in enumerate(keys):
        a, b, c = key
        coeff = np.zeros((degree-c+1, 1)); coeff[b] = 1
        transform.reshape(degree+1, degree+1, -1)[:, c, j] = elevate(coeff, degree).ravel()
    poles = (transform@np.array([cp[k] for k in keys])).reshape(degree+1, degree+1, 3)
    free = [i for i, key in enumerate(keys) if min(key) > 0]
    return poles, transform[:, free]


def initial_support(v, patch, network, degree):
    boundary = []
    for key, a in zip(patch['edges'], patch['vertices']):
        edge = network[key]; cp = edge['line']+edge['scale']*(edge['cp']-edge['line'])
        boundary.append(cp if a == key[0] else cp[::-1])
    if len(boundary) == 3:
        return triangular_net(v[patch['vertices']], boundary, degree)
    bottom, right, top, left = [elevate(c, degree) for c in boundary]
    top, left = top[::-1], left[::-1]
    t = np.linspace(0, 1, degree+1)
    u, w = t[:, None, None], t[None, :, None]
    corners = v[patch['vertices']]
    bilinear = ((1-u)*(1-w)*corners[0]+u*(1-w)*corners[1]
                +u*w*corners[2]+(1-u)*w*corners[3])
    poles = (1-w)*bottom[:, None]+w*top[:, None]+(1-u)*left[None]+u*right[None]-bilinear
    free = np.array([i*(degree+1)+j for i in range(1, degree) for j in range(1, degree)])
    return poles, np.eye((degree+1)**2)[:, free]


def edge_uv(count, side, t):
    if count == 3 and side == 2:
        return np.c_[0*t, 1-t]
    return [lambda: np.c_[t, 0*t], lambda: np.c_[1+0*t, t],
            lambda: np.c_[1-t, 1+0*t], lambda: np.c_[0*t, 1-t]][side]()


def evaluate(poles, uv):
    degree = len(poles)-1; p = poles.reshape(-1, 3)
    origin = p[0].copy(); p = p-origin
    xyz = tensor(degree, uv)@p+origin
    du, dv = tensor(degree, uv, 1, 0)@p, tensor(degree, uv, 0, 1)@p
    n = np.cross(du, dv); area = np.linalg.norm(n, axis=1)
    return xyz, n/np.maximum(area[:, None], 1e-30), area


def fit_support(v, patch, network, reference, source_normals, config):
    poles, transform = initial_support(v, patch, network, config.degree)
    origin = poles[0, 0].copy(); base = (poles-origin).reshape(-1, 3)
    corner = v[patch['vertices']]
    normal = sum((np.cross(corner[i]-corner[0], corner[i+1]-corner[0]) for i in range(1, len(corner)-1)), np.zeros(3))
    normal /= np.linalg.norm(normal)
    grid = np.linspace(.025, .975, config.grid)
    uv = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    a = tensor(config.degree, uv); initial = a@base+origin
    target, _, ids, bary = reference.closest(initial)
    normals = np.einsum('ij,ijk->ik', bary, source_normals[reference.triangles[ids]])
    normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-30)
    size = max(np.linalg.norm(np.ptp(v[patch['vertices']], axis=0)), 1e-12)
    free_count = transform.shape[1]
    # The affine chart's tangent coordinates are not fitting unknowns. Scalar
    # normal displacements preserve its Jacobian sign and remove the poorly
    # conditioned tangential modes responsible for folded skinny patches.
    matrix = [(a@transform)/np.sqrt(len(uv))]
    rhs = [(target-initial)@normal/np.sqrt(len(uv))]
    for du, dv in [(1, 0), (0, 1)]:
        derivative = tensor(config.degree, uv, du, dv)
        length = np.maximum(np.linalg.norm(derivative@base, axis=1), size*.01)
        weight = np.sqrt(config.normal_weight/len(uv))*size/length
        matrix.append((derivative@transform)*(normals@normal*weight)[:, None])
        rhs.append(-np.sum((derivative@base)*normals, axis=1)*weight)
    for side, (key, vertex) in enumerate(zip(patch['edges'], patch['vertices'])):
        t = np.linspace(.025, .975, 13); forward = vertex == key[0]
        canonical = t if forward else 1-t; edge = network[key]
        cp = edge['line']+edge['scale']*(edge['cp']-edge['line'])
        tangent = bernstein(3, canonical, 1)@cp
        tangent /= np.maximum(np.linalg.norm(tangent, axis=1)[:, None], 1e-30)
        nn = edge['normals'][patch['region']]
        seam_normal = (1-canonical[:, None])*nn[0]+canonical[:, None]*nn[1]
        seam_normal -= tangent*np.sum(seam_normal*tangent, axis=1)[:, None]
        seam_normal /= np.maximum(np.linalg.norm(seam_normal, axis=1)[:, None], 1e-30)
        q = edge_uv(len(patch['vertices']), side, t)
        for du, dv in [(1, 0), (0, 1)]:
            derivative = tensor(config.degree, q, du, dv)
            length = np.maximum(np.linalg.norm(derivative@base, axis=1), size*.01)
            weight = np.sqrt(config.seam_weight/(len(t)*len(patch['edges'])))*size/length
            matrix.append((derivative@transform)*(seam_normal@normal*weight)[:, None])
            rhs.append(-np.sum((derivative@base)*seam_normal, axis=1)*weight)
    matrix.append(np.eye(free_count)*np.sqrt(config.correction_weight))
    rhs.append(np.zeros(free_count))
    heights = np.linalg.lstsq(np.vstack(matrix), np.concatenate(rhs), rcond=1e-10)[0]
    correction = heights[:, None]*normal
    result = (base+transform@correction+origin).reshape(poles.shape)
    # Fold checks are repeated after fitting, with one fixed coarse orientation.
    xyz, n, jac = evaluate(result, uv)
    min_dot = float(np.min(n@normal))
    distance = np.linalg.norm(xyz-target, axis=1)
    return result, dict(training_distance_mm=stats(distance), minimum_coarse_normal_dot=min_dot,
                        minimum_jacobian=float(jac.min()), maximum_correction_mm=float(np.linalg.norm(transform@correction, axis=1).max()))


def reconstruct(partition, stl, output, config, cached_network=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter()
    report = dict(status='running', algorithm='regional-bezier-ribbons-v2-scalar', config=asdict(config),
                  stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()),
                  units='mm', source_CAD_read=False, supplied_regions=True)
    dump(out/'run.json', report)
    try:
        source_v, source_f, labels, check = load_partition(partition, stl)
        report['input_topology'] = check
        v, f, region, source_ids, reduction = simplify(source_v, source_f, labels, config.simplify_error, config.target_triangles)
        np.savez_compressed(out/'simplified_mesh.npz', vertices=v, triangles=f, labels=region, source_ids=source_ids)
        report['simplification'] = reduction
        layout = quad_layout(v, f, region)
        nodal = {}
        for label in np.unique(labels):
            selected = source_f[labels == label]; ids = np.unique(selected)
            normal = vertex_normals(source_v[ids], np.searchsorted(ids, selected))
            nodal[int(label)] = {i: normal[np.searchsorted(ids, source_ids[i])]
                                 for i in np.unique(f[region == label])}
        network = make_network(v, layout, nodal, config.feature_angle)
        reference = Locator(source_v, source_f); sn = vertex_normals(source_v, source_f)
        patches = [None]*len(layout); records = [None]*len(layout); failures = []
        pending = set(range(len(layout)))
        for repeat in range(9):
            bad = []
            for i in sorted(pending):
                p = layout[i]
                poles, record = fit_support(v, p, network, reference, sn, config)
                patches[i] = dict(**p, poles=poles); records[i] = record
                if record['minimum_coarse_normal_dot'] <= .001:
                    bad.append(i)
                if i % 500 == 0:
                    print(json.dumps(dict(stage='fit', repeat=repeat, patch=i, total=len(layout), seconds=time.perf_counter()-started)), flush=True)
            if not bad:
                break
            failures.append(dict(repeat=repeat, patches=bad))
            print(json.dumps(dict(stage='fold_check', repeat=repeat, rejected=len(bad))), flush=True)
            affected = {key for i in bad for key in layout[i]['edges']}
            pending = {owner for key in affected for owner in network[key]['owners']}
            for key in affected:
                network[key]['scale'] = network[key]['scale']*.25 if repeat < 5 else 0.
        dump(out/'fitting_attempts.json', dict(damping_retries=failures, records=records))
        if bad:
            raise ValueError(f'Folded patch rejection: {len(bad)} patches remain after common-edge damping')
        np.savez_compressed(out/'surfaces.npz', poles=np.array([p['poles'] for p in patches]),
                            regions=np.array([p['region'] for p in patches]),
                            corners=np.array([len(p['vertices']) for p in patches]))
        dump(out/'fitting.json', dict(patches=[dict(region=p['region'], vertices=p['vertices'], **rec) for p, rec in zip(patches, records)],
                                     shared_edges=len(network), damping_retries=failures))
        report['fit_seconds'] = time.perf_counter()-started
        from .regional_step import assemble
        report.update(assemble(v, patches, network, out, config.tolerance))
        report.update(status='DiagnosticOnly', can_apply=False)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['wall_seconds'] = time.perf_counter()-started
        dump(out/'run.json', report)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--partition', required=True); parser.add_argument('--stl', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--simplify-error', type=float, default=.1)
    parser.add_argument('--target-triangles', type=int, default=2000)
    parser.add_argument('--degree', type=int, choices=[3, 4, 5], default=5)
    parser.add_argument('--normal-weight', type=float, default=.08)
    parser.add_argument('--seam-weight', type=float, default=1.)
    args = parser.parse_args()
    config = Config(simplify_error=args.simplify_error, target_triangles=args.target_triangles,
                    degree=args.degree, normal_weight=args.normal_weight, seam_weight=args.seam_weight)
    reconstruct(args.partition, args.stl, args.output, config)


if __name__ == '__main__':
    main()
