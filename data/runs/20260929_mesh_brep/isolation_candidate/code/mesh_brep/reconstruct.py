"""STL + discrete layout -> mesh UV -> shared XYZ spline network -> real STEP."""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from .mesh import read_stl, charts, audit, dump
from .splines import fit_mesh
from . import cad


def reconstruct(stl, layout, out, controls=29, samples=24000, seed=2909179):
    start = time.perf_counter(); out = Path(out); out.mkdir(parents=True, exist_ok=False)
    vertices, triangles = read_stl(stl); metadata = json.loads(Path(layout).read_text(encoding='utf-8'))
    allowed = {'schema', 'units', 'triangle_labels', 'patches', 'smooth_seams', 'input_kind',
               'tolerance_mm', 'seam_tolerance_mm', 'construction_tolerance_mm'}
    if set(metadata)-allowed or metadata['units'] != 'mm':
        raise ValueError('Input contract rejects undeclared fields or unsupported units')
    if not metadata.get('smooth_seams', False):
        raise ValueError('This prototype requires smooth seams; split feature normals before fitting')
    topo = audit(vertices, triangles)
    if any(topo[k] for k in ['nonmanifold_edges', 'inconsistent_edges', 'degenerate_triangles']) or topo['components'] != 1:
        raise ValueError(f'Invalid input topology: {topo}')
    read_seconds = time.perf_counter()-start
    started = time.perf_counter(); chart = charts(vertices, triangles, metadata); uv_seconds = time.perf_counter()-started
    poles, knots, uses, solve = fit_mesh(vertices, chart, controls, samples, seed)
    started = time.perf_counter()
    shape = cad.construct(poles, knots, [c['distortion']['orientation'] for c in chart], topo['boundary_edges'] == 0,
                          metadata['construction_tolerance_mm'])
    brep_seconds = time.perf_counter()-started
    started = time.perf_counter(); cad.write_step(out/'network.step', shape); write_seconds = time.perf_counter()-started
    started = time.perf_counter(); reread = cad.read_generated(out/'network.step'); reread_seconds = time.perf_counter()-started
    started = time.perf_counter(); topology = cad.topology(reread); checks = cad.edge_checks(reread); cost = cad.expression(reread)
    cad_seconds = time.perf_counter()-started
    for i, (p, (tu, tv), c) in enumerate(zip(poles, knots, chart)):
        np.savez_compressed(out/f'patch_{i}.npz', poles=p, tu=tu, tv=tv,
                            vertex_uv=c['uv'], triangle_ids=c['triangle_ids'])
    timing = dict(stl_read_index_audit=read_seconds, uv_solve=uv_seconds,
                  correspondence=solve['correspondence_and_shared_map_seconds'],
                  matrix_assembly=solve['matrix_assembly_seconds'], solve=solve['solve_seconds'],
                  brep_construct=brep_seconds, step_write=write_seconds, step_read=reread_seconds, cad_checks=cad_seconds)
    report = dict(status='DiagnosticOnly', can_apply=False, mode='given-partition pure mesh reconstruction',
                  inputs=[str(Path(stl).resolve()), str(Path(layout).resolve())], input_topology=topo,
                  controls=controls, samples=samples, seed=seed, solver=solve, topology=topology, cad_checks=checks,
                  expression=cost, uv=[dict(topology=c['audit'], distortion=c['distortion'], residual=c['residual']) for c in chart],
                  timing_seconds=timing, wall_seconds=time.perf_counter()-start,
                  gates_pending='Independent geometry, normals, within-face regularity, seams and external CAD',
                  tolerances={k: metadata[k] for k in ['tolerance_mm', 'seam_tolerance_mm', 'construction_tolerance_mm']})
    report['timing_seconds']['bookkeeping'] = report['wall_seconds']-sum(timing.values())
    dump(out/'reconstruction.json', report)
    return report


def main():
    p = argparse.ArgumentParser(); p.add_argument('--stl', required=True); p.add_argument('--layout', required=True)
    p.add_argument('--output', required=True); p.add_argument('--controls', type=int, default=29)
    p.add_argument('--samples', type=int, default=24000); p.add_argument('--seed', type=int, default=2909179)
    a = p.parse_args(); report = reconstruct(a.stl, a.layout, a.output, a.controls, a.samples, a.seed)
    print(json.dumps({k: report[k] for k in ['status', 'expression', 'topology', 'timing_seconds', 'wall_seconds']}), flush=True)


if __name__ == '__main__':
    main()
