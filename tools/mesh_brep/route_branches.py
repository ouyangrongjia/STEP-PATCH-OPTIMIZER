"""Refit subordinate branches after their endpoints move onto master curves."""
import argparse
import json
from pathlib import Path
import shutil
import numpy as np
from .mesh import dump
from .curve_routes import Config, fit_master
from .route_audit import source_strip
from .surface_boundary import certify_tube
from .boundary_quality import measures
from .network_support import validate_wires


def repair(source, output, movement=.12, tube=.012):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    z = np.load(meta['partition']); v, f = z['mesh_vertex_original'], z['mesh_triangle']
    charts = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    make_locator, halo = source_strip(v, f, movement, tube)
    changed = []; rejected = []; decisions = []
    for ci, c in list(curves.items()):
        if str(c.get('method', '')) != 'route_branch_endpoint_transport':
            continue
        locator = make_locator(c['chain'])
        cert = certify_tube(c['poles'], c['knots'], locator, tube); cert.pop('violating_parameters', None)
        if cert['passed']:
            c.update(source_tube_certificate_version=1, source_tube_limit_mm=tube,
                     source_tube_certified_max_mm=cert['certified_max_mm'])
            continue
        config = Config(movement=movement, tube=tube, maximum_controls=12, fairness_length=.1)
        fit, trials = fit_master(v[c['chain']], c['poles'][[0, -1]], locator, config)
        row = dict(curve=ci, initial_certificate=cert, trials=trials); decisions.append(row)
        if fit is None:
            row.update(accepted=False, reason='source_fit'); rejected.append(ci); continue
        replacement = dict(c, **fit, method='source_constrained_route_branch', source_tube_certificate_version=1,
                           source_tube_limit_mm=tube, movement_budget_mm=movement)
        curves[ci] = replacement
        failures = validate_wires(v, f, charts, curves, set(map(int, c['owners'])))
        if failures:
            curves[ci] = c; row.update(accepted=False, reason='trim', failures=failures); rejected.append(ci)
        else:
            changed.append(ci); row.update(accepted=True, before=measures(c), after=measures(replacement))
        print(dict(stage='route_branch_repair', curve=ci, accepted=row['accepted']), flush=True)
    for name in ('charts', 'curves', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for ci, c in curves.items():
        np.savez_compressed(out/'curves'/f'{ci:05d}.npz', **c)
    report = dict(changed_curves=changed, failed_curves=rejected, decisions=decisions,
                  source_tube_mm=tube, movement_mm=movement, source_geodesic_halo_mm=halo)
    dump(out/'branch_repair.json', report)
    selected = set(meta.get('required_refit_charts', [])) if meta.get('requires_support_refit') else set()
    selected.update(int(di) for ci in changed for di in curves[ci]['owners'])
    meta.update(source_fit=str(out.resolve()), required_refit_charts=sorted(selected), requires_support_refit=bool(selected),
                route_branch_repair=report, status='boundary_candidate' if selected else meta['status'])
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--movement', type=float, default=.12)
    a = p.parse_args(); repair(a.run, a.output, a.movement)
