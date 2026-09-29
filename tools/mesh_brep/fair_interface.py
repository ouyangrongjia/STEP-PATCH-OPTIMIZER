"""XYZ mesh fit with hard assembly boundaries and tangent-plane control.

CAD supplies only the assembly interface. Interior positions and normal targets
come from the given-partition triangle mesh, including artificial knot lines.
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from .adapt_interface import make_layout, boundary_coverage
from .mesh import read_stl, charts, mean_value, distortion, sample, Locator, dump
from .splines import joint_fit
from .cad import read_generated, topology, edge_checks
from geometry import read_step, evaluate, gp_Pnt, xyz
from spline import from_occt, to_occt, basis
from designer_layout import variable_maps, uv_on_side, export_layout
from OCP.GeomAPI import GeomAPI_ProjectPointOnCurve


def mesh_specs(vertices, triangles, metadata, layout, outer, count, seed):
    atlas = charts(vertices, triangles, metadata)
    specs, probes, reports = [], [], []
    area = np.array([x['audit']['area_mm2'] for x in atlas])
    for fi, chart in enumerate(atlas):
        fixed = []; ids = []
        for side, (chain, t) in enumerate(zip(chart['chains'], chart['chain_t'])):
            if fi < 4:
                if side == 0:
                    params = []
                    for k in chain:
                        point = gp_Pnt(*vertices[k]); curve = outer[fi]
                        candidates = [(point.Distance(curve.StartPoint()), 0.), (point.Distance(curve.EndPoint()), 1.)]
                        project = GeomAPI_ProjectPointOnCurve(point, curve)
                        if project.NbPoints():
                            candidates.append((project.LowerDistance(), project.LowerDistanceParameter()))
                        params.append(min(candidates)[1])
                    params = np.array(params)
                    params[0], params[-1] = 0., 1.
                    if np.any(np.diff(params) <= 0):
                        raise ValueError('Nonmonotone hard-interface mesh correspondence')
                    mapping = layout['sides'][fi][0]['mapping']
                else:
                    sid = [(fi+1) % 4, fi+4, fi][side-1]
                    mapping = next(s['mapping'] for s in layout['sides'][fi] if not s['outer'] and s['seam'] == sid)
                    params = t if side == 1 else 1-t
            else:
                mapping = next(s['mapping'] for s in layout['sides'][fi] if s['seam'] == side+4)
                params = t
            ids.extend(chain[:-1]); fixed.extend(uv_on_side(mapping, params[:-1]))
        uv, residual = mean_value(vertices, chart['triangles'], np.array(ids), np.asarray(fixed))
        quality = distortion(vertices, chart['triangles'], uv)
        if quality['flipped'] or quality['zero']:
            raise ValueError(f'Hard-interface chart failed: {quality}')
        p, tu, tv, _ = from_occt(layout['surfaces'][fi])
        ns = max(1000, int(count*area[fi]/area.sum()))
        points, normals, q, _, _ = sample(vertices, chart['triangles'], ns, seed+fi, uv, chart['normals'])
        lu = np.linalg.norm(p[-1, 0]-p[0, 0]); lv = np.linalg.norm(p[0, -1]-p[0, 0])
        specs.append(dict(uv=q, points=points, normals=normals, tu=tu, tv=tv, lengths=[lu, lv],
                          vertex_uv=uv, triangle_ids=chart['triangle_ids']))
        # Probe the mesh normal field at both sides of C0 knot lines and at
        # chart sides. This constrains tangent planes, not parameter speeds.
        queries = []
        for axis, t in enumerate([tu, tv]):
            unique, multiplicity = np.unique(t, return_counts=True)
            for i in range(1, len(unique)-1):
                if multiplicity[i] < 3:
                    continue
                for direction in [-np.inf, np.inf]:
                    q = np.zeros((97, 2)); q[:, axis] = np.nextafter(unique[i], direction)
                    q[:, 1-axis] = np.linspace(0, 1, 97); queries.append(q)
        for s in layout['sides'][fi]:
            queries.append(uv_on_side(s['mapping'], np.linspace(0, 1, 193)))
        query = np.concatenate(queries)
        locator = Locator(np.c_[uv, np.zeros(len(uv))], chart['triangles'])
        _, _, tid, bary = locator.closest(np.c_[query, np.zeros(len(query))])
        normal = np.einsum('ij,ijk->ik', bary, chart['normals'][chart['triangles'][tid]])
        normal /= np.maximum(np.linalg.norm(normal, axis=1)[:, None], 1e-30)
        probes.append((fi, query, normal))
        reports.append(dict(face=fi, distortion=quality, uv_residual=residual, samples=ns, tangent_probes=len(query)))
    return specs, probes, reports


def run(args):
    start = time.perf_counter(); out = Path(args.output)
    if out.exists() and any(out.iterdir()):
        raise FileExistsError('Refusing to overwrite existing fairing results')
    out.mkdir(parents=True, exist_ok=True)
    layout, _, outer, _ = make_layout(args.mesh_result, read_step(args.region))
    layout['seam_ends'] = [(None, j) for j in range(4)]+[(j, (j+1) % 4) for j in range(4)]
    layout['seam_knots'] = [np.repeat([c.Knot(i) for i in range(1, c.NbKnots()+1)], [c.Multiplicity(i) for i in range(1, c.NbKnots()+1)]) for c in layout['seam_curves']]
    maps, _, _ = variable_maps(layout)
    offsets = [from_occt(s)[0].reshape(-1, 3) for s in layout['surfaces']]
    vertices, tri = read_stl(args.stl); meta = json.loads(Path(args.layout).read_text(encoding='utf-8'))
    specs, probes, uv_report = mesh_specs(vertices, tri, meta, layout, outer, 36000, 2909300)
    hard_tangents = []
    if args.corner_planes:
        for i in range(4):
            a, b = np.asarray(xyz(outer[i].DN(0., 1))), np.asarray(xyz(outer[(i-1) % 4].DN(1., 1)))
            a /= np.linalg.norm(a); b /= np.linalg.norm(b); n = np.cross(a, b)
            if np.linalg.norm(n) < 1e-6:
                raise ValueError('Adjacent boundary tangents cannot determine a stable corner plane')
            n /= np.linalg.norm(n)
            side = next(s for s in layout['sides'][i] if not s['outer'] and s['seam'] == i)
            q = uv_on_side(side['mapping'], [0., 1.])
            # One shared radial derivative per corner. Its perpendicularity to
            # this plane normal makes both incident faces tangent-compatible.
            hard_tangents.append((i, q[:1], n[None], q[1]-q[0]))
    prepared = time.perf_counter()
    poles, solver = joint_fit(specs, maps, offsets, probes, normal_weight=args.normal_weight,
                              fairness=args.fairness, seam_weight=args.tangent_weight, quadrature_order=4,
                              hard_tangents=hard_tangents)
    for fi, (p, spec) in enumerate(zip(poles, specs)):
        layout['surfaces'][fi] = to_occt(p, spec['tu'], spec['tv'])
        np.savez_compressed(out/f'patch_{fi}.npz', poles=p, tu=spec['tu'], tv=spec['tv'],
                            vertex_uv=spec['vertex_uv'], triangle_ids=spec['triangle_ids'])
    for sid, curve in enumerate(layout['seam_curves']):
        fi, side = next((fi, s) for fi, ss in enumerate(layout['sides']) for s in ss if not s['outer'] and s['seam'] == sid)
        t = layout['seam_knots'][sid]; greville = np.array([t[i+1:i+4].mean() for i in range(len(t)-4)])
        values, _ = evaluate(layout['surfaces'][fi], uv_on_side(side['mapping'], greville))
        master = np.linalg.solve(basis(t, 3, greville), values)
        for i, point in enumerate(master):
            curve.SetPole(i+1, gp_Pnt(*point))
    layout['inner_final'] = np.asarray([xyz(c.EndPoint()) for c in layout['seam_curves'][:4]])
    coverage = boundary_coverage(layout)
    export_layout(layout, out/'network.step'); shape = read_generated(out/'network.step')
    report = dict(stage='Mesh XYZ refit with hard CAD assembly interface', solver=solver, uv=uv_report,
                  boundary_coverage=coverage, topology=topology(shape), cad_checks=edge_checks(shape),
                  actual_support_control_points=sum(p.shape[0]*p.shape[1] for p in poles),
                  preparation_seconds=prepared-start, wall_seconds=time.perf_counter()-start,
                  inputs=dict(mesh_result=str(Path(args.mesh_result).resolve()), stl=str(Path(args.stl).resolve()),
                              layout=str(Path(args.layout).resolve()), interface=str(Path(args.region).resolve())),
                  can_apply=False, status='DiagnosticOnly')
    dump(out/'fairing.json', report)
    print(json.dumps(dict(topology=report['topology'], controls=report['actual_support_control_points'],
                          boundary_max_mm=coverage['maximum_distance_mm'], wall_seconds=report['wall_seconds'])), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ['mesh-result', 'stl', 'layout', 'region', 'output']:
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--normal-weight', type=float, default=.01)
    parser.add_argument('--fairness', type=float, default=1e-5)
    parser.add_argument('--tangent-weight', type=float, default=.05)
    parser.add_argument('--corner-planes', action='store_true', help='Enforce corner planes from the supplied CAD boundary tangents')
    run(parser.parse_args())
