"""Remove redundant bivalent junctions between uses of one master curve."""
import argparse
import json
from pathlib import Path
import shutil
import numpy as np
from .curve_routes import restrict
from .network_fairing import incidence
from .mesh import dump


def coalesce(curves, masters):
    curves = {ci: dict(c) for ci, c in curves.items()}; events = []
    while True:
        merged = False
        for vi, uses in incidence(curves).items():
            if len(uses) != 2:
                continue
            ai, bi = (use[0] for use in uses)
            if ai == bi:
                continue
            a, b = curves[ai], curves[bi]
            if ('route_id' not in a or 'route_id' not in b or int(a['route_id']) != int(b['route_id'])
                    or set(a['owners']) != set(b['owners'])):
                continue
            if a['route_interval'][0] > b['route_interval'][0]:
                a, b = b, a
            left, middle = a['route_interval']; other, right = b['route_interval']
            if abs(middle-other) > 1e-12:
                continue
            def oriented(c):
                start, stop = c['route_interval']
                if bool(c['route_forward']):
                    return c['chain'], start+(stop-start)*c['parameters']
                return c['chain'][::-1], stop-(stop-start)*c['parameters'][::-1]
            ac, at = oriented(a); bc, bt = oriented(b)
            if ac[-1] != vi or bc[0] != vi:
                raise ValueError('Inconsistent master interval orientation')
            master = masters[int(a['route_id'])]; poles, knot = restrict(master['poles'], master['knots'], left, right)
            keep, remove = min(ai, bi), max(ai, bi)
            curves[keep] = dict(a, poles=poles, knots=knot, chain=np.r_[ac, bc[1:]],
                                parameters=(np.r_[at, bt[1:]]-left)/(right-left), route_forward=True,
                                route_interval=np.array([left, right]), movement_mm=max(float(a['movement_mm']), float(b['movement_mm'])))
            del curves[remove]
            events.append(dict(removed_junction=int(vi), combined_uses=[ai, bi], retained_use=keep, route=int(a['route_id'])))
            merged = True; break
        if not merged:
            return curves, events


def compact(source, output):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    masters = {int(p.stem): dict(np.load(p)) for p in (root/'routes').glob('*.npz')}
    records, events = coalesce(curves, masters)
    for name in ('charts', 'routes'):
        shutil.copytree(root/name, out/name)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    (out/'curves').mkdir()
    for i, c in enumerate(records.values()):
        np.savez_compressed(out/'curves'/f'{i:05d}.npz', **c)
    report = dict(before_curves=len(curves), after_curves=len(records), removed_bivalent_junctions=len(events), events=events,
                  geometry_changed=False, source_edge_coverage_changed=False,
                  scope='Topological uses of identical incident charts and one master only; STEP exchange subdivision is independent.')
    dump(out/'route_compact.json', report)
    meta.update(source_fit=str(out.resolve()), route_compact=report)
    dump(out/'run.json', meta)
    print({k: v for k, v in report.items() if k != 'events'}, flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); compact(a.run, a.output)
