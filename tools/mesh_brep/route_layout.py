"""Remove collapsed chart slivers while preserving source region ownership."""
import argparse
import json
from pathlib import Path
import shutil
import numpy as np

from .mesh import dump
from .network_support import validate_wires
from .partition_fit import boundary_edges, loops, projected_boundary_valid
from .skeleton_graph import stable_frame


def merge_candidates(di, charts, curves, vertices, triangles):
    region = int(charts[di]['region'])
    neighbors = {int(i) for c in curves.values() if di in c['owners'] for i in c['owners']}
    valid = []
    for other in sorted(neighbors-{di}):
        if int(charts[other]['region']) != region:
            continue
        ids = np.union1d(charts[di]['triangle_ids'], charts[other]['triangle_ids'])
        used = np.unique(triangles[ids]); local = np.searchsorted(used, triangles[ids])
        try:
            frame = stable_frame(vertices[used], local)
            if frame[-1].min() <= .15 or not projected_boundary_valid(frame[4], boundary_edges(local)):
                continue
            if not loops(boundary_edges(local)):
                continue
        except ValueError:
            continue
        valid.append((other, ids, used))
    return sorted(valid, key=lambda x: (-len(charts[x[0]]['triangle_ids']), x[0]))


def simplify(source, output, maximum_source_triangles=12, absorb_transport_slivers=False):
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    z = np.load(meta['partition']); v, f = z['mesh_vertex_original'], z['mesh_triangle']
    charts = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    assignment = np.load(root/'chart_index.npz')['face_chart'].copy()
    selected = set(meta['required_refit_charts']); events = []; removed = []
    forced = set(meta.get('force_refit_charts', []))
    for iteration in range(128):
        failures = validate_wires(v, f, charts, curves, selected)
        targets = {r['chart'] for r in failures}
        if absorb_transport_slivers:
            for c in curves.values():
                if str(c.get('method', '')) != 'route_branch_endpoint_transport':
                    continue
                pair = list(map(int, c['owners']))
                if int(charts[pair[0]]['region']) == int(charts[pair[1]]['region']):
                    targets.update(di for di in pair if len(charts[di]['triangle_ids']) <= maximum_source_triangles)
        if not targets:
            break
        did_merge = False
        for di in sorted(targets):
            if len(charts[di]['triangle_ids']) > maximum_source_triangles:
                continue
            for other, ids, used in merge_candidates(di, charts, curves, v, f):
                proposed_charts = dict(charts)
                proposed_charts[other] = dict(charts[other], triangle_ids=ids, vertex_ids=used)
                del proposed_charts[di]
                proposed_curves = {}; dropped = []
                for ci, c in curves.items():
                    owners = np.where(c['owners'] == di, other, c['owners'])
                    if len(np.unique(owners)) == 1:
                        dropped.append(ci)
                    else:
                        proposed_curves[ci] = dict(c, owners=owners)
                if validate_wires(v, f, proposed_charts, proposed_curves, {other}):
                    continue
                events.append(dict(removed_chart=di, merged_into=other, region=int(charts[di]['region']),
                                   source_triangles=len(charts[di]['triangle_ids']), removed_curves=dropped))
                charts, curves = proposed_charts, proposed_curves
                selected.discard(di); selected.add(other); assignment[assignment == di] = other
                forced.discard(di); forced.add(other)
                removed.extend(dropped); did_merge = True
                break
            if did_merge:
                break
        if not did_merge:
            if failures:
                dump(out/'layout.json', dict(passed=False, unresolved=failures, merges=events))
                return None
            break
        print(dict(stage='route_layout_merge', merges=len(events), chart=events[-1]['removed_chart']), flush=True)
    else:
        raise ValueError('Chart simplification did not converge')
    # Preserve source-face ancestry exactly, but give the new layout contiguous
    # CAD array indices. Never guess an owner from nearest-label voting.
    mapping = {di: i for i, di in enumerate(sorted(charts))}
    source_coverage = np.concatenate([c['triangle_ids'] for c in charts.values()])
    if not np.array_equal(np.sort(source_coverage), np.arange(len(f))):
        raise ValueError('Chart merge changed source triangle coverage')
    (out/'charts').mkdir(); (out/'curves').mkdir()
    for di, c in charts.items():
        # Existing shared-support bookkeeping refers to the old layout. The
        # geometry can be reused, but no old group may cross new identities.
        c = {k: value for k, value in c.items() if k != 'shared_support_group'}
        np.savez_compressed(out/'charts'/f'{mapping[di]:05d}.npz', **c)
    curve_mapping = {}
    for i, (ci, c) in enumerate(sorted(curves.items())):
        curve_mapping[ci] = i
        np.savez_compressed(out/'curves'/f'{i:05d}.npz', **dict(c, owners=np.array([mapping[int(di)] for di in c['owners']])))
    new_assignment = np.array([mapping[int(di)] for di in assignment])
    np.savez_compressed(out/'chart_index.npz', face_chart=new_assignment)
    fitting = json.loads((root/'fitting.json').read_text(encoding='utf-8'))
    dump(out/'fitting.json', [dict(r, chart=mapping[r['chart']]) for r in fitting if r['chart'] in mapping])
    for name in ('routes',):
        if (root/name).exists():
            shutil.copytree(root/name, out/name)
    report = dict(passed=True, merges=events, removed_curves=removed, chart_mapping=mapping,
                  curve_mapping=curve_mapping, complete_source_coverage=True, partition_labels_unchanged=True)
    dump(out/'layout.json', report)
    meta.update(source_fit=str(out.resolve()), status='boundary_candidate', requires_support_refit=True,
                required_refit_charts=sorted(mapping[i] for i in selected), fitted_charts=len(charts),
                force_refit_charts=sorted(mapping[i] for i in forced),
                route_layout=report)
    dump(out/'run.json', meta)
    print(dict(stage='route_layout', merged_charts=len(events), removed_curves=len(removed), charts=len(charts)), flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--absorb-transport-slivers', action='store_true')
    a = p.parse_args(); simplify(a.run, a.output, absorb_transport_slivers=a.absorb_transport_slivers)
