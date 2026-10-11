"""Cold STL/label reconstruction: plan, fair, fit, bounded repair, validate.

The public input contract deliberately has no candidate, prior-fit, boundary
cache or resume option. Reuse is limited to supports computed during this call.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import time
import numpy as np
from .mesh import dump
from .partition_fit import Config
from .curve_routes import Config as CurveConfig
from .distance_guard import Options


@dataclass(frozen=True)
class Budget:
    workers: int = 8
    layout_rounds: int = 2
    fit_iterations: int = 16
    solver_seconds: float = .75
    support_seconds: float = 20.
    round_seconds: float = 600.
    correction_seconds: float = 2.
    join_proposals: int = 0
    join_seconds: float = 45.
    distance_repairs: int = 1
    distance_groups: int = 8
    distance_seconds: float = 60.
    minimum_graph_cosine: float = .25
    normal_rms_goal: float = 3.
    continuity_seconds: float = 300.
    continuity_solver_seconds: float = 2.
    continuity_variables: int = 6000

    def validate(self):
        if (not 0 < self.minimum_graph_cosine < 1 or not np.isfinite(self.normal_rms_goal) or self.normal_rms_goal <= 0
                or min(self.workers, self.layout_rounds, self.fit_iterations, self.distance_groups, self.continuity_variables) < 1
                or min(self.join_proposals, self.distance_repairs) < 0
                or any(not np.isfinite(x) or x <= 0 for x in
                       (self.solver_seconds, self.support_seconds, self.round_seconds,
                        self.correction_seconds, self.join_seconds, self.distance_seconds,
                        self.continuity_seconds, self.continuity_solver_seconds))):
            raise ValueError('Invalid cold reconstruction compute budget')


def boundary_coverage(before, after, output):
    """All regions share one denominator, including unchanged interfaces."""
    from .boundary_quality import compare
    before, after = Path(before), Path(after)
    paired = compare(before, after, output)
    charts = {int(p.stem): int(np.load(p)['region']) for p in (after/'charts').glob('*.npz')}
    changed = {r['after_curve']: r for r in paired['changed']}
    totals = {}
    from .boundary_quality import measures
    for path in sorted((after/'curves').glob('*.npz')):
        ci = int(path.stem); c = dict(np.load(path)); old = dict(np.load(before/'curves'/path.name))
        bm, am = measures(old), measures(c)
        for region in set(charts[int(di)] for di in c['owners']):
            row = totals.setdefault(region, dict(region=region, curves=0, changed_curves=0,
                length_mm=0., changed_length_mm=0., before_turn_degrees=0., after_turn_degrees=0.,
                before_bending=0., after_bending=0.))
            row['curves'] += 1; row['length_mm'] += bm['length_mm']
            row['changed_curves'] += ci in changed
            row['changed_length_mm'] += bm['length_mm'] if ci in changed else 0.
            row['before_turn_degrees'] += bm['total_tangent_turn_degrees']
            row['after_turn_degrees'] += am['total_tangent_turn_degrees']
            row['before_bending'] += bm['curvature_squared_integral_per_mm']
            row['after_bending'] += am['curvature_squared_integral_per_mm']
    paired['per_region'] = list(totals.values())
    paired['all_source_regions'] = len(totals)
    paired['changed_regions'] = sum(r['changed_curves'] > 0 for r in totals.values())
    paired['region_length_counts_shared_interfaces_on_both_sides'] = True
    dump(output, paired)
    return paired


def reconstruct(stl, partition, output, *, reference_faces, budget=None, config=None,
                curve_config=None, creo_root=None, native_probe=None, validate=True):
    budget = budget or Budget(); budget.validate()
    config = config or Config(distance=.03, boundary=.005, controls=(65,),
                              normal_weight=.01, fairness=1e-6, area_weighted=True)
    curve_config = curve_config or CurveConfig(movement=.16, tube=.012, maximum_controls=32,
                                               minimum_length=.15, cross_region_junctions=True,
                                               crease_plane_tolerance=.006)
    if reference_faces < 2 or (config.boundary_normal_weight and budget.join_proposals):
        raise ValueError('Invalid face budget or incompatible boundary-normal repair mode')
    if (creo_root is None) != (native_probe is None):
        raise ValueError('Creo validation requires both installation and native probe')
    root = Path(output); root.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter()
    report = dict(status='running', algorithm='cold-shared-master-network-v6', can_apply=False,
        stl=str(Path(stl).resolve()), partition=str(Path(partition).resolve()), units='mm',
        budget=asdict(budget), config=asdict(config), curve_config=asdict(curve_config),
        reference_faces=reference_faces, cold_start=True, external_candidate_reads=0,
        source_CAD_read=False, Geomagic_API_used=False, stages=[], visual_review_required=True)
    report['distance_acceptance_metric'] = 'finite_source_euclidean_then_actual_STEP_audits'
    dump(root/'run.json', report)

    def stage(name, operation):
        row = dict(stage=name, status='running'); report['stages'].append(row)
        dump(root/'run.json', report); tick = time.perf_counter()
        print(dict(stage=name, status='running'), flush=True)
        try:
            value = operation()
            row['status'] = 'complete'
            return value
        except (Exception, KeyboardInterrupt) as error:
            row.update(status='failed', error=f'{type(error).__name__}: {error}')
            raise
        finally:
            row['seconds'] = time.perf_counter()-tick
            dump(root/'run.json', report)

    try:
        from .skeleton_graph import reconstruct as fit
        fitted = stage('plan_smooth_fit', lambda: fit(partition, stl, root/'reconstruction',
            smoothing=min(.12, curve_config.movement), workers=budget.workers, rounds=budget.layout_rounds,
            face_limit=reference_faces-1, config=config, boundary_mode='fair-chains',
            normal_field='crease-fans', normal_filter_steps=2, support_solver='bounded',
            master_config=curve_config, fitting_fraction=.9, fit_iterations=budget.fit_iterations,
            solver_seconds=budget.solver_seconds, fit_seconds=budget.support_seconds,
            round_seconds=budget.round_seconds, correction_seconds=budget.correction_seconds,
            split_minor_residuals=False, solver_retry=False,
            minimum_graph_cosine=budget.minimum_graph_cosine, quick_penalty=True,
            normal_rms_goal=budget.normal_rms_goal))
        report['fitting'] = {k: fitted[k] for k in ('source_fit', 'fitted_charts', 'complete_source_coverage',
                             'partition_labels_unchanged', 'master_network', 'fitting_budget')}
        initial_fit = Path(fitted['source_fit']); surface_fit = initial_fit
        report['first_fit'] = str(initial_fit)
        from .global_continuity import optimize as couple
        from .seam_fairing import Config as ContinuityConfig
        report['continuity_optimization'] = stage('bounded_whole_continuity', lambda: couple(
            surface_fit, root/'continuity', workers=budget.workers, sweeps=1,
            max_variables=budget.continuity_variables, max_charts=24, seconds=budget.continuity_seconds,
            config=ContinuityConfig(normal_weight=.3, seam_weight=.2, curvature_weight=.0005,
                variation_weight=1e-6, boundary_source_weight=.2, normal_slack_degrees=.1,
                iterations=1, solver_seconds=budget.continuity_solver_seconds,
                solver_retry=False, step_trials=2, physical_distance=True, protect_curvature_energy=True)))
        surface_fit = root/'continuity'
        if budget.join_proposals:
            from .shared_support import optimize
            report['join_repair'] = stage('bounded_join_repair', lambda: optimize(surface_fit, root/'join_repair',
                workers=min(4, budget.workers), max_proposals=budget.join_proposals, seconds=budget.join_seconds,
                fit_options=dict(max_iterations=min(14, budget.fit_iterations), solver_seconds=budget.solver_seconds,
                                 seconds=min(20., budget.support_seconds))))
            surface_fit = root/'join_repair'
        metrics = root/'metrics'; metrics.mkdir()
        from .network_support import validate_wires
        def check_trims():
            with np.load(partition) as data:
                v, f = data['mesh_vertex_original'], data['mesh_triangle']
            records = {int(p.stem): dict(np.load(p)) for p in (surface_fit/'charts').glob('*.npz')}
            curves = {int(p.stem): dict(np.load(p)) for p in (surface_fit/'curves').glob('*.npz')}
            failures = validate_wires(v, f, records, curves, set(records), native_frame=True)
            dump(metrics/'native_trims.json', dict(passed=not failures, failures=failures, charts=len(records)))
            if failures: raise ValueError('Fresh native support trim validation failed')
        stage('native_trim_check', check_trims)
        if validate:
            from .skeleton_reconstruct import reconstruct as finalize
            report['validation'] = stage('STEP_distance_repair_Creo', lambda: finalize(stl, partition, root/'validated',
                reference_faces=reference_faces, surface_fit=surface_fit, render=False,
                distance_options=Options(max_repairs=budget.distance_repairs, preventive_repair=False,
                    preserve_continuity=True, projection_repair=False, max_repair_groups=budget.distance_groups,
                    repair_seconds=budget.distance_seconds, fit_seconds=budget.support_seconds,
                    solver_seconds=budget.solver_seconds, fit_iterations=budget.fit_iterations,
                    short_repair_seconds=budget.correction_seconds),
                creo_root=creo_root, native_probe=native_probe))
            surface_fit = Path(report['validation']['surface_fit'])
            report['model'] = report['validation']['model']
        report['surface_fit'] = str(surface_fit.resolve())
        from .surface_jets import audit
        stage('final_continuity_measurement', lambda: audit(surface_fit, metrics/'continuity.json', samples=129))
        quality = stage('whole_boundary_coverage', lambda: boundary_coverage(
            initial_fit/'source_network', surface_fit, metrics/'boundaries.json'))
        report['boundary_changes'] = dict(curves=quality['changed_matched_curves'],
                                          regions=quality['changed_regions'], total_regions=quality['all_source_regions'])
        report.update(status='DiagnosticOnly' if validate else 'fitted_pending_CAD_validation',
                      surface_fit=str(surface_fit.resolve()), whole_model_G1_G2_certified=False)
        if validate and creo_root is not None and report['validation']['creo']['geometry_check_features']:
            raise ValueError('Creo geometry-check features remain')
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                      error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['seconds'] = time.perf_counter()-started
        dump(root/'run.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stl', required=True); p.add_argument('--partition', required=True)
    p.add_argument('--output', required=True); p.add_argument('--reference-faces', required=True, type=int)
    p.add_argument('--workers', type=int, default=8)
    p.add_argument('--creo-root'); p.add_argument('--native-probe')
    a = p.parse_args()
    result = reconstruct(a.stl, a.partition, a.output, reference_faces=a.reference_faces,
        budget=Budget(workers=a.workers), creo_root=a.creo_root, native_probe=a.native_probe)
    print({k: result.get(k) for k in ('status', 'model', 'seconds', 'boundary_changes')}, flush=True)
