"""Rebuild cached native height charts using canonical topological identities."""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump
from .partition_fit import incidence, boundary_edges, loops
from .partition_cad import assemble


def reconstruct(run, output, segment_edges=False, smooth_boundary=0., skip_smooth_faces=(), write_preview=True,
                smooth_segments=False, maximum_edge_length=None, curve_certificates=None,
                native_context=None, binary_native=False, measure_volume=True, segment_charts=()):
    root = Path(run); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter(); source = json.loads((root/'run.json').read_text(encoding='utf-8'))
    report = dict(status='running', can_apply=False, commercial_engine_used=False,
        partition=source['partition'], stl=source['stl'], source_fit=str(root.resolve()),
        algorithm='native-graph-canonical-topology-v1', boundary_tolerance_mm=source['config']['boundary'],
        segment_edges=segment_edges, boundary_smoothing_budget_mm=smooth_boundary,
        smoothing_disabled_charts=list(skip_smooth_faces), smooth_segments=smooth_segments)
    report['source_segmented_charts'] = sorted(map(int, segment_charts))
    dump(out/'run.json', report)
    try:
        if source.get('requires_support_refit'):
            raise ValueError('Changed shared curves require incident-support validation before CAD assembly')
        z = np.load(source['partition']); v, f = z['mesh_vertex_original'], z['mesh_triangle']
        assignment = np.load(root/'chart_index.npz')['face_chart']; records = []
        for path in sorted((root/'charts').glob('*.npz')):
            rec = dict(np.load(path)); used = rec['vertex_ids']; tri = np.searchsorted(used, f[rec['triangle_ids']])
            rec['loops'] = [used[loop] for loop in loops(boundary_edges(tri))]; records.append(rec)
        edges, owners = incidence(f)
        curve_files = sorted((root/'curves').glob('*.npz'))
        curve_records = [dict(np.load(p)) for p in curve_files] if curve_files else None
        report.update(assemble(v, f, records, assignment, edges, owners, source['config']['boundary'], out,
                               identity_topology=True, segment_edges=segment_edges,
                               smooth_boundary=smooth_boundary, skip_smooth_faces=skip_smooth_faces,
                               write_preview=write_preview, smooth_segments=smooth_segments,
                               shared_curve_records=curve_records, maximum_edge_length=maximum_edge_length,
                               curve_certificates=curve_certificates, native_context=native_context,
                               binary_native=binary_native, measure_volume=measure_volume,
                               segment_charts=segment_charts), status='DiagnosticOnly')
        report['fitted_shared_skeleton'] = curve_records is not None
        report['maximum_edge_length_mm'] = maximum_edge_length
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        report['seconds'] = time.perf_counter()-started; dump(out/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    p.add_argument('--segment-edges', action='store_true')
    p.add_argument('--smooth-boundary', type=float, default=0.)
    a = p.parse_args(); reconstruct(a.run, a.output, a.segment_edges, a.smooth_boundary)
