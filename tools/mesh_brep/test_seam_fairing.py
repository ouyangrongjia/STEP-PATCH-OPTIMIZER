import unittest
import json
from pathlib import Path
import tempfile
import shutil
import numpy as np
from .seam_fairing import normal_jacobian
from .splines import knots


class SeamFairingTests(unittest.TestCase):
    def test_matching_seams_still_detect_common_source_normal_error(self):
        from .seam_fairing import Config,seam_cost,seam_objective
        rec=dict(origin=np.zeros(3),axes=np.eye(3),lo=np.zeros(2),size=np.ones(2),
                 knots=knots(4),height=np.repeat(np.linspace(0,.1,4),4))
        flat=dict(rec,height=np.zeros(16))
        xyz=np.c_[np.linspace(.1,.9,9),np.full(9,.5),np.zeros(9)]
        seams=[((0,1),xyz,np.ones(9)/9)]
        targets=[np.tile([0.,0.,1.],(9,1))]
        before=seam_cost({0:rec,1:rec},seams,targets)
        after=seam_cost({0:flat,1:flat},seams,targets)
        self.assertEqual(before['normal_difference_energy'],0.)
        self.assertEqual(before['curvature_difference_energy'],0.)
        self.assertGreater(before['source_mean_normal_energy'],.005)
        cfg=Config(boundary_source_weight=.2)
        self.assertLess(seam_objective(after,cfg),seam_objective(before,cfg)*.001)

    def test_actual_joint_solver_removes_curvature_jump_with_fixed_curves(self):
        from .mesh import write_stl
        from .partition_fit import Config as FitConfig
        from .skeleton_graph import reconstruct
        from .seam_fairing import Config, optimize
        from .splines import tensor
        v = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0],
                      [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], float)
        f = np.array([[4, 5, 6], [4, 6, 7], [0, 2, 1], [0, 3, 2],
                      [0, 1, 5], [0, 5, 4], [1, 2, 6], [1, 6, 5],
                      [2, 3, 7], [2, 7, 6], [3, 0, 4], [3, 4, 7]])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); write_stl(root/'source.stl', v, f)
            np.savez_compressed(root/'partition.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=np.arange(len(f)))
            result = reconstruct(root/'partition.npz', root/'source.stl', root/'initial', smoothing=.02,
                                 workers=1, rounds=2, face_limit=12, normal_field='crease-fans', support_solver='bounded',
                                 config=FitConfig(distance=.03, boundary=.005, controls=(9,), normal_weight=.01, fairness=1e-6))
            source = Path(result['source_fit'])
            shutil.copytree(source, root/'flat_reference')
            path = next(p for p in (source/'charts').glob('*.npz') if int(np.load(p)['region']) == 0)
            r = dict(np.load(path)); q = np.stack(np.meshgrid(np.linspace(0, 1, 13), np.linspace(0, 1, 13)), -1).reshape(-1, 2)
            xyz = r['origin']+(q*r['size']+r['lo'])@r['axes'][:2]
            triangle = v[f[0], :2]
            bary = np.linalg.solve(np.vstack((triangle.T, np.ones(3))), np.c_[xyz[:, :2], np.ones(len(xyz))].T).T
            bump = .27*np.prod(bary, axis=1)
            r['height'] += np.linalg.lstsq(tensor(q, r['knots'], r['knots']).toarray(), bump, rcond=None)[0]
            np.savez_compressed(path, **r)
            curves = {p.name: p.read_bytes() for p in (source/'curves').glob('*.npz')}
            from .global_continuity import optimize as optimize_whole
            from .surface_jets import audit
            baseline = audit(source, root/'before.json', samples=33)
            from .continuity_guard import rollback_regressions
            flat=audit(root/'flat_reference',root/'flat.json',samples=129)
            rolled,rolled_audit,rollback=rollback_regressions(root/'flat_reference',source,flat,baseline,root/'quality_rollback')
            self.assertTrue(rollback['history'])
            self.assertLess(rolled_audit['smooth']['normal_degrees']['rms'],1e-5)
            self.assertEqual(curves,{p.name:p.read_bytes() for p in (rolled/'curves').glob('*.npz')})
            whole_config = Config(curvature_weight=.0005, variation_weight=1e-8, seam_weight=.2, iterations=2, solver_seconds=10.)
            from dataclasses import replace
            bounded = optimize_whole(source, root/'bounded',
                config=replace(whole_config, iterations=1, solver_seconds=2., solver_retry=False,
                               step_trials=2, physical_distance=True, protect_curvature_energy=True),
                sweeps=1, max_variables=162, max_charts=2, workers=2, seconds=30.)
            bounded_audit = audit(root/'bounded', root/'bounded.json', samples=33)
            self.assertTrue(bounded['whole_model_sweep_completed'])
            self.assertLess(bounded_audit['smooth']['normal_degrees']['rms'], baseline['smooth']['normal_degrees']['rms']*.5)
            self.assertLess(bounded_audit['smooth']['shape_jump_per_mm']['rms'], baseline['smooth']['shape_jump_per_mm']['rms']*.5)
            self.assertEqual(curves, {p.name:p.read_bytes() for p in (root/'bounded/curves').glob('*.npz')})
            expired = optimize_whole(source, root/'expired', config=whole_config, sweeps=1, workers=2, seconds=1e-12)
            self.assertTrue(expired['budget_exhausted'])
            self.assertFalse(expired['whole_model_sweep_completed'])
            self.assertEqual(expired['changed_charts'], [])
            self.assertEqual(len(expired['unfinished_charts']), 12)
            for p in (source/'charts').glob('*.npz'):
                np.testing.assert_array_equal(np.load(p)['height'], np.load(root/'expired/charts'/p.name)['height'])
            whole = optimize_whole(source, root/'whole', config=whole_config, sweeps=1, max_variables=162, max_charts=2, workers=2)
            final = audit(root/'whole', root/'after.json', samples=33)
            self.assertEqual(len(whole['attempted_charts']), 12)
            self.assertLess(final['smooth']['normal_degrees']['rms'], baseline['smooth']['normal_degrees']['rms']*.1)
            from .continuity_guard import optimize_curvature
            curvature_recovery=optimize_curvature(root/'flat_reference',source,root/'curvature_recovery',workers=1)
            self.assertTrue(curvature_recovery['initial_regressions'])
            self.assertTrue(curvature_recovery['history'][0]['accepted'])
            self.assertLess(len(curvature_recovery['remaining_regressions']),len(curvature_recovery['initial_regressions']))
            repeated=optimize_curvature(root/'flat_reference',source,root/'curvature_recovery',workers=1,resume=True)
            self.assertEqual(repeated['surface_fit'],curvature_recovery['surface_fit'])
            checkpoint = optimize_whole(source, root/'whole', config=whole_config, sweeps=1, max_variables=162, max_charts=2, workers=2, resume=True)
            self.assertEqual(len(checkpoint['events']), len(whole['events']))
            self.assertEqual(checkpoint['changed_charts'], whole['changed_charts'])
            from types import SimpleNamespace
            from unittest.mock import patch
            import osqp
            original_solve = osqp.OSQP.solve; calls = []
            def first_certificate_failure(solver, *args, **kwargs):
                calls.append(1)
                if len(calls) == 1:
                    return SimpleNamespace(x=None, info=SimpleNamespace(status='dual infeasible', status_val=5, run_time=0.))
                return original_solve(solver, *args, **kwargs)
            with patch.object(osqp.OSQP, 'solve', first_certificate_failure):
                report = optimize(source, root/'joint', regions=[0, 1],
                                  config=Config(curvature_weight=.0005, variation_weight=1e-8, seam_weight=.2, iterations=3, solver_seconds=10.))
            self.assertTrue(report['history'][0]['equilibrated_delta_retry'])
            self.assertTrue(report['accepted'])
            self.assertLess(report['after']['normal_difference_energy'], report['before']['normal_difference_energy']*.1)
            self.assertLess(report['after']['curvature_difference_energy'], report['before']['curvature_difference_energy']*.1)
            self.assertEqual(curves, {p.name:p.read_bytes() for p in (root/'joint/curves').glob('*.npz')})
            self.assertEqual(json.loads((root/'joint/run.json').read_text())['config']['distance'], .03)
            # Interrupted whole-model runs reuse only proposals rechecked on
            # the current canonical boundaries and original source domain.
            from .network_support import refit
            source = root/'joint'; meta = json.loads((source/'run.json').read_text())
            selected = [int(p.stem) for p in (source/'charts').glob('*.npz') if int(np.load(p)['region']) in (0, 1)]
            meta.update(requires_support_refit=True, required_refit_charts=selected, force_refit_charts=selected,
                        required_shared_support_groups=[selected])
            (source/'run.json').write_text(json.dumps(meta), encoding='utf-8')
            first = refit(source, root/'refitted', workers=1, boundary_normal_weight=.02)
            self.assertTrue(first['accepted'])
            self.assertEqual(first['jointly_refitted_groups'], [sorted(selected)])
            from .network_support import validate_wires
            fitted_records = {int(p.stem):dict(np.load(p)) for p in (root/'refitted/charts').glob('*.npz')}
            fitted_curves = {int(p.stem):dict(np.load(p)) for p in (root/'refitted/curves').glob('*.npz')}
            self.assertEqual(validate_wires(v, f, fitted_records, fitted_curves, selected, native_frame=True), [])
            # A valid source projection must not hide an invalid new support
            # projection: this frame collapses a horizontal trim to a line.
            folded = {i:dict(r) for i,r in fitted_records.items()}
            folded[selected[0]]['axes'] = np.array([[0.,0.,1.],[1.,0.,0.],[0.,1.,0.]])
            self.assertTrue(validate_wires(v, f, folded, fitted_curves, [selected[0]], native_frame=True))
            self.assertEqual(validate_wires(v, f, folded, fitted_curves, [selected[0]]), [])
            (root/'resumed').mkdir()
            shutil.copytree(root/'refitted/support_proposals', root/'resumed/support_proposals')
            shutil.copy2(root/'refitted/fitting.json', root/'resumed/fitting.json')
            resumed = refit(source, root/'resumed', workers=1, boundary_normal_weight=0., resume=True)
            self.assertEqual(resumed['verified_cached_proposals'], sorted(selected))
            from .network_pipeline import reuse_supports
            inherited = root/'inherited'; inherited.mkdir()
            shutil.copy2(root/'refitted/fitting.json', inherited/'fitting.json')
            (inherited/'network_support.json').write_text(json.dumps(dict(source_fit=str(root/'refitted'),
                 reused_compatible_charts=selected)), encoding='utf-8')
            shutil.copytree(source, root/'next_network')
            carried = reuse_supports(root/'next_network', inherited)
            self.assertEqual(carried, sorted(selected))
            for di in selected:
                np.testing.assert_array_equal(np.load(root/'next_network/charts'/f'{di:05d}.npz')['height'],
                                              np.load(root/'refitted/charts'/f'{di:05d}.npz')['height'])
            # Legacy caches can contain identical native supports without any
            # shared-support tag. Detect and restore the geometric identity.
            shutil.copytree(root/'refitted',root/'split')
            for di in selected:
                p=root/'split/charts'/f'{di:05d}.npz'; rec=dict(np.load(p)); rec.pop('shared_support_group',None)
                if di == selected[0]: rec['height']=rec['height']+.001
                np.savez_compressed(p,**rec)
                p=root/'refitted/charts'/f'{di:05d}.npz'; rec=dict(np.load(p)); rec.pop('shared_support_group',None)
                np.savez_compressed(p,**rec)
            from .continuity_guard import optimize as recover
            repaired=recover(root/'refitted',root/'split',root/'recovered',workers=1,rounds=0)
            self.assertEqual(repaired['broken_common_groups'],[sorted(selected)])
            restored=Path(repaired['surface_fit'])
            group_records=[dict(np.load(restored/'charts'/f'{di:05d}.npz')) for di in selected]
            for key in ('origin','axes','lo','size','knots','height'):
                np.testing.assert_array_equal(group_records[0][key],group_records[1][key])
            self.assertEqual(curves,{p.name:p.read_bytes() for p in (restored/'curves').glob('*.npz')})
            # If a moved network cannot retain a common support, the wrapper
            # protects that neighborhood and rebuilds the network itself.
            from dataclasses import asdict
            from .curve_routes import Config as CurveConfig
            meta=json.loads((root/'split/run.json').read_text())
            meta['global_network']=dict(excluded_charts=[],config=asdict(CurveConfig()))
            (root/'split/run.json').write_text(json.dumps(meta),encoding='utf-8')
            count=[]
            def fail_first_common_fit(*args,**kwargs):
                count.append(1)
                if len(count)==1: return dict(accepted=False,failed_charts=sorted(selected))
                return refit(*args,**kwargs)
            with patch('tools.mesh_brep.continuity_guard.refit',side_effect=fail_first_common_fit):
                fallback=recover(root/'refitted',root/'split',root/'fallback',workers=1,rounds=0)
            self.assertEqual(len(fallback['network_backoff']),1)
            self.assertEqual(fallback['network_backoff'][0]['excluded_charts'],sorted(selected))
            fallback_records=[dict(np.load(Path(fallback['surface_fit'])/'charts'/f'{i:05d}.npz')) for i in selected]
            for key in ('origin','axes','lo','size','knots','height'):
                np.testing.assert_array_equal(fallback_records[0][key],fallback_records[1][key])

    def test_unit_normal_derivative_in_rotated_anisotropic_chart(self):
        angle = .7
        axes = np.array([[np.cos(angle), 0., np.sin(angle)], [0., 1., 0.],
                         [-np.sin(angle), 0., np.cos(angle)]])
        rec = dict(origin=np.array([3., -2., 7.]), axes=axes, lo=np.array([-.3, .2]),
                   size=np.array([2., .15]), knots=knots(6), height=np.random.default_rng(8).normal(0, .01, 36))
        uv = np.array([[.2, .7], [.7, .3], [.45, .55]])
        xyz = rec['origin']+(uv*rec['size']+rec['lo'])@axes[:2]
        n, jac = normal_jacobian(rec, xyz)
        np.testing.assert_allclose(np.linalg.norm(n, axis=1), 1., atol=1e-13)
        delta = np.random.default_rng(4).normal(size=36)
        eps = 1e-7
        plus, _ = normal_jacobian(dict(rec, height=rec['height']+eps*delta), xyz)
        minus, _ = normal_jacobian(dict(rec, height=rec['height']-eps*delta), xyz)
        predicted = np.column_stack([j@delta for j in jac])
        np.testing.assert_allclose(predicted, (plus-minus)/(2*eps), rtol=1e-7, atol=1e-7)
        np.testing.assert_allclose(np.sum(predicted*n, axis=1), 0., atol=1e-12)

    def test_height_translation_does_not_change_normals(self):
        rec = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                   knots=knots(5), height=np.linspace(-.1, .2, 25))
        xyz = np.array([[.2, .6, 0.], [.8, .3, 0.]])
        before, jac = normal_jacobian(rec, xyz)
        after, _ = normal_jacobian(dict(rec, height=rec['height']+4.), xyz)
        np.testing.assert_allclose(before, after, atol=1e-13)
        for j in jac:
            np.testing.assert_allclose(j@np.ones(25), 0., atol=1e-13)


if __name__ == '__main__':
    unittest.main()
