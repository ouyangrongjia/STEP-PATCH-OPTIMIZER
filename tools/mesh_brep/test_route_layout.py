import unittest
import numpy as np
from .route_layout import merge_candidates


class RouteLayoutTests(unittest.TestCase):
    def test_small_chart_can_only_merge_with_same_region_neighbor(self):
        v = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], float)
        f = np.array([[0, 1, 2], [0, 2, 3]])
        charts = {0: dict(region=7, triangle_ids=np.array([0])), 1: dict(region=7, triangle_ids=np.array([1]))}
        curves = {0: dict(owners=np.array([0, 1]))}
        candidates = merge_candidates(0, charts, curves, v, f)
        self.assertEqual(len(candidates), 1)
        np.testing.assert_array_equal(candidates[0][1], [0, 1])
        charts[1]['region'] = 8
        self.assertEqual(merge_candidates(0, charts, curves, v, f), [])

    def test_folded_union_is_not_a_height_chart(self):
        v = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, .5, 0]], float)
        f = np.array([[0, 1, 2], [1, 0, 3]])
        charts = {0: dict(region=7, triangle_ids=np.array([0])), 1: dict(region=7, triangle_ids=np.array([1]))}
        self.assertEqual(merge_candidates(0, charts, {0: dict(owners=np.array([0, 1]))}, v, f), [])


if __name__ == '__main__':
    unittest.main()
