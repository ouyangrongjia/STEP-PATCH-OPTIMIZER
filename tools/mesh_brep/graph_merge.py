"""Agglomerate fitting charts without crossing or relabelling input regions.

Representation splits are reversible. A union is retained only when it passes
the original graph, boundary topology, distance and boundary fitting criteria.
Source samples and semantic partition labels remain unchanged.
"""
import argparse
from dataclasses import asdict
import heapq
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump, vertex_normals
from .partition_fit import Config, incidence, boundary_edges, solve_graph


def merge(run, output, trials=1200, maximum_triangles=2400, small_first=True):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False); (out/'charts').mkdir()
    start = time.perf_counter(); source = json.loads((root/'run.json').read_text(encoding='utf-8'))
    config = Config(**source['config']); data = np.load(source['partition'])
    v, f, labels = data['mesh_vertex_original'], data['mesh_triangle'], data['face_instance_id']
    active = {i: dict(np.load(p)) for i, p in enumerate(sorted((root/'charts').glob('*.npz')))}
    original_count = len(active); assignment = np.load(root/'chart_index.npz')['face_chart'].copy()
    edges, owners = incidence(f); lengths = np.linalg.norm(v[edges[:, 1]]-v[edges[:, 0]], axis=1)
    adjacent = {}; edge_pairs = assignment[owners]
    for (a, b), length in zip(edge_pairs, lengths):
        a, b = int(a), int(b)
        if a == b or active[a]['region'] != active[b]['region']:
            continue
        adjacent.setdefault(a, {})[b] = adjacent.get(a, {}).get(b, 0.)+length
        adjacent.setdefault(b, {})[a] = adjacent.get(b, {}).get(a, 0.)+length
    global_normals = vertex_normals(v, f) if config.shared_smooth_normals else None
    sharp = np.array([], dtype=int)
    if global_normals is not None:
        normal = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
        normal /= np.linalg.norm(normal, axis=1)[:, None]
        sharp = np.unique(edges[np.sum(normal[owners[:, 0]]*normal[owners[:, 1]], axis=1)
                                < np.cos(np.radians(config.feature_angle))])
    normals = {}
    for region in np.unique(labels):
        tri = f[labels == region]; ids = np.unique(tri)
        nn = vertex_normals(v[ids], np.searchsorted(ids, tri))
        if global_normals is not None:
            smooth = ~np.isin(ids, sharp); nn[smooth] = global_normals[ids[smooth]]
        normals[int(region)] = (ids, nn)
    queue = []; queued = set()
    def enqueue(a, b):
        a, b = sorted((a, b))
        if (a, b) in queued or a not in active or b not in active:
            return
        na, nb = len(active[a]['triangle_ids']), len(active[b]['triangle_ids'])
        if na+nb > maximum_triangles:
            return
        queued.add((a, b))
        heapq.heappush(queue, ((min(na, nb) if small_first else 0), -adjacent[a][b], a, b))
    for a, values in adjacent.items():
        for b in values:
            enqueue(a, b)
    history = []; accepted = []; next_id = len(active)
    report = dict(status='running', can_apply=False, commercial_engine_used=False, source_CAD_read=False,
        source_fit=str(root.resolve()), partition=source['partition'], stl=source['stl'], config=asdict(config),
        algorithm='native-regional-graph-agglomeration-v1', original_charts=original_count,
        trial_limit=trials, maximum_union_triangles=maximum_triangles)
    dump(out/'run.json', report)
    try:
        while queue and len(history) < trials:
            _, _, a, b = heapq.heappop(queue)
            if a not in active or b not in active:
                continue
            ra, rb = active[a], active[b]; region = int(ra['region'])
            ids = np.r_[ra['triangle_ids'], rb['triangle_ids']]; used = np.unique(f[ids])
            tri = np.searchsorted(used, f[ids]); rv, rn = normals[region]
            rec, info = solve_graph(v[used], tri, rn[np.searchsorted(rv, used)], boundary_edges(tri), config)
            item = dict(a=a, b=b, region=region, triangles=len(ids), accepted=rec is not None, **info)
            history.append(item)
            if rec is None:
                continue
            rec.update(region=region, triangle_ids=ids, vertex_ids=used)
            rec.pop('uv'); rec.pop('loops'); active[next_id] = rec; assignment[ids] = next_id
            neighbors = (set(adjacent.get(a, {})) | set(adjacent.get(b, {})))-{a, b}
            adjacent[next_id] = {}
            for neighbor in neighbors:
                if neighbor not in active:
                    continue
                length = adjacent.get(a, {}).get(neighbor, 0.)+adjacent.get(b, {}).get(neighbor, 0.)
                adjacent[next_id][neighbor] = length; adjacent[neighbor][next_id] = length
                adjacent[neighbor].pop(a, None); adjacent[neighbor].pop(b, None)
            active.pop(a); active.pop(b); adjacent.pop(a, None); adjacent.pop(b, None)
            for neighbor in neighbors:
                enqueue(next_id, neighbor)
            item['merged_id'] = next_id; accepted.append(item); next_id += 1
            if len(accepted) % 25 == 0:
                print(dict(trials=len(history), merged=len(accepted), remaining=len(active),
                           seconds=time.perf_counter()-start), flush=True)
        renumber = np.full(next_id, -1, dtype=int)
        for new, (old, rec) in enumerate(sorted(active.items())):
            renumber[old] = new; np.savez_compressed(out/'charts'/f'{new:05d}.npz', **rec)
        assignment = renumber[assignment]
        region_ids = np.array([int(r['region']) for r in active.values()])
        if np.any(assignment < 0) or not np.array_equal(region_ids[assignment], labels):
            raise ValueError('Agglomeration lost coverage or crossed a supplied partition')
        np.savez_compressed(out/'chart_index.npz', face_chart=assignment, source_labels=labels)
        dump(out/'merge_trials.json', history)
        report.update(status='DiagnosticOnly', fitted_charts=len(active), accepted_merges=len(accepted),
                      attempted_merges=len(history), complete_coverage=True, partition_labels_unchanged=True,
                      actual_control_points=sum(len(r['height']) for r in active.values()))
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-start; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--trials', type=int, default=1200); p.add_argument('--maximum-triangles', type=int, default=2400)
    a = p.parse_args(); merge(a.run, a.output, a.trials, a.maximum_triangles)
