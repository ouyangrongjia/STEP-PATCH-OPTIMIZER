"""Physical, frame-independent surface jets for sampled seam diagnostics.

The symmetric world-space shape operator compares normal curvature in every
tangent direction. Matching it is a second-order target; finite sampling and
nonzero positional tolerances do not constitute an exact G2 certificate.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.interpolate import BSpline, PPoly
from .mesh import dump
from .partition_fit import parameters, positions
from .splines import tensor
from .regional_quality import weighted_stats
from .shared_support import source_angles
from .boundary_quality import measures


COMPONENTS = ((0, 0), (1, 1), (2, 2), (0, 1), (0, 2), (1, 2))


def jets(record, xyz, derivatives=False):
    """Return oriented unit normals, shape operators, and optional Jacobians."""
    q = parameters(xyz, record); t = record['knots']; size = record['size']
    operators = [tensor(q, t, t, a, b)/size[0]**a/size[1]**b
                 for a, b in ((1, 0), (0, 1), (2, 0), (1, 1), (0, 2))]
    gx, gy, xx, xy, yy = [op@record['height'] for op in operators]
    gradient = np.column_stack((gx, gy)); axes = record['axes']
    w = np.sqrt(1+np.sum(gradient**2, axis=1))
    normal = (axes[2]-gradient@axes[:2])/w[:, None]
    tangent = np.broadcast_to(axes[:2].T, (len(q), 3, 2)).copy()
    tangent += axes[2][None, :, None]*gradient[:, None, :]
    metric_inv = np.eye(2)[None]-gradient[:, :, None]*gradient[:, None, :]/w[:, None, None]**2
    dual = tangent@metric_inv
    hessian = np.empty((len(q), 2, 2))
    hessian[:, 0, 0], hessian[:, 0, 1] = xx, xy
    hessian[:, 1, 0], hessian[:, 1, 1] = xy, yy
    shape = dual@hessian@dual.transpose(0, 2, 1)/w[:, None, None]
    if not derivatives:
        return normal, shape
    projection = (np.eye(3)[None]-normal[:, :, None]*normal[:, None, :])/w[:, None, None]
    ng = -projection@axes[:2].T
    nj = [operators[0].multiply(ng[:, k, 0, None])+operators[1].multiply(ng[:, k, 1, None]) for k in range(3)]
    partials = []
    for k in range(2):
        unit = np.eye(2)[k]
        dt = np.outer(axes[2], unit)
        dm = unit[None, :, None]*gradient[:, None, :]+gradient[:, :, None]*unit[None, None, :]
        dd = dt@metric_inv-dual@dm@metric_inv
        partials.append((dd@hessian@dual.transpose(0, 2, 1)+dual@hessian@dd.transpose(0, 2, 1))/w[:, None, None]
                        -shape*(gradient[:, k]/w**2)[:, None, None])
    for h in (np.array([[1., 0.], [0., 0.]]), np.array([[0., 1.], [1., 0.]]), np.array([[0., 0.], [0., 1.]])):
        partials.append(dual@h@dual.transpose(0, 2, 1)/w[:, None, None])
    sj = [sum(op.multiply(part[:, a, b, None]) for op, part in zip(operators, partials)) for a, b in COMPONENTS]
    return normal, shape, nj, sj


def support_span_parameters(curve, records, quadrature=False):
    """Split the curve at every crossed knot of both support parameterizations.

    Curve knots alone miss narrow surface spans. Parameters of a graph support
    are affine in XYZ, so these intersections are roots of cubic polynomials.
    """
    degree = len(curve['knots'])-len(curve['poles'])-1
    cuts = list(np.r_[curve['knots'], curve['parameters']])
    for rec in records:
        uv_poles = parameters(curve['poles'], rec)
        for axis in range(2):
            polynomial = PPoly.from_spline((curve['knots'], uv_poles[:, axis], degree))
            for knot in np.unique(rec['knots'])[1:-1]:
                roots = polynomial.solve(float(knot), extrapolate=False)
                cuts.extend(roots[np.isfinite(roots) & (roots > 0) & (roots < 1)])
    cuts = np.unique(cuts); cuts = cuts[np.r_[True, np.diff(cuts) > 1e-12]]
    nodes = (np.polynomial.legendre.leggauss(6)[0]+1)/2 if quadrature else np.linspace(0, 1, 5)
    return np.unique(np.r_[cuts, (cuts[:-1, None]+np.diff(cuts)[:, None]*nodes).ravel()])


def seam_sampling(curve, lookup, count=65, smooth_degrees=10., records=None):
    extra = np.r_[curve['knots'], curve['parameters']] if records is None else support_span_parameters(curve, records, quadrature=True)
    q = np.unique(np.r_[np.linspace(0, 1, count), extra])
    spline = BSpline(curve['knots'], curve['poles'], 3)
    xyz = spline(q)
    source = np.array([lookup[tuple(sorted((int(a), int(b))))] for a, b in zip(curve['chain'][:-1], curve['chain'][1:])])
    segment = np.clip(np.searchsorted(curve['parameters'], q, side='right')-1, 0, len(source)-1)
    smooth = source[segment] <= smooth_degrees
    weight = np.linalg.norm(spline(q, 1), axis=1)*np.r_[np.diff(q)[0]/2, (q[2:]-q[:-2])/2, np.diff(q)[-1]/2]
    return q, xyz, weight, smooth


def audit(source, output, samples=65, smooth_degrees=10.):
    root = Path(source)
    if Path(output).exists():
        raise FileExistsError(output)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    with np.load(meta['partition']) as data:
        v, f = data['mesh_vertex_original'], data['mesh_triangle']
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    lookup = source_angles(v, f); rows = []; all_angles = []; all_curvature = []; all_weights = []
    regional = {}; coverage = {}; geometry = {}; gaps = []; gap_weights = []
    for ci, c in sorted(curves.items()):
        pair = tuple(map(int, c['owners'])); region_pair = tuple(int(records[di]['region']) for di in pair)
        q, xyz, weight, smooth = seam_sampling(c, lookup, samples, smooth_degrees, [records[di] for di in pair])
        left, ls = jets(records[pair[0]], xyz); right, rs = jets(records[pair[1]], xyz)
        support_points = [positions(parameters(xyz, records[di]), records[di]) for di in pair]
        gap = np.linalg.norm(support_points[0]-support_points[1], axis=1)
        gaps.append(gap); gap_weights.append(weight)
        angle = np.degrees(np.arccos(np.clip(np.sum(left*right, axis=1), -1., 1.)))
        jump = np.linalg.norm(ls-rs, axis=(1, 2))
        m = measures(c); category = 'master' if 'route_id' in c else 'unreconstructed'
        group = coverage.setdefault(category, dict(curves=0, length_mm=0., regions=set()))
        group['curves'] += 1; group['length_mm'] += m['length_mm']; group['regions'].update(region_pair)
        for name, value in m.items():
            geometry[name] = geometry.get(name, 0.)+value
        row = dict(curve=ci, charts=pair, regions=region_pair, method=str(c.get('method', '')), geometry=m,
                   smooth_length_mm=float(weight[smooth].sum()), position_gap_mm=weighted_stats(gap, weight))
        if smooth.any():
            aa, kk, ww = angle[smooth], jump[smooth], weight[smooth]
            row.update(normal_degrees=weighted_stats(aa, ww), shape_jump_per_mm=weighted_stats(kk, ww))
            for name, values in (('normal', angle), ('curvature', jump)):
                at = np.flatnonzero(smooth)[int(np.argmax(values[smooth]))]
                row[name+'_worst'] = dict(parameter=float(q[at]), xyz=xyz[at].tolist(), value=float(values[at]))
            all_angles.append(aa); all_curvature.append(kk); all_weights.append(ww)
            for region in set(region_pair):
                regional.setdefault(region, []).append((aa, kk, ww))
        rows.append(row)
    summary = lambda a, k, w: dict(normal_degrees=weighted_stats(a, w), shape_jump_per_mm=weighted_stats(k, w),
                                  length_above_5_degrees_mm=float(w[a > 5.].sum()), length_mm=float(w.sum()))
    per_region = {str(region): summary(*[np.concatenate([r[i] for r in items]) for i in range(3)]) for region, items in regional.items()}
    for group in coverage.values():
        group['regions'] = sorted(group['regions'])
    report = dict(curves=len(rows), regions=len({int(r['region']) for r in records.values()}),
                  smooth_source_dihedral_degrees=smooth_degrees, samples_per_curve=samples,
                  sampling='Uniform minimum plus both support knot crossings and six Gauss points per span; distinct from optimization collocation.',
                  smooth=summary(*map(np.concatenate, (all_angles, all_curvature, all_weights))) if all_angles else None,
                  all_seam_position_gap_mm=weighted_stats(np.concatenate(gaps), np.concatenate(gap_weights)),
                  coverage=coverage, geometry=geometry, per_region=per_region, per_curve=rows,
                  scope='All supplied interfaces, including unchanged ones and endpoints. Shape operator differences also include tangent-plane mismatch when normals differ. This is a sampled second-order residual, not an exact G2 certificate.')
    dump(output, report)
    print({k: report[k] for k in ('curves', 'regions', 'smooth', 'geometry')}, flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--samples', type=int, default=65)
    args = parser.parse_args(); audit(args.run, args.output, args.samples)
