"""Transfer frozen GUI region IDs to the original STL; no fitting or relabeling.

CAD is used upstream to identify the GUI partitions. The resulting fit input
contains original STL coordinates/triangles and discrete labels only. Exact CAD
positions, normals, UVs, curves and surfaces are not exported to the fitter.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np

from .mesh import Locator, audit, dump, read_stl, stats


def transfer(generator, stl, output, baseline_audit=None, candidate_id=None):
    generator = Path(generator); output = Path(output)
    output.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    meta = json.loads((generator/'partition.json').read_text(encoding='utf-8'))
    confirmed = None
    if baseline_audit is not None:
        baseline = json.loads(Path(baseline_audit).read_text(encoding='utf-8'))
        matches = [r for r in meta['regions'] if r['gui_candidate_id'] == candidate_id]
        if len(matches) != 1 or matches[0]['face_ids'] != baseline['selected_face_ids']:
            raise ValueError('GUI candidate does not match the frozen local experiment FaceIds')
        if Path(meta['source']).resolve() != Path(baseline['source']).resolve():
            raise ValueError('GUI candidate came from a different source model')
        confirmed = dict(candidate_id=candidate_id, source_face_ids=matches[0]['face_ids'], exact_face_id_match=True,
                         baseline=str(Path(baseline_audit).resolve()))
    dtype = np.dtype([('points', '<f8', (3, 3)), ('region', '<i4'), ('face', '<i4')])
    raw = np.fromfile(generator/'triangles.bin', dtype=dtype)
    if len(raw) != meta['triangles'] or not len(raw):
        raise ValueError('Generator triangle array is incomplete')
    reference_vertices = raw['points'].reshape(-1, 3).copy()
    reference_faces = np.arange(len(reference_vertices)).reshape(-1, 3)
    locator = Locator(reference_vertices, reference_faces)
    v, f = read_stl(stl); labels = np.empty(len(f), dtype=np.int64)
    source_face = np.empty(len(f), dtype=np.int32); distances = np.empty(len(f))
    for start in range(0, len(f), 50000):
        points = v[f[start:start+50000]].mean(axis=1)
        _, dist, ids, _ = locator.closest(points)
        labels[start:start+len(points)] = raw['region'][ids]
        source_face[start:start+len(points)] = raw['face'][ids]
        distances[start:start+len(points)] = dist
    if not np.isfinite(distances).all():
        raise ValueError('Nonfinite GUI-to-STL correspondence')
    np.savez_compressed(output/'partition.npz', mesh_vertex_original=v, mesh_triangle=f,
                        face_instance_id=labels, reference_face_id=source_face)
    counts = np.bincount(labels, minlength=len(meta['regions']))
    report = dict(input_STL=str(Path(stl).resolve()), gui_generator=str(generator.resolve()),
                  generator_uses_CAD=True, fitter_reads_CAD=False,
                  partition_method='Nearest reference tessellation triangle to each original STL triangle centroid; GUI region IDs retained without smoothing, merging or quality optimization',
                  mesh_coordinates_and_triangles_unchanged=True, units='mm', topology=audit(v, f),
                  gui_candidates=meta['gui_candidates'], retained_singletons=meta['retained_singletons'],
                  gui_regions=len(counts), represented_regions=int(np.sum(counts > 0)),
                  regions_without_STL_centroids=np.flatnonzero(counts == 0).tolist(),
                  triangles_per_region=counts.tolist(), label_transfer_distance_mm=stats(distances),
                  baseline_confirmation=confirmed, seconds=time.perf_counter()-started)
    dump(output/'run.json', report)
    print(json.dumps({k: value for k, value in report.items() if k != 'triangles_per_region'}, ensure_ascii=False), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True); parser.add_argument('--stl', required=True)
    parser.add_argument('--output', required=True); parser.add_argument('--baseline-audit')
    parser.add_argument('--candidate-id', type=int)
    args = parser.parse_args(); transfer(args.generator, args.stl, args.output, args.baseline_audit, args.candidate_id)


if __name__ == '__main__':
    main()
