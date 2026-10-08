"""Optional constrained fairing on a closed, independently generated mesh."""
from dataclasses import asdict
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from .generate import fixture
from .mesh import read_stl, write_stl, dump
from .partition_fit import Config, fit_partition, boundary_edges, polyline_graph_error


@unittest.skipUnless(importlib.util.find_spec('osqp'), 'Optional OSQP is not installed')
class BoundedFairingTests(unittest.TestCase):
    def test_hard_cone_enforces_oriented_normal_and_rejects_backwards_target(self):
        import osqp
        from scipy import sparse
        from .graph_fair_optimize import normal_cone_constraints
        du, dv = sparse.csr_matrix([[1., 0.]]), sparse.csr_matrix([[0., 1.]])
        for target, feasible in [(np.array([.4, -.5, 1.]), True), (np.array([0., 0., -1.]), False)]:
            target /= np.linalg.norm(target)
            a, lower, upper = normal_cone_constraints(du, dv, target[None], 8.)
            solver = osqp.OSQP()
            solver.setup(P=sparse.eye(2, format='csc'), q=np.zeros(2), A=a, l=lower, u=upper,
                         eps_abs=1e-9, eps_rel=1e-9, verbose=False)
            result = solver.solve(raise_error=False)
            if feasible:
                self.assertEqual(result.info.status_val, 1)
                normal = np.r_[-result.x, 1.]; normal /= np.linalg.norm(normal)
                self.assertLessEqual(np.degrees(np.arccos(np.clip(normal@target, -1, 1))), 8.00001)
            else:
                self.assertEqual(result.info.status_val, 3)

    def test_entry_runs_fairing_conditioning_and_boundary_history_on_native_geometry(self):
        from .graph_reconstruct import reconstruct
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); stl = root/'input.stl'
            v = np.array([[0.,0,0],[1,0,0],[0,1,0],[0,0,1]])
            f = np.array([[0,2,1],[0,1,3],[1,2,3],[2,0,3]])
            write_stl(stl, v, f); v, f = read_stl(stl)
            partition = root/'partition.npz'
            np.savez(partition, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=np.arange(4))
            result = reconstruct(stl, partition, root/'result', reference_faces=5, render=False,
                config=Config(distance=1e-5, boundary=1e-5, controls=(4,), normal_weight=0.),
                fairing=True, condition_normals=True, boundary_smoothing=1e-7, compact_edges=True)
            self.assertTrue(result['step']['watertight'])
            self.assertEqual(result['step']['topology']['faces'], 4)
            from .graph_fair_optimize import optimize
            bounded = optimize(result['native_fit'], root/'tight_boundary', normal_field='crease-fans',
                boundary_normal_weight=.1, boundary_normal_limit=5., boundary_smoothing=1e-7,
                boundary_target_tolerance=1e-7)
            self.assertEqual(bounded['optimized_charts'], 4)
            events = json.loads((root/'tight_boundary/optimization.json').read_text())
            self.assertTrue(all(row['boundary_sample_max_mm'] <= 1.001e-7 for row in events))
            # Exercise the observed long-edge STEP tolerance failure without
            # requiring a large commercial sample in the regression suite.
            from unittest.mock import patch
            from .graph_export import export as real_export
            calls = []
            def reported_tolerance_failure(*args, **kwargs):
                exported = real_export(*args, **kwargs); calls.append(exported)
                if len(calls) == 1:
                    import copy
                    exported = copy.deepcopy(exported)
                    exported['watertight'] = False
                    exported['topology']['maximum_edge_tolerance_mm'] = 1.
                return exported
            with patch('tools.mesh_brep.graph_export.export', side_effect=reported_tolerance_failure):
                second = reconstruct(stl, partition, root/'reused', reference_faces=5, render=False,
                    from_fit=result['native_fit'], boundary_smoothing=1e-7, boundary_history=[root/'result'],
                    compact_edges=True, max_refinements=1)
            self.assertTrue(second['step']['watertight'])
            self.assertEqual(second['boundary_history'][0]['cumulative_fallback_charts'], [])
            self.assertEqual(second['attempts'][0]['status'], 'failed_compact_step')
            self.assertFalse(second['attempts'][1]['compact_shared_edges'])
            self.assertEqual(second['step']['topology']['faces'], 4)

    def test_closed_mesh_keeps_chart_topology_and_continuous_boundary_budget(self):
        from .graph_fair_optimize import optimize
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root/'input', 3, 'closed'); stl = root/'input/input.stl'
            v, f = read_stl(stl)
            labels = np.array(json.loads((root/'input/layout.json').read_text())['triangle_labels'])
            partition = root/'partition.npz'
            np.savez(partition, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            config = Config(distance=.1, boundary=.02, controls=(4, 7, 11), normal_weight=0.,
                            shared_smooth_normals=True, interior_subdivisions=4)
            fit = root/'fit'; fit.mkdir(); fit_partition(v, f, labels, config, fit)
            dump(fit/'run.json', dict(config=asdict(config), partition=str(partition), stl=str(stl), status='DiagnosticOnly'))
            from .graph_repair import repair
            repair(fit, root/'refined', refine_charts=[0], boundary_scale=.25)
            fit = root/'refined'
            result = optimize(fit, root/'optimized', time_limit=2., boundary_normal_weight=.1,
                              normal_field='crease-fans', boundary_normal_limit=20., boundary_smoothing=.002)
            self.assertGreater(result['optimized_charts'], 0)
            events = json.loads((root/'optimized/optimization.json').read_text())
            self.assertTrue(all(row['boundary_normal_max_degrees'] <= 20.0001
                                for row in events if row['accepted']))
            np.testing.assert_array_equal(np.load(fit/'chart_index.npz')['face_chart'],
                                          np.load(root/'optimized/chart_index.npz')['face_chart'])
            for path in sorted((root/'optimized/charts').glob('*.npz')):
                old = dict(np.load(fit/'charts'/path.name)); rec = dict(np.load(path))
                self.assertEqual(rec['height'].shape, old['height'].shape)
                np.testing.assert_array_equal(rec['triangle_ids'], old['triangle_ids'])
                self.assertEqual(int(rec['region']), int(old['region']))
                used = rec['vertex_ids']; tri = np.searchsorted(used, f[rec['triangle_ids']])
                maximum = max(polyline_graph_error(v[used[e]], rec) for e in boundary_edges(tri))
                self.assertLessEqual(maximum, min(config.boundary,
                    float(rec.get('boundary_budget_mm', config.boundary)))*.9)


if __name__ == '__main__':
    unittest.main()
