import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import numpy as np
from . import bounded_support
from .fast_merge import Budget, initialize, propose, save_fit
from .partition_fit import Config
from .splines import knots


class FastMergeTests(unittest.TestCase):
    def fixture(self):
        v = np.array([[0., 0., 0.], [1., 0., 0.], [1., 1., 0.], [0., 1., 0.]])
        f = np.array([[0, 1, 2], [0, 2, 3]])
        base = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                    height=np.zeros(81), knots=knots(9))
        records = {i: dict(base, triangle_ids=np.array([i]), vertex_ids=f[i], region=0) for i in range(2)}
        records[2] = dict(base, region=1, triangle_ids=np.array([], dtype=int), vertex_ids=np.array([], dtype=int))
        curves = {}
        for ci, (a, b, owners) in enumerate([(0, 1, [0, 2]), (1, 2, [0, 2]), (2, 3, [1, 2]),
                                            (3, 0, [1, 2]), (0, 2, [0, 1])]):
            curves[ci] = dict(chain=np.array([a, b]), poles=np.linspace(v[a], v[b], 4),
                              parameters=np.array([0., 1.]), knots=knots(4), owners=np.array(owners))
        cfg = Config(distance=.03, boundary=.005, controls=(33,), area_weighted=True, fairness=1e-6)
        initialize(v, f, np.tile([0., 0., 1.], (2, 3, 1)), records, curves, cfg, Budget())
        return records, curves

    def test_planar_union_removes_cut_with_one_fit_and_unchanged_outer_curves(self):
        records, curves = self.fixture()
        fitted, info = propose((0, 1))
        self.assertTrue(info['accepted'], info)
        self.assertEqual(len(info['fitting']['history']), 1)
        self.assertLess(np.max(abs(fitted['height'])), 1e-10)
        with TemporaryDirectory() as folder:
            root = Path(folder)/'source'; root.mkdir(); out = Path(folder)/'merged'; out.mkdir()
            np.savez_compressed(root/'chart_index.npz', face_chart=np.array([0, 1]))
            (root/'fitting.json').write_text(json.dumps([dict(chart=i) for i in records]), encoding='utf-8')
            report = save_fit(root, out, records, curves, [((0, 1), fitted)], {})
            np.testing.assert_array_equal(np.load(out/'chart_index.npz')['face_chart'], [0, 0])
            self.assertEqual(report['fast_merge_ancestry']['removed_curves'], [4])
            for ci in range(4):
                saved = dict(np.load(out/'curves'/f'{ci:05d}.npz'))
                np.testing.assert_array_equal(saved['poles'], curves[ci]['poles'])
                np.testing.assert_array_equal(saved['owners'], [0, 1])

    def test_infeasible_boundary_is_rejected_without_local_retry(self):
        records, curves = self.fixture()
        curves[0]['poles'][1:3, 2] = .2
        with patch.object(bounded_support, 'solve', wraps=bounded_support.solve) as solve:
            fitted, info = propose((0, 1))
        self.assertIsNone(fitted)
        self.assertEqual(info['reason'], 'single_fit_rejected')
        self.assertEqual(solve.call_count, 1)
        self.assertFalse(solve.call_args.kwargs['equilibrated_retry'])
        self.assertEqual(len(info['fitting']['history']), 1)

    def test_invalid_budget_is_rejected(self):
        for budget in (Budget(seconds=0), Budget(workers=0), Budget(controls=8), Budget(seconds=float('nan'))):
            with self.assertRaises(ValueError): budget.validate()

    def test_small_input_cannot_silently_increase_representation_cost(self):
        records, _ = self.fixture()
        for i in (0, 1):
            records[i]['height'] = np.zeros(16); records[i]['knots'] = knots(4)
        with patch.object(bounded_support, 'solve', wraps=bounded_support.solve) as solve:
            fitted, info = propose((0, 1))
        self.assertIsNone(fitted)
        self.assertEqual(info['reason'], 'insufficient_control_budget')
        solve.assert_not_called()


if __name__ == '__main__':
    unittest.main()
