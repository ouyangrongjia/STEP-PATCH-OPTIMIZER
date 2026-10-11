import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from .automatic_reconstruct import reconstruct, Budget
from .curve_routes import Config as CurveConfig
from .partition_fit import Config
from .skeleton_graph import reconstruct as fit, fit_support
from .mesh import write_stl


class AutomaticReconstructionTests(unittest.TestCase):
    def test_one_quality_proposal_improves_normals_or_retains_valid_geometry(self):
        from unittest.mock import patch
        from .splines import knots, tensor
        x = np.linspace(0., 1., 9)
        v = np.array([[a,b,0.] for a in x for b in x])
        f = np.array([[9*i+j,9*i+j+1,9*(i+1)+j+1] for i in range(8) for j in range(8)]
                     +[[9*i+j,9*(i+1)+j+1,9*(i+1)+j] for i in range(8) for j in range(8)])
        f = f[:, ::-1]
        n = np.tile([0.,0.,1.], (len(v),1))
        corners = v[[0,72,80,8]]
        curves = [dict(knots=knots(4), poles=np.linspace(corners[i],corners[(i+1)%4],4)) for i in range(4)]
        boundary = np.concatenate([c['poles'] for c in curves])
        cfg = Config(distance=.027,boundary=.005,controls=(25,),area_weighted=True)
        task = (0,v,f,n,boundary,curves,cfg,'bounded',None,
                dict(origin=np.array([.5,.5,0.]),axes=np.eye(3),lo=np.array([-.51,-.51]),size=np.array([1.02,1.02]),knots=knots(9)),True)
        for useful in (True,False):
            calls=[]
            def proposal(a,z,b,bz,normal_q,normals,t,size,*args,**kwargs):
                count=len(t)-4;calls.append(count)
                if len(calls)>1:
                    return np.full(count*count,0. if useful else .06),dict(solved=False)
                g=np.array([t[i+1:i+4].mean() for i in range(count)])
                q=np.stack(np.meshgrid(g,g,indexing='ij'),axis=-1).reshape(-1,2)
                xx,yy=(-.01+q*size).T
                return np.linalg.solve(tensor(q,t,t).toarray(),.32*xx*(1-xx)*yy*(1-yy)),dict(solved=False)
            with self.subTest(useful=useful), patch('tools.mesh_brep.bounded_support.solve',side_effect=proposal):
                _,rec,info=fit_support(task,max_iterations=16,acceptance_distance=.03,normal_rms_goal=1.)
                self.assertTrue(info['accepted']);self.assertEqual(len(calls),2)
                self.assertEqual(info['quality_proposal']['attempts'],1)
                self.assertEqual(info['quality_proposal']['accepted'],useful)
                self.assertLess(rec['continuous_boundary_error_mm'],.002)
                self.assertLess(rec['interior_probe_max_mm'],.03)
                if useful:
                    self.assertLess(rec['fit_normal_rms_degrees'],1e-6)
                else:
                    self.assertGreater(rec['fit_normal_rms_degrees'],1.)
                    self.assertEqual(len(rec['knots'])-4,9)

    def test_minor_failure_only_shares_a_replan_already_required_by_substantial_failure(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        v = np.array([[0.,0,0],[1.,0,0],[1.,1,0],[0.,1,0],
                      [0.,0,1],[1.,0,1],[1.,1,1],[0.,1,1]])
        f = np.array([[0,2,1],[0,3,2],[4,5,6],[4,6,7],[0,1,5],[0,5,4],
                      [1,2,6],[1,6,5],[2,3,7],[2,7,6],[3,0,4],[3,4,7]])
        for substantial in (False, True):
            with self.subTest(substantial=substantial), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); write_stl(root/'source.stl', v, f)
                np.savez(root/'source.npz', mesh_vertex_original=v, mesh_triangle=f,
                         face_instance_id=np.repeat(np.arange(6), 2))
                calls = 0
                def simulated_failure(task, **options):
                    nonlocal calls
                    calls += 1; chart = task[0]
                    if calls <= 6 and (chart == 0 or (substantial and chart == 1)):
                        return chart, None, dict(accepted=False, history=[], reason='injected_failure',
                            failure_class='minor_residual' if chart == 0 else 'unresolved_support')
                    return fit_support(task, **options)
                def serial_results(fitter, tasks, chunksize):
                    iterator = (fitter(task) for task in tasks)
                    return SimpleNamespace(next=lambda timeout=None: next(iterator))
                with patch('tools.mesh_brep.skeleton_graph.fit_support', side_effect=simulated_failure), \
                        patch('multiprocessing.Pool') as pool:
                    pool.return_value.imap_unordered.side_effect = serial_results
                    kwargs = dict(workers=1, rounds=2, face_limit=10, smoothing=.01,
                        config=Config(distance=.001, boundary=.001, controls=(15,), normal_weight=0.),
                        support_solver='bounded', split_minor_residuals=False)
                    if substantial:
                        result = fit(root/'source.npz', root/'source.stl', root/'result', **kwargs)
                        self.assertTrue(result['complete_source_coverage'])
                        self.assertEqual(result['fitted_charts'], 8)
                        self.assertEqual(result['minor_residuals_in_required_replan']['additional_layout_rounds'], 0)
                    else:
                        with self.assertRaisesRegex(ValueError, 'Unresolved minor residuals'):
                            fit(root/'source.npz', root/'source.stl', root/'result', **kwargs)
                        self.assertFalse((root/'result/round_1').exists())

    def test_tilted_patch_uses_finite_surface_distance_instead_of_height_proxy(self):
        from unittest.mock import patch
        from .splines import knots, tensor
        v = np.array([[0., 0, 0], [1., 0, 2], [1., 1, 2], [0., 1, 0]])
        f = np.array([[0, 1, 2], [0, 2, 3]])
        normals = np.tile(np.array([-2., 0, 1])/np.sqrt(5), (4, 1))
        curves = [dict(knots=knots(4), poles=np.linspace(v[i], v[(i+1)%4], 4),
                       parameters=np.array([0., 1.])) for i in range(4)]
        t = knots(9); g = np.array([t[i+1:i+4].mean() for i in range(9)])
        q = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
        seed = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.full(2, -.01), size=np.full(2, 1.02), knots=t)
        x, y = (seed['lo']+q*seed['size']).T
        for amplitude, accepted in ((.04, True), (.09, False)):
            height = np.linalg.solve(tensor(q, t, t).toarray(), 2*x+amplitude*16*x*(1-x)*y*(1-y))
            with patch('tools.mesh_brep.bounded_support.solve', return_value=(height, dict(solved=False))):
                _, rec, info = fit_support((0, v, f, normals, v, curves,
                    Config(distance=.027, boundary=.005, controls=(9,)), 'bounded', None, seed, True),
                    max_iterations=1, acceptance_distance=.03)
            self.assertEqual(info['accepted'], accepted)
            if accepted:
                self.assertGreater(info['history'][0]['distance_max'], .03)
                self.assertLess(rec['interior_probe_max_mm'], .03)
                self.assertEqual(rec['interior_probe_metric'], 'finite_source_euclidean')
            else:
                self.assertIsNone(rec)

    def test_physical_tolerance_not_solver_reserve_decides_acceptance(self):
        from unittest.mock import patch
        from .splines import knots
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0]])
        f = np.array([[0, 1, 2]]); normals = np.tile([0., 0, 1], (3, 1))
        for offset, accepted in ((.029, True), (.031, False)):
            edges = v+np.array([0., 0, offset])
            curves = [dict(knots=knots(4), poles=np.linspace(edges[i], edges[(i+1)%3], 4)) for i in range(3)]
            # A timed-out solver proposal is checked as geometry, not accepted
            # because of a status flag or an increased tolerance.
            with patch('tools.mesh_brep.bounded_support.solve', return_value=(np.full(81, offset),
                       dict(solved=False, status='run time limit reached'))):
                _, rec, info = fit_support((0, v, f, normals, edges, curves,
                    Config(distance=.027, boundary=.005, controls=(9,)), 'bounded'),
                    max_iterations=1, acceptance_distance=.03)
            self.assertEqual(info['accepted'], accepted)
            if accepted:
                self.assertFalse(rec['distance_reserve_met'])
                self.assertEqual(len(info['history']), 1)
                self.assertLess(rec['continuous_boundary_error_mm'], .002)
            else:
                self.assertIsNone(rec)

    def test_cold_pipeline_plans_before_fitting_and_exports_a_closed_solid(self):
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); write_stl(root/'source.stl', v, f)
            np.savez(root/'partition.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=np.arange(4))
            result = reconstruct(root/'source.stl', root/'partition.npz', root/'result', reference_faces=5,
                budget=Budget(workers=1),
                config=Config(distance=.001, boundary=.001, controls=(15,), normal_weight=0., fairness=1e-8,
                              boundary_normal_weight=.02),
                curve_config=CurveConfig(movement=.05, tube=.0001))
            self.assertEqual(result['status'], 'DiagnosticOnly')
            self.assertEqual(result['external_candidate_reads'], 0)
            self.assertTrue(result['validation']['step']['watertight'])
            self.assertEqual(result['validation']['step']['topology']['faces'], 4)
            self.assertEqual(result['validation']['regions']['exported_regions'], 4)
            seed = Path(result['first_fit'])/'source_network'
            self.assertTrue(all('height' not in np.load(p).files for p in (seed/'charts').glob('*.npz')))
            rows = json.loads((seed/'fitting.json').read_text())
            self.assertTrue(all(not row['accepted'] and row['reason'] == 'not_fitted_yet' for row in rows))
            final = Path(result['surface_fit'])
            for p in (final/'charts').glob('*.npz'):
                rec = dict(np.load(p)); self.assertIn('height', rec)
                self.assertTrue(np.all(np.arange(4)[rec['triangle_ids']] == int(rec['region'])))
            self.assertEqual(result['boundary_changes']['total_regions'], 4)
            self.assertFalse(result['whole_model_G1_G2_certified'])

    def test_master_planning_rejects_external_candidate_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, 'without prior candidates'):
                fit('source.npz', 'source.stl', Path(temporary)/'out',
                    master_config=CurveConfig(), from_fit='old_candidate')
            self.assertFalse((Path(temporary)/'out').exists())

    def test_expired_support_budget_returns_failure_without_claiming_fit(self):
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0]])
        di, rec, info = fit_support((0, v, np.array([[0, 1, 2]]), np.tile([0., 0, 1], (3, 1)),
                                    v, [], Config(), 'bounded'), seconds=1e-12)
        self.assertIsNone(rec)
        self.assertEqual(info['reason'], 'support_time_budget')
        self.assertFalse(info['accepted'])


if __name__ == '__main__':
    unittest.main()
