"""Audit actual master restrictions, branch geometry, and the source envelope."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.interpolate import BSpline
from .mesh import dump, Locator
from .network_fairing import incidence
from .surface_boundary import certify_tube
from .regional_rectangle import curve_polyline_error
from .boundary_quality import measures
from .partition_fit import incidence as mesh_incidence


def source_strip(vertices, triangles, movement, tube):
    from scipy import sparse
    edges, _ = mesh_incidence(triangles)
    lengths = np.linalg.norm(vertices[edges[:, 1]]-vertices[edges[:, 0]], axis=1)
    graph = sparse.coo_matrix((np.repeat(lengths, 2), (edges.ravel(), edges[:, ::-1].ravel())), shape=(len(vertices), len(vertices))).tocsr()
    vertex_faces = sparse.coo_matrix((np.ones(triangles.size), (triangles.ravel(), np.repeat(np.arange(len(triangles)), 3))), shape=(len(vertices), len(triangles))).tocsr()
    halo = movement+tube+2*float(np.median(lengths))
    def locator(chain):
        distance = sparse.csgraph.dijkstra(graph, indices=chain, min_only=True, limit=halo)
        ids = np.unique(vertex_faces[np.flatnonzero(np.isfinite(distance))].indices)
        used = np.unique(triangles[ids])
        return Locator(vertices[used], np.searchsorted(used, triangles[ids]))
    return locator, halo


def audit(before, after, output, tube=.012, movement=.12):
    old_root, root = Path(before), Path(after)
    if Path(output).exists():
        raise FileExistsError(output)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    data = np.load(meta['partition']); v, f, labels = (data[k] for k in ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    charts = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    old_curves = {int(p.stem): dict(np.load(p)) for p in (old_root/'curves').glob('*.npz')}
    prior = {tuple(c['chain']): c for c in old_curves.values()}
    source_edges = {}; old_vertices = {}
    for ci, c in old_curves.items():
        for a, b in zip(c['chain'][:-1], c['chain'][1:]):
            source_edges[tuple(sorted((int(a), int(b))))] = ci
        for vi, point in zip(c['chain'], BSpline(c['knots'], c['poles'], 3)(c['parameters'])):
            old_vertices[int(vi)] = point
    masters = {int(p.stem): dict(np.load(p)) for p in (root/'routes').glob('*.npz')}
    make_locator, halo = source_strip(v, f, movement, tube)
    rows = []; restrictions = []; max_junction = 0.; moved = 0
    for vi, uses in incidence(curves).items():
        points = np.array([curves[ci]['poles'][0 if end == 0 else -1] for ci, end in uses])
        max_junction = max(max_junction, float(np.max(np.linalg.norm(points-points[0], axis=1))))
        moved += int(np.linalg.norm(points[0]-old_vertices[vi]) > 1e-10)
    for ci, c in curves.items():
        b = prior.get(tuple(c['chain']))
        if b is not None and np.array_equal(c['poles'], b['poles']) and np.array_equal(c['knots'], b['knots']):
            continue
        # A moved regional trim can lie on triangles previously owned by its
        # neighbor. A single old label's finite domain is not the source
        # surface envelope. Grow a connected local triangle strip instead of
        # either freezing the old label boundary or querying another sheet.
        locator = make_locator(c['chain'])
        cert = certify_tube(c['poles'], c['knots'], locator, tube)
        cert.pop('violating_parameters', None)
        displacement, _ = curve_polyline_error(c['poles'], c['knots'], v[c['chain']], c['parameters'])
        reference = sorted({source_edges[tuple(sorted((int(a), int(b))))] for a, b in zip(c['chain'][:-1], c['chain'][1:])})
        old_measures = [measures(old_curves[oi]) for oi in reference]
        old_m = {key: sum(r[key] for r in old_measures) for key in old_measures[0]}
        new_m = measures(c)
        rows.append(dict(curve=ci, method=str(c['method']), before=old_m, after=new_m,
                         original_curve_uses=reference,
                         source_tube=cert, movement_mm=displacement,
                         movement_passed=bool(displacement <= movement*(1+1e-8)),
                         branch_turn_regression=bool('route_id' not in c and new_m['total_tangent_turn_degrees'] > old_m['total_tangent_turn_degrees']*1.05+2)))
        if 'route_id' in c:
            master = masters[int(c['route_id'])]; a, b = c['route_interval']; forward = bool(c['route_forward'])
            q = np.unique(np.r_[np.linspace(0, 1, 33), c['knots']])
            child = BSpline(c['knots'], c['poles'], 3); parent = BSpline(master['knots'], master['poles'], 3)
            u = a+(b-a)*q if forward else b-(b-a)*q
            errors = [float(np.linalg.norm(child(q, d)-parent(u, d)*((b-a)*(1 if forward else -1))**d, axis=1).max()) for d in (0, 1, 2)]
            restrictions.append(dict(curve=ci, route=int(c['route_id']), derivative_errors=errors))
    report = dict(before=str(old_root.resolve()), after=str(root.resolve()), changed_curves=len(rows),
                  moved_junctions=moved, maximum_junction_disagreement_mm=max_junction,
                  master_restrictions=restrictions, master_restrictions_passed=all(max(r['derivative_errors']) < 1e-8 for r in restrictions),
                  source_tube_limit_mm=tube, movement_limit_mm=movement,
                  source_tube_domain=dict(kind='source_chain_connected_geodesic_triangle_halo', radius_mm=halo),
                  source_tube_passed=all(r['source_tube']['passed'] for r in rows),
                  movement_passed=all(r['movement_passed'] for r in rows),
                  branch_turn_regressions=[r['curve'] for r in rows if r['branch_turn_regression']], per_curve=rows,
                  scope='C2 agreement only along restrictions of each master; not a G1/G2 surface certificate.')
    dump(output, report)
    print({k: value for k, value in report.items() if k not in ('per_curve', 'master_restrictions')}, flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    for name in ('before', 'after', 'output'):
        p.add_argument('--'+name, required=True)
    p.add_argument('--movement', type=float, default=.12); p.add_argument('--tube', type=float, default=.012)
    a = p.parse_args(); audit(a.before, a.after, a.output, a.tube, a.movement)
