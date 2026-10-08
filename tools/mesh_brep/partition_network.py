"""Topology-checked simplification of an indexed, fixed partition network.

Region wedges share positions; meshoptimizer collapses both sides of a seam
together. One original triangle per region is anchored. An output is accepted
only after closed oriented manifold and region-coverage checks. Failed native
collapses are retried with their original one-ring locked, never repaired by
deleting faces or changing labels.
"""
import ctypes
import time

import numpy as np

from .mesh import audit, edges


def simplify(v, f, labels, error, target_faces, retries=12, anchors='triangle'):
    import meshoptimizer as mo
    from meshoptimizer.simplifier import lib
    # The 0.2.30a0 Python wheel leaves this exported function untyped.
    fn = lib.meshopt_simplifyWithAttributes
    if fn.argtypes is None:
        fn.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                      ctypes.c_size_t, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                      ctypes.c_size_t, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                      ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                      ctypes.POINTER(ctypes.c_ubyte), ctypes.c_size_t, ctypes.c_float,
                      ctypes.c_uint, ctypes.POINTER(ctypes.c_float)]
        fn.restype = ctypes.c_size_t
    started = time.perf_counter()
    from .partition_fit import incidence
    from .intersections import self_intersections
    source_edges, source_owners = incidence(f)
    source_pair = np.sort(labels[source_owners], axis=1)
    original_adjacency = set(map(tuple, np.unique(source_pair[source_pair[:,0] != source_pair[:,1]], axis=0)))
    wedges, inverse = np.unique(np.c_[f.ravel(), np.repeat(labels, 3)], axis=0, return_inverse=True)
    positions = np.ascontiguousarray(v[wedges[:, 0]], dtype=np.float32)
    indices = np.asarray(inverse, dtype=np.uint32); dest = np.empty_like(indices)
    cross = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
    area = np.linalg.norm(cross, axis=1)
    locked = np.zeros(len(v), dtype=np.uint8)
    if anchors not in {'triangle', 'junction', 'vertex'}:
        raise ValueError(f'Unknown anchor policy: {anchors}')
    if anchors == 'junction':
        boundary = source_edges[source_pair[:, 0] != source_pair[:, 1]]
        degree = np.bincount(boundary.ravel(), minlength=len(v))
        locked[(degree != 0) & (degree != 2)] = 1
    else:
        for label in np.unique(labels):
            ids = np.flatnonzero(labels == label)
            seed = f[ids[np.argmax(area[ids])]]
            locked[seed if anchors == 'triangle' else seed[:1]] = 1
    attributes = np.zeros((len(wedges), 1), np.float32)
    attempts = []
    for attempt in range(retries):
        reported = np.zeros(1, np.float32)
        count = mo.simplify_with_attributes(
            dest, indices, positions, attributes, np.zeros(1, np.float32),
            vertex_lock=locked[wedges[:, 0]], target_index_count=3*target_faces,
            target_error=error, options=mo.SIMPLIFY_ERROR_ABSOLUTE, result_error=reported)
        out = dest[:count].reshape(-1, 3); region = wedges[out, 1]
        if not np.all(region == region[:, :1]):
            raise ValueError('Native simplifier crossed a fixed region boundary')
        raw_faces = wedges[out, 0]
        used, remap = np.unique(raw_faces, return_inverse=True)
        points, triangles = v[used], remap.reshape(-1, 3)
        check = audit(points, triangles)
        missing = np.setdiff1d(labels, region[:, 0])
        added_pairs = set(); lost_pairs = set(); bad_adjacency_vertices = []
        manifold = all(check[k] == 0 for k in ('boundary_edges', 'nonmanifold_edges', 'inconsistent_edges'))
        if manifold:
            current_edges, current_owners = incidence(raw_faces)
            current_pair = np.sort(region[current_owners, 0], axis=1)
            adjacency = set(map(tuple, np.unique(current_pair[current_pair[:,0] != current_pair[:,1]], axis=0)))
            added_pairs, lost_pairs = adjacency-original_adjacency, original_adjacency-adjacency
            for pair in added_pairs:
                bad_adjacency_vertices.extend(current_edges[np.all(current_pair == pair, axis=1)].ravel())
            for pair in lost_pairs:
                bad_adjacency_vertices.extend(source_edges[np.all(source_pair == pair, axis=1)].ravel())
        intersection_faces = self_intersections(points,triangles) if manifold and not check['degenerate_triangles'] else np.array([],dtype=int)
        attempts.append(dict(attempt=attempt, locked_vertices=int(locked.sum()),
                             native_error_mm=float(reported[0]), topology=check,
                             missing_regions=missing.tolist(),
                             self_intersecting_faces=int(len(intersection_faces)),
                             added_adjacencies=[[int(x) for x in p] for p in sorted(added_pairs)],
                             lost_adjacencies=[[int(x) for x in p] for p in sorted(lost_pairs)]))
        if not len(missing) and not len(intersection_faces) and not added_pairs and not lost_pairs and all(check[k] == 0 for k in
                ('boundary_edges', 'nonmanifold_edges', 'inconsistent_edges', 'degenerate_triangles')):
            return points, triangles, region[:, 0], used, dict(
                seconds=time.perf_counter()-started, attempts=attempts,
                input_faces=len(f), region_count=len(np.unique(labels)),
                region_adjacency_count=len(original_adjacency), region_adjacency_preserved=True,
                requested_error_mm=error, requested_faces=target_faces, anchor_policy=anchors)
        e, counts, direction = edges(raw_faces)
        bad = list(e[(counts != 2) | (direction != 0)].ravel())
        bad.extend(bad_adjacency_vertices)
        bad.extend(raw_faces[intersection_faces].ravel())
        size = np.linalg.norm(np.cross(points[triangles[:, 1]]-points[triangles[:, 0]],
                                      points[triangles[:, 2]]-points[triangles[:, 0]]), axis=1)
        bad.extend(raw_faces[size < 2e-15].ravel())
        if len(missing):
            bad.extend(f[np.isin(labels, missing)].ravel())
        incident = np.isin(f, bad).any(axis=1)
        before = int(locked.sum()); locked[np.unique(f[incident])] = 1
        if int(locked.sum()) == before:
            raise ValueError(f'No safe simplification progress: {attempts[-1]}')
    raise ValueError(f'Simplification exceeded topology retry budget: {attempts}')
