"""Bounded overlapping-neighborhood optimization across the whole model.

All smooth-source seams participate. Shared supports are atomic, neighbors
outside the active block are fixed targets, and later sweeps revisit block
interfaces. This keeps the joint sparse systems bounded without region IDs.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import asdict
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .shared_support import source_angles, restore_groups
from .source_normals import crease_fan_normals
from .surface_jets import jets, seam_sampling
from .seam_fairing import Config, optimize as optimize_block


_STATIC = None
OBJECTIVE_VERSION = 'fixed-source-and-mean-normal-v1'


def default_config():
    return Config(normal_weight=.3,seam_weight=.2,curvature_weight=.0005,
                  variation_weight=1e-6,iterations=2,solver_seconds=45.,normal_slack_degrees=.5,
                  boundary_source_weight=.2)


def _initialize(context):
    global _STATIC
    _STATIC = context


def _block(task):
    source, path, ids, records, config = task
    context = dict(_STATIC, records=records)
    report = optimize_block(source, path, config=config, chart_ids=ids, context=context)
    checkpoint = Path(path)/'accepted_charts'; checkpoint.mkdir()
    for di in report['changed_charts']:
        np.savez_compressed(checkpoint/f'{di:05d}.npz', **context['records'][di])
    report['block_directory'] = str(Path(path).resolve())
    return report


def independent_batches(blocks, curves):
    """Color block adjacency: simultaneous blocks never share a seam."""
    owner = {di:i for i, ids in enumerate(blocks) for di in ids}
    neighbors = {i:set() for i in range(len(blocks))}
    for c in curves.values():
        a, b = (owner.get(int(di)) for di in c['owners'])
        if a is not None and b is not None and a != b:
            neighbors[a].add(b); neighbors[b].add(a)
    colors = {}
    for i in sorted(neighbors, key=lambda i:(-len(neighbors[i]), i)):
        used = {colors[j] for j in neighbors[i] if j in colors}; color = 0
        while color in used:
            color += 1
        colors[i] = color
    return [sorted(i for i,c in colors.items() if c == color) for color in sorted(set(colors.values()))]


def neighborhoods(records, curves, lookup, max_variables=12000, max_charts=32, reverse=False, smooth_degrees=10., selected_charts=None):
    groups = {}; group_of = {}
    for di, record in records.items():
        ids = tuple(map(int, record.get('shared_support_group', [di])))
        key = min(ids); groups[key] = ids
        for i in ids:
            group_of[i] = key
    graph = {key: set() for key in groups}; scores = {key: 0. for key in groups}; eligible = set()
    for c in curves.values():
        pair = tuple(map(int, c['owners'])); a, b = (group_of[i] for i in pair)
        _, xyz, weight, smooth = seam_sampling(c, lookup, count=17, smooth_degrees=smooth_degrees)
        if not smooth.any() or a == b:
            continue
        nl, sl = jets(records[pair[0]], xyz[smooth]); nr, sr = jets(records[pair[1]], xyz[smooth])
        residual = np.linalg.norm(nl-nr, axis=1)+.05*np.linalg.norm(sl-sr, axis=(1, 2))
        score = float(np.sqrt(np.average(residual**2, weights=weight[smooth])))
        scores[a] = max(scores[a], score); scores[b] = max(scores[b], score)
        graph[a].add(b); graph[b].add(a); eligible.update((a, b))
    all_eligible = set(eligible)
    if selected_charts is not None:
        eligible.intersection_update(group_of[i] for i in selected_charts)
    pending = set(eligible); blocks = []
    costs = {key: sum(len(records[i]['height']) for i in ids) for key, ids in groups.items()}
    while pending:
        seed = min(pending, key=lambda key: (scores[key] if reverse else -scores[key], key))
        active = {seed}; pending.remove(seed); frontier = graph[seed] & pending
        variables = costs[seed]; count = len(groups[seed])
        while pending:
            available = {key for key in pending if variables+costs[key] <= max_variables and count+len(groups[key]) <= max_charts}
            if not available:
                break
            candidates = frontier & available
            key = min(candidates or available, key=lambda i: (-scores[i], i)); frontier.discard(key)
            if variables+costs[key] > max_variables or count+len(groups[key]) > max_charts:
                continue
            active.add(key); pending.remove(key); variables += costs[key]; count += len(groups[key])
            frontier.update(graph[key] & pending)
        blocks.append(sorted(i for key in active for i in groups[key]))
    return blocks, dict(eligible_charts=sum(len(groups[k]) for k in eligible),
                        excluded_shared_or_crease_only_charts=sorted(set(records)-{i for k in all_eligible for i in groups[k]}),
                        outside_requested_charts=sorted(i for k in all_eligible-eligible for i in groups[k]))


def optimize(source, output, config=None, sweeps=2, max_variables=12000, max_charts=32, resume=False, workers=4, selected_charts=None, reference_fit=None, seconds=None):
    start = time.perf_counter()
    if sweeps < 1 or max_variables < 1 or max_charts < 2 or workers < 1:
        raise ValueError('Invalid neighborhood limits')
    if seconds is not None and (not np.isfinite(seconds) or seconds <= 0):
        raise ValueError('Invalid whole continuity time budget')
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=resume)
    cfg = config or default_config()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if meta.get('requires_support_refit'):
        raise ValueError('Moved network must be refitted first')
    data = np.load(meta['partition']); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = {int(p.stem): dict(np.load(p)) for p in (root/'curves').glob('*.npz')}
    restore_groups(records)
    references=dict(records) if reference_fit is None else {int(p.stem):dict(np.load(p)) for p in (Path(reference_fit)/'charts').glob('*.npz')}
    if any(i not in references or not np.array_equal(r['triangle_ids'],references[i]['triangle_ids']) for i,r in records.items()):
        raise ValueError('Interior-fidelity reference has different source ownership')
    context = dict(meta=meta, vertices=v, triangles=f, records=records, curves=curves, lookup=source_angles(v, f),
                   normals=crease_fan_normals(v, f, meta['config']['feature_angle'], filter_steps=meta['normal_filter_steps']),
                   reference_records=references)
    events = []; attempted = set(); accepted = set(); sweep_reports = []; budget_exhausted = False
    progress_path = out/'progress.json'
    if resume and progress_path.exists():
        checkpoint = json.loads(progress_path.read_text(encoding='utf-8'))
        if (checkpoint['source_fit'] != str(root.resolve()) or checkpoint['config'] != asdict(cfg)
                or checkpoint.get('objective_version') != OBJECTIVE_VERSION
                or checkpoint.get('reference_fit') != (None if reference_fit is None else str(Path(reference_fit).resolve()))
                or checkpoint.get('selected_charts') != (None if selected_charts is None else sorted(selected_charts))):
            raise ValueError('Continuity checkpoint has a different input or objective')
        events, sweep_reports = checkpoint['events'], checkpoint['sweeps']
        for event in events:
            attempted.update(event['charts']); accepted.update(event['changed_charts'])
            for di in event['changed_charts']:
                path = Path(event['directory'])/'accepted_charts'/f'{di:05d}.npz'
                record = dict(np.load(path)); reference = context['records'][di]
                if any(not np.array_equal(record[k], reference[k]) for k in ('region', 'vertex_ids', 'triangle_ids')):
                    raise ValueError('Continuity checkpoint changed source ownership')
                context['records'][di] = record
    def checkpoint_progress():
        temporary = out/'progress.next.json'
        dump(temporary, dict(source_fit=str(root.resolve()), config=asdict(cfg), events=events, sweeps=sweep_reports,
                             objective_version=OBJECTIVE_VERSION,
                             reference_fit=None if reference_fit is None else str(Path(reference_fit).resolve()),
                             selected_charts=None if selected_charts is None else sorted(selected_charts)))
        temporary.replace(progress_path)
    static = {k:v for k,v in context.items() if k != 'records'}
    executor = ProcessPoolExecutor(max_workers=workers, initializer=_initialize, initargs=(static,)) if workers > 1 and seconds is None else None
    pool = None
    if seconds is not None and time.perf_counter()-start < seconds:
        import multiprocessing as mp
        pool = mp.Pool(workers, initializer=_initialize, initargs=(static,))
    if executor is None:
        _initialize(static)
    try:
        for sweep in range(sweeps):
            if seconds is not None and time.perf_counter()-start >= seconds:
                budget_exhausted = True
                break
            saved = next((item for item in sweep_reports if item['sweep'] == sweep), None)
            if saved is None:
                blocks, coverage = neighborhoods(context['records'], curves, context['lookup'], max_variables, max_charts, reverse=bool(sweep % 2), smooth_degrees=cfg.smooth_degrees, selected_charts=selected_charts)
                sweep_reports.append(dict(sweep=sweep, blocks=blocks, **coverage)); checkpoint_progress()
            else:
                blocks = saved['blocks']; coverage = {k:v for k,v in saved.items() if k not in ('sweep', 'blocks')}
            print(dict(stage='global_continuity', sweep=sweep, blocks=len(blocks), **coverage), flush=True)
            for batch in independent_batches(blocks, curves):
                if seconds is not None and time.perf_counter()-start >= seconds:
                    budget_exhausted = True
                    break
                tasks = []
                for index in batch:
                    if any(event['sweep'] == sweep and event['block'] == index for event in events):
                        continue
                    ids = blocks[index]; selected = set(ids); needed = set(ids)
                    for c in curves.values():
                        if selected.intersection(map(int, c['owners'])):
                            needed.update(map(int, c['owners']))
                    path = out/'blocks'/f'pass_{sweep:02d}'/f'block_{index:03d}'; retry = 0
                    while path.exists():
                        retry += 1
                        path = out/'blocks'/f'pass_{sweep:02d}'/f'block_{index:03d}_retry_{retry:02d}'
                    task = (root, path, ids, {di:context['records'][di] for di in needed}, cfg)
                    tasks.append((index, path, task))
                pending = {executor.submit(_block, task):(index,path) for index,path,task in tasks} if executor else None
                if pool is not None:
                    queued = pool.imap_unordered(_block, [t for _,_,t in tasks], chunksize=1)
                    by_path = {str(p.resolve()): (i,p) for i,p,_ in tasks}
                    def timed_results():
                        nonlocal budget_exhausted
                        for _ in tasks:
                            remaining = seconds-(time.perf_counter()-start)
                            if remaining <= 0:
                                budget_exhausted = True
                                break
                            try:
                                item = queued.next(timeout=remaining)
                            except mp.TimeoutError:
                                budget_exhausted = True
                                break
                            i,p = by_path[item['block_directory']]
                            yield i,p,item
                    results = timed_results()
                else:
                    results = ((pending[future][0], pending[future][1], future.result()) for future in as_completed(pending)) if executor else ((i,p,_block(t)) for i,p,t in tasks)
                for index, path, report in results:
                    ids = blocks[index]
                    for di in report['changed_charts']:
                        context['records'][di] = dict(np.load(path/'accepted_charts'/f'{di:05d}.npz'))
                    attempted.update(ids); accepted.update(report['changed_charts'])
                    events.append(dict(sweep=sweep, block=index, charts=ids, accepted=report['accepted'],
                                       changed_charts=report['changed_charts'], before=report['before'], after=report['after'],
                                       seconds=report['seconds'], directory=str(path.resolve())))
                    print(dict(stage='continuity_block', sweep=sweep, block=index, total=len(blocks),
                               accepted=report['accepted'], changed=len(report['changed_charts'])), flush=True)
                    checkpoint_progress()
                if budget_exhausted:
                    break
            if budget_exhausted:
                break
    finally:
        if pool is not None:
            pool.terminate(); pool.join()
        if executor is not None:
            executor.shutdown(wait=True, cancel_futures=True)
    for name in ('charts', 'curves', 'routes'):
        if (root/name).exists():
            shutil.copytree(root/name, out/name, dirs_exist_ok=resume)
    for name in ('chart_index.npz', 'fitting.json'):
        shutil.copy2(root/name, out/name)
    for di in sorted(accepted):
        np.savez_compressed(out/'charts'/f'{di:05d}.npz', **context['records'][di])
    report = dict(config=asdict(cfg), sweeps=sweep_reports, events=events, total_charts=len(records), source_fit=str(root.resolve()),
                  objective_version=OBJECTIVE_VERSION,
                  attempted_charts=sorted(attempted), changed_charts=sorted(accepted),
                  changed_regions=sorted({int(records[i]['region']) for i in accepted}),
                  all_regions_considered=selected_charts is None and bool(sweep_reports), shared_curves_unchanged=True, source_CAD_read=False,
                  reference_fit=None if reference_fit is None else str(Path(reference_fit).resolve()),
                  selected_charts=None if selected_charts is None else sorted(selected_charts),
                  commercial_engine_used=False, seconds=time.perf_counter()-start,
                  time_budget_seconds=seconds, budget_exhausted=budget_exhausted,
                  whole_model_sweep_completed=not budget_exhausted,
                  unfinished_charts=sorted((set(i for row in sweep_reports for block in row['blocks'] for i in block)
                                            if sweep_reports else set(records))-attempted),
                  scope='Bounded block Gauss-Newton fairing of normals and shape operators; independent STEP distance and dense continuity audits remain required.')
    dump(out/'global_continuity.json', report)
    meta.update(source_fit=str(out.resolve()), global_continuity=report, can_apply=False)
    dump(out/'run.json', meta)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--sweeps', type=int, default=2)
    parser.add_argument('--variables', type=int, default=12000)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--workers', type=int, default=4)
    args = parser.parse_args(); optimize(args.run, args.output, sweeps=args.sweeps, max_variables=args.variables, resume=args.resume, workers=args.workers)
