import unittest
import numpy as np
from scipy.interpolate import BSpline

from .boundary_refine import Config, propose, tangent_constraints, swept_band_check
from .splines import knots
from .mesh import Locator


def plane():
    return dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                knots=knots(4), height=np.zeros(16))


def wave(z=0.):
    t = knots(14); x = np.array([t[i+1:i+4].mean() for i in range(14)])
    poles = np.c_[x, .5+.045*np.sin(7*np.pi*x), np.full(len(x), z)]
    return dict(poles=poles, knots=t, parameters=np.linspace(0, 1, 101), chain=np.arange(101))


class BoundaryRefineTests(unittest.TestCase):
    def test_crease_attraction_improves_source_line_distance(self):
        c = wave(); r = plane(); q = np.linspace(0, 1, 81)
        guide = (q, np.c_[q, np.full(len(q), .5), np.zeros(len(q))], np.ones(len(q)))
        candidate, result = propose(c, [r, r], Config(feature_weight=200.), feature_guide=guide)
        self.assertTrue(result['accepted'], result)
        self.assertLess(result['source_crease_rms_mm'][1], result['source_crease_rms_mm'][0])
        np.testing.assert_array_equal(candidate['poles'][[0, -1]], c['poles'][[0, -1]])

    def test_timeout_recovery_requires_complete_modelcheck_and_successful_import(self):
        import tempfile, json
        from pathlib import Path
        from .boundary_reconstruct import completed_modelcheck_after_timeout
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/'audit/modelcheck').mkdir(parents=True)
            for name,value in [('import',dict(timed_out=False,exit_code=0)),('modelcheck',dict(timed_out=True,exit_code=None))]:
                (root/f'audit/{name}_process.json').write_text(json.dumps(value),encoding='utf-8')
            p=root/'audit/modelcheck/model.p.xml'
            p.write_text('<root><check name="GEOM_CHECKS"><ans>0</ans></check><check name="PARAM_INFO">'
                         '<item><info1>PTC_VAL_IMP_PART_STATUS</info1><info2>OK</info2></item>'
                         '<item><info1>PTC_VAL_IMP_SCORE</info1><info2>PASS</info2></item></check></root>',encoding='utf-8')
            result=completed_modelcheck_after_timeout(root,'unused')
            self.assertTrue(result['completed_report_read_after_timeout'])
            self.assertTrue(result['process_failure_preserved'])
            p.write_text('<root>',encoding='utf-8')
            with self.assertRaises(Exception): completed_modelcheck_after_timeout(root,'unused')
            (root/'audit/import_process.json').write_text(json.dumps(dict(timed_out=True,exit_code=None)),encoding='utf-8')
            with self.assertRaises(ValueError): completed_modelcheck_after_timeout(root,'unused')

    def test_binary_assembly_and_in_memory_export_keep_closed_solid(self):
        import tempfile
        from pathlib import Path
        from dataclasses import asdict
        from .partition_fit import Config as FitConfig, fit_partition
        from .graph_reassemble import reconstruct
        from .graph_export import export
        from .export_regions import export as regions
        from .mesh import dump
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fit = root/'fit'; fit.mkdir()
            v = np.array([[0., 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]])
            f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
            partition = root/'input.npz'; labels = np.arange(4)
            np.savez(partition, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            config = FitConfig(distance=.001, boundary=.001, controls=(4,), normal_weight=0.)
            fit_partition(v, f, labels, config, fit)
            dump(fit/'run.json', dict(config=asdict(config), partition=str(partition), stl='unused'))
            context = {}
            assembled = reconstruct(fit, root/'assembly', write_preview=False,
                                    binary_native=True, native_context=context)
            self.assertTrue(assembled['watertight'])
            self.assertTrue((root/'assembly/native.binbrep').exists())
            result = export(root/'assembly', root/'step', native_context=context)
            self.assertTrue(result['watertight'])
            self.assertEqual(result['topology']['faces'], 4)
            compact = export(root/'assembly', root/'precise_step', native_context=context,
                             uncertainty=1e-7, measure_volume=False)
            self.assertTrue(compact['watertight'])
            self.assertIsNone(compact['topology']['volume_mm3'])
            self.assertFalse(compact['topology']['volume_evaluated'])
            self.assertEqual(compact['topology']['free_edges'], 0)
            self.assertEqual(compact['topology']['faces'], 4)
            self.assertEqual(regions(root/'assembly', root/'regions')['exported_regions'], 4)

    def test_curve_error_reuse_rejects_changed_surface_or_curve(self):
        from .partition_cad import checked_curve_errors
        c = dict(wave(), owners=np.array([0, 0])); r = plane()
        cert = dict(records={0: r}, curves={0: c}, errors={0: [0., 0.]})
        self.assertEqual(checked_curve_errors([r], [c], cert), [[0., 0.]])
        modified = dict(r, height=r['height']+.001)
        with self.assertRaisesRegex(ValueError, 'chart'):
            checked_curve_errors([modified], [c], cert)
        moved = dict(c, poles=c['poles']+np.array([0., .001, 0.]))
        with self.assertRaisesRegex(ValueError, 'curve'):
            checked_curve_errors([r], [moved], cert)

    def test_fairing_reduces_real_curve_bending_and_keeps_endpoints_and_supports(self):
        c = wave(); r = plane(); old_height = r['height'].copy()
        candidate, result = propose(c, [r, r], Config())
        self.assertTrue(result['accepted'], result)
        self.assertLess(result['after']['curvature_squared_integral_per_mm'],
                        result['before']['curvature_squared_integral_per_mm']*.5)
        np.testing.assert_array_equal(candidate['poles'][[0, -1]], c['poles'][[0, -1]])
        np.testing.assert_array_equal(r['height'], old_height)
        self.assertLess(max(result['support_error_mm']), 1e-9)

    def test_incompatible_fixed_endpoint_cannot_be_accepted(self):
        c = wave(.02); result, info = propose(c, [plane(), plane()], Config())
        self.assertFalse(info['accepted'])
        np.testing.assert_array_equal(result['poles'], c['poles'])

    def test_tangent_cone_excludes_overshoot_and_backtracking(self):
        t = knots(4); cp = np.array([[0, 0, 0], [1/3, 0, 0], [2/3, 0, 0], [1, 0, 0.]])
        target = np.array([[1., 1., 0.], [1., 0., 0.]])
        a, low, high = tangent_constraints(t, cp, target)
        def feasible(direction):
            new = cp.copy(); new[1] = direction/3
            value = a@(new-cp).ravel()
            return np.all(value >= low-1e-9) and np.all(value <= high+1e-9)
        self.assertTrue(feasible(np.array([1., .5, 0.])))
        self.assertFalse(feasible(np.array([1., 2., 0.])))
        self.assertFalse(feasible(np.array([-1., 0., 0.])))
        self.assertFalse(feasible(np.array([1., .5, .2])))

    def test_newly_exposed_strip_is_checked_against_finite_source_triangles(self):
        v = np.array([[0., 0., 0.], [1, 0, 0], [1, 1, 0], [0, 1, 0]])
        locator = Locator(v, np.array([[0, 1, 2], [0, 2, 3]]))
        r = plane(); r['size'] = np.array([2., 2.])
        c = wave(); c['poles'][:, 1] = .95
        bad = dict(c, poles=c['poles']+np.array([0., .15, 0.]))
        check = swept_band_check(c, bad, [r, r], locator, .03)
        self.assertFalse(check['passed'])
        self.assertGreater(check['source_distance_max_mm'], .09)


if __name__ == '__main__':
    unittest.main()
