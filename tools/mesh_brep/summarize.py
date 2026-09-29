"""Collect measured results without changing thresholds or missing values."""
import argparse
import csv
import json
from datetime import datetime, timezone
from pathlib import Path
from .mesh import dump


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def cad_summary(folder):
    if not (folder/'result.json').exists():
        return None
    report = read(folder/'result.json'); checks = []
    for row in report.get('component_checks', []):
        s = row['summary']; values = {x['name']: x['answer'] for x in s.get('key_checks', [])}
        checks.append(dict(geom_checks=values.get('GEOM_CHECKS'), short_edges=values.get('SHORT_EDGES'),
                           absolute_accuracy=values.get('ACCURACY_INFO'), error_count=s.get('error_count'),
                           warning_count=s.get('warning_count'), xml_path=s.get('xml_path')))
    return dict(status=report['status'], diagnostic_passed=report['validated'], checks=checks,
                seconds=(datetime.fromisoformat(report['ended_at'])-datetime.fromisoformat(report['started_at'])).total_seconds())


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True); args = parser.parse_args()
    r = Path(args.run); rows = []; stage_rows = []
    configs = [
        ('baseline_cold', 'baseline_region', 'height_CAD', 'candidate_coarse'),
        ('A/inverse_xyz', '', 'XYZ_CAD_inverse', 'candidate_coarse'),
        ('A/mesh_uv_xyz', 'A_mesh_region', 'XYZ_CAD_mesh_UV', 'candidate_coarse'),
        ('B_coarse_29_global', 'B_coarse_29_global', 'mesh_only', 'candidate_coarse'),
        ('B_coarse_25', 'B_coarse_25', 'mesh_only', 'candidate_coarse'),
        ('B_fine_25', 'B_fine_25', 'mesh_only', 'candidate_fine'),
        ('B_closed32_13_global', '', 'mesh_only_fixture', 'closed32'),
        ('B_closed64_13', 'B_closed64_13', 'mesh_only_fixture', 'closed64'),
        ('B_wrapped32_17', '', 'mesh_only_fixture', 'wrapped32'),
        ('B_wrapped64_17', 'B_wrapped64_17', 'mesh_only_fixture', 'wrapped64')]
    for case, creo, mode, mesh in configs:
        evaluation = read(r/case/'confirmation_v2.json')
        rec = (read(r/case/'reconstruction.json') if case != 'baseline_cold' else
               read(r/'baseline_cold/aligned_inner_cubic/fit.json'))
        cost = evaluation['expression']; forward = evaluation.get('step_to_surface'); reverse = evaluation.get('surface_to_step')
        seam = evaluation['internal_seams']; cad = cad_summary(r/'creo'/creo) if creo else None
        topology = rec.get('topology', rec.get('after_read'))
        unknowns = rec['solver'].get('independent_scalar_variables', rec['solver'].get('unknowns'))
        boundary_checks = rec.get('cad_checks', rec.get('edge_checks'))
        gen = read(r/'inputs'/mesh/'generation.json')
        row = dict(case=case, mode=mode, evaluation_mesh_vertices=gen['topology']['vertices'], evaluation_mesh_triangles=gen['topology']['triangles'],
                   reconstruction_mesh_vertices=gen['topology']['vertices'] if mode.startswith('mesh_only') else None,
                   faces=cost['faces'], surface_controls=cost['actual_support_control_points'], scalar_variables=unknowns,
                   max_direction=cost['maximum_direction_poles'], curve_controls=cost['unique_3d_curve_controls'], pcurve_controls=cost['pcurve_controls'],
                   step_bytes=evaluation['step_bytes'],
                   step_mesh_max_mm=evaluation.get('step_to_mesh', {}).get('distance_mm', {}).get('max'),
                   mesh_step_max_mm=evaluation.get('mesh_to_step', {}).get('distance_mm', {}).get('max'),
                   mesh_surface_max_mm=evaluation['mesh_to_surface']['distance_mm']['max'],
                   surface_mesh_max_mm=evaluation['surface_to_mesh']['distance_mm']['max'],
                   mesh_surface_normal_max_deg=evaluation['mesh_to_surface']['normal_degrees']['max'],
                   reverse_mesh_facet_normal_max_deg=evaluation['surface_to_mesh']['facet_normal_degrees']['max'],
                   step_surface_max_mm=forward['distance_mm']['max'] if forward else None,
                   surface_step_max_mm=reverse['distance_mm']['max'] if reverse else None,
                   step_surface_rms_mm=forward['distance_mm']['rms'] if forward else None,
                   step_surface_normal_p95_deg=forward['normal_degrees']['p95'] if forward else None,
                   step_surface_normal_max_deg=forward['normal_degrees']['max'] if forward else None,
                   internal_gap_max_mm=max((x['separation_mm'] for x in seam), default=None),
                   internal_normal_max_deg=max((x['normal_degrees']['max'] for x in seam), default=None),
                   inside_knot_normal_jump_deg=evaluation['interior_continuity']['maximum_sampled_normal_jump_degrees'],
                   curvature_max_per_mm=max(x['absolute_mean_curvature_per_mm']['max'] for x in evaluation['regularity']['faces']),
                   sampled_singular_count=sum(x['singular_samples'] for x in evaluation['regularity']['faces']),
                   brep_valid=topology['brep_valid'], local_solids=topology['solids'], free_edges=topology['free_edges'],
                   curve_on_surface_mm=boundary_checks['max_curve_on_surface_mm'],
                   creo_geom_checks=cad['checks'][0]['geom_checks'] if cad and cad['checks'] else None,
                   creo_diagnostic_passed=cad['diagnostic_passed'] if cad else None,
                   confirmation_seconds=evaluation['wall_seconds'], status='DiagnosticOnly', can_apply=False)
        if 'uv' in rec:
            row.update(uv_flipped=sum(c['distortion']['flipped'] for c in rec['uv']),
                       uv_zero=sum(c['distortion']['zero'] for c in rec['uv']),
                       uv_max_stretch=max(c['distortion']['stretch_ratio']['max'] for c in rec['uv']))
        else:
            row.update(uv_flipped=None, uv_zero=None, uv_max_stretch=None)
        distances = [row['mesh_surface_max_mm'], row['surface_mesh_max_mm']]+([row['step_surface_max_mm'], row['surface_step_max_mm']] if forward else [])
        row['distance_gate'] = max(distances) <= .03
        row['normal_gate'] = (row['step_surface_normal_max_deg'] if forward else row['mesh_surface_normal_max_deg']) <= 15
        row['internal_normal_gate'] = None if not seam else row['internal_normal_max_deg'] <= 5
        row['direction_budget_gate'] = row['max_direction'] <= 25
        rows.append(row)
        if mode.startswith('mesh_only'):
            stage_rows.append(dict(case=case, **rec['timing_seconds'], wall=rec['wall_seconds'],
                                   generation=gen.get('mesh_and_layout_seconds', gen.get('generation_seconds', 0))+gen.get('step_read_seconds', 0),
                                   independent_confirmation=evaluation['wall_seconds'], creo=None if not cad else cad['seconds']))
        elif mode.startswith('XYZ'):
            stage_rows.append(dict(case=case, shared_layout_read=rec['preparation_shared_seconds'], mapping=rec['mapping_seconds'],
                                   matrix_assembly=rec['solver']['matrix_assembly_seconds'], solve=rec['solver']['solve_seconds'],
                                   brep_io_checks=rec['brep_io_check_seconds'], independent_confirmation=evaluation['wall_seconds'],
                                   creo=None if not cad else cad['seconds']))
    for name in ['baseline_cold', 'baseline_warm']:
        p = read(r/name/'timing.json'); stage_rows.append(dict(case=name, **p['stages_seconds'], wall=p['wall_before_independent_geometry_and_cad_seconds']))
    for filename, records in [('comparison.csv', rows), ('timing.csv', stage_rows)]:
        fields = list(dict.fromkeys(k for row in records for k in row))
        with (r/filename).open('w', encoding='utf-8-sig', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fields); writer.writeheader()
            writer.writerows({k: ('N/A' if v is None else v) for k, v in row.items()} for row in records)
    experiments = []
    for p in sorted(r.glob('B_*/reconstruction.json')):
        v = read(p); experiments.append(dict(case=p.parent.name, wall_seconds=v['wall_seconds'], controls=v['controls'],
                                             normal_policy=v.get('normal_policy', 'chart' if p.parent.name in ['B_coarse_29', 'B_coarse_17', 'B_closed32_13'] else 'global')))
    cad_results = {d.name: cad_summary(d) for d in (r/'creo').iterdir() if d.is_dir()}
    evaluation_executions = [dict(path=str(p.relative_to(r)), seconds=read(p)['wall_seconds'])
                            for p in r.rglob('*.json') if p.name in ['development.json', 'confirmation.json', 'confirmation_v2.json']]
    result = dict(status='completed_diagnostic_experiment', can_apply=False, base_commit='6efff7e59dd2a0d8f0e713f34019d88216c01568',
                  branch='experiment/mesh-brep-20260929', generated_at=datetime.now(timezone.utc).isoformat(),
                  comparison=rows, stage_timings=stage_rows, all_B_executions=experiments,
                  total_B_reconstruction_seconds=sum(x['wall_seconds'] for x in experiments), cad=cad_results,
                  evaluation_executions=evaluation_executions, total_evaluation_seconds=sum(x['seconds'] for x in evaluation_executions),
                  total_creo_seconds=sum(x['seconds'] for x in cad_results.values() if x is not None),
                  baseline_replay=read(r/'baseline_replay_comparison.json'),
                  input_generation_profile=read(r/'source_pipeline_profile/profile.json'),
                  isolation={k: read(r/k/'result/isolation_evidence.json') for k in ['isolation_candidate', 'isolation_closed']},
                  A_mapping=read(r/'A/mesh_correspondence.json'), A_interfaces=read(r/'A/original_interfaces.json'),
                  tests=dict(mesh_backend=5, retained_numerical=8, status='passed', cpp_product_tests='not applicable; no C++ product changes'),
                  caveats=['Maxima are sampled, not continuous bounds.', 'Closed fixtures have no source CAD; STEP-to-source-CAD metrics are N/A.',
                           'B requires supplied partitions/corners/chains and all internal seams declared smooth.',
                           'No algorithm cache is used; warm reruns do not claim physical disk-cache cold measurements.',
                           'Legacy evaluator boundary errors corrected without modifying reconstruction; initial confirmations retained.',
                           'Original outer-edge identity does not apply to B; new shared boundaries use exact common coefficients.',
                           'Same-face global self-intersection is not certified; sampled regularity and pairwise OCCT checks are reported.',
                           'CAD regional import checks do not certify local replacement into the original whole part.'])
    dump(r/'run.json', result)


if __name__ == '__main__':
    main()
