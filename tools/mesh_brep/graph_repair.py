"""Refine cached charts whose projected source boundaries overlap."""
from dataclasses import asdict, replace
import json
from pathlib import Path
import time
import numpy as np
from scipy import sparse
from .mesh import dump, vertex_normals
from .partition_fit import (Config, incidence, boundary_edges, parameters,
                            projected_boundary_valid, solve_graph, split_faces)


def feature_components(ids, adjacency, threshold):
    """Connected agglomerates that never reunite either side of a crease.

    Adjacency stores 2 + normal dot product (strictly positive, including
    perpendicular faces). Ordinary connected components can bypass a crease
    through a smooth path around its endpoint.
    """
    sub = sparse.triu(adjacency[ids][:, ids], k=1).tocoo()
    parent = np.arange(len(ids)); size = np.ones(len(ids), dtype=int)
    forbidden = [set() for _ in ids]
    sharp = sub.data < 2+threshold
    for a, b in zip(sub.row[sharp], sub.col[sharp]):
        forbidden[a].add(int(b)); forbidden[b].add(int(a))
    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return int(i)
    smooth = np.flatnonzero(~sharp)
    for edge in smooth[np.argsort(-sub.data[smooth], kind='stable')]:
        a, b = find(sub.row[edge]), find(sub.col[edge])
        if a == b or b in forbidden[a]:
            continue
        if size[a] < size[b]:
            a, b = b, a
        blocked = forbidden[a] | forbidden[b]
        parent[b] = a; size[a] += size[b]
        for neighbor in blocked:
            forbidden[neighbor].discard(a); forbidden[neighbor].discard(b)
            forbidden[neighbor].add(a)
        forbidden[a] = blocked; forbidden[b] = set()
    groups = np.array([find(i) for i in range(len(ids))])
    if np.any(groups[sub.row[sharp]] == groups[sub.col[sharp]]):
        raise ValueError('Feature agglomeration crossed a source crease')
    return [ids[groups == k] for k in np.unique(groups)]


def failed_step_charts(step, failed_faces, fit, assembly=None, details=None):
    """Use failed output trim edges only to select original STL chart IDs.

    The fitter still receives original mesh points and normals. Diagnostic
    STEP geometry is neither a fitting target nor a replacement partition.
    """
    from OCP.BRep import BRep_Tool
    from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE
    from . import cad
    from .mesh import Locator
    root = Path(fit); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(meta['partition']); index = np.load(root/'chart_index.npz')['face_chart']
    faces = cad.shapes(cad.read_generated(step), TopAbs_FACE); points = []
    selected, remaining = set(), list(failed_faces)
    if assembly is not None:
        from .step_identity import match_supports
        selected, remaining, report = match_supports(faces, remaining, assembly, fit)
        if details is not None:
            details.update(report)
    for fi in remaining:
        for edge in cad.shapes(faces[fi], TopAbs_EDGE):
            a, b = BRep_Tool.Range_s(edge); curve = BRep_Tool.Curve_s(edge, a, b)
            if curve is not None:
                points.extend(curve.Value(float(t)).Coord() for t in np.linspace(a, b, 9))
    if remaining and not points:
        raise ValueError('Failed STEP faces have no boundaries usable for diagnosis')
    if points:
        locator = Locator(source['mesh_vertex_original'], source['mesh_triangle'])
        _, _, ids, _ = locator.closest(np.asarray(points))
        selected.update(map(int, index[ids]))
    return sorted(selected)


def repair(run, output, refine_charts=(), boundary_scale=1., grow_neighbors=False, config=None,
           normal_cone=False, feature_split_angle=None):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    (out/'charts').mkdir(); start = time.perf_counter()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8')); cfg = config or Config(**meta['config'])
    if cfg.distance > meta['config']['distance'] or cfg.boundary > meta['config']['boundary']:
        raise ValueError('Refinement cannot relax the original geometric budgets')
    data = np.load(meta['partition']); v, f, labels = (data[k] for k in
        ('mesh_vertex_original', 'mesh_triangle', 'face_instance_id'))
    records = [dict(np.load(p)) for p in sorted((root/'charts').glob('*.npz'))]
    if not 0 < boundary_scale <= 1:
        raise ValueError('Refinement must preserve or tighten the original boundary budget')
    selected = set(int(i) for i in refine_charts)
    if any(i < 0 or i >= len(records) for i in selected):
        raise ValueError('Requested refinement chart is absent')
    if selected and grow_neighbors:
        edges, owners = incidence(f); assignment = np.load(root/'chart_index.npz')['face_chart']
        adjacent = assignment[owners]
        selected.update(adjacent[np.any(np.isin(adjacent, list(selected)), axis=1)].ravel().tolist())
    split_parts = {}
    if feature_split_angle is not None:
        if not np.isfinite(feature_split_angle) or not 0 < feature_split_angle < 180:
            raise ValueError('Feature split angle must lie between 0 and 180 degrees')
        edges, owners = incidence(f)
        fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
        fn /= np.linalg.norm(fn, axis=1)[:, None]
        similarity = 2+np.sum(fn[owners[:, 0]]*fn[owners[:, 1]], axis=1)
        adjacency = sparse.csr_matrix((np.repeat(similarity, 2), (owners.ravel(), owners[:, ::-1].ravel())),
                                      shape=(len(f), len(f)))
        for i, rec in enumerate(records):
            if selected and i not in selected:
                continue
            ids = rec['triangle_ids']
            parts = feature_components(ids, adjacency, np.cos(np.radians(feature_split_angle)))
            if len(parts) > 1:
                split_parts[i] = parts
    bad = []
    for i, rec in enumerate(records):
        used = rec['vertex_ids']; tri = np.searchsorted(used, f[rec['triangle_ids']])
        if i in selected or i in split_parts or not projected_boundary_valid(parameters(v[used], rec), boundary_edges(tri)):
            bad.append(i)
    report = dict(status='running', can_apply=False, algorithm='native-graph-projection-refinement-v1',
        source_fit=str(root.resolve()), stl=meta['stl'], partition=meta['partition'], config=asdict(cfg),
        commercial_engine_used=False, source_CAD_read=False, original_charts=len(records), refined_charts=bad,
        requested_charts=list(refine_charts), neighbor_refinement=grow_neighbors, boundary_scale=boundary_scale)
    report['normal_cone_refinement'] = normal_cone
    report.update(feature_split_angle_degrees=feature_split_angle,
                  feature_split_method=('connected_agglomeration_with_crease_cannot_links'
                                        if feature_split_angle is not None else None),
                  feature_split_source_charts=sorted(split_parts),
                  feature_split_components=sum(len(parts) for parts in split_parts.values()))
    dump(out/'run.json', report)
    try:
        repaired = []; events = []
        if bad:
            edges, owners = incidence(f)
            corners = None
            if normal_cone:
                from .source_normals import crease_fan_normals
                corners = crease_fan_normals(v, f, cfg.feature_angle, (edges, owners))
            graph = sparse.coo_matrix((np.ones(owners.size), (owners.ravel(), owners[:, ::-1].ravel())),
                                      shape=(len(f), len(f))).tocsr()
            shared = vertex_normals(v, f) if cfg.shared_smooth_normals else None
            sharp = np.array([], dtype=int)
            if shared is not None:
                fn = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
                fn /= np.linalg.norm(fn, axis=1)[:, None]
                sharp = np.unique(edges[np.sum(fn[owners[:, 0]]*fn[owners[:, 1]], axis=1)
                                        < np.cos(np.radians(cfg.feature_angle))])
            normals = {}
            for region in {int(records[i]['region']) for i in bad}:
                tri = f[labels == region]; used = np.unique(tri)
                nn = vertex_normals(v[used], np.searchsorted(used, tri))
                if shared is not None:
                    smooth = ~np.isin(used, sharp); nn[smooth] = shared[used[smooth]]
                normals[region] = used, nn
        for i, rec in enumerate(records):
            if i not in bad:
                repaired.append(rec); continue
            region = int(rec['region']); used_normal, normal = normals[region]
            local_config = replace(cfg, boundary=min(cfg.boundary*boundary_scale,
                                   float(rec.get('boundary_budget_mm', cfg.boundary))))
            queue = [(part, 0) for part in split_parts.get(i, [rec['triangle_ids']])]
            while queue:
                ids, depth = queue.pop(0); used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids])
                fitted, result = solve_graph(v[used], tri,
                    corners[ids] if normal_cone else normal[np.searchsorted(used_normal, used)],
                    boundary_edges(tri), local_config, condition_frame=normal_cone)
                events.append(dict(source_chart=i, region=region, triangles=len(ids), depth=depth, **result))
                if fitted is None:
                    if len(ids) <= 1 or depth >= cfg.max_depth:
                        raise ValueError(f'Cannot refine chart {i}: {result}')
                    queue.extend((part, depth+1) for part in split_faces(v, f, ids, graph))
                else:
                    fitted.pop('uv'); fitted.pop('loops')
                    fitted.update(region=region, triangle_ids=ids, vertex_ids=used,
                                  boundary_budget_mm=local_config.boundary); repaired.append(fitted)
        assignment = np.full(len(f), -1, dtype=int)
        for i, rec in enumerate(repaired):
            ids = rec['triangle_ids']
            if np.any(assignment[ids] >= 0) or np.any(labels[ids] != rec['region']):
                raise ValueError('Refinement crossed supplied regions or duplicated source triangles')
            assignment[ids] = i; np.savez_compressed(out/'charts'/f'{i:05d}.npz', **rec)
        if np.any(assignment < 0):
            raise ValueError('Refinement lost source coverage')
        np.savez_compressed(out/'chart_index.npz', face_chart=assignment, source_labels=labels)
        dump(out/'refinement.json', events)
        report.update(status='DiagnosticOnly', fitted_charts=len(repaired), complete_coverage=True,
                      partition_labels_unchanged=True, actual_control_points=sum(len(r['height']) for r in repaired))
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-start; dump(out/'run.json', report)
    return report
