import unittest
import numpy as np
from scipy.interpolate import BSpline
from .feature_boundary import fair_curve, subcurve
from .mesh import chain_parameters
from .splines import knots


class FeatureBoundaryTests(unittest.TestCase):
    def test_boundary_targets_share_smooth_normals_and_exclude_crease_sides(self):
        from .boundary_normals import targets
        v = np.array([[0., 0, 0], [1., 0, 0], [1., 1, 0], [0., 1, 0]])
        f = np.array([[0, 1, 2], [0, 2, 3]])
        rec = dict(chain=np.array([0, 2]), parameters=np.array([0., 1.]),
                   poles=np.linspace(v[0], v[2], 4), knots=knots(4))
        n = np.tile([0., 0., 1.], (2, 3, 1))
        args = (v, f, n, np.array([2, 5]), np.array([[0, 2]]), np.array([[0, 1]]), [rec], [[2, 5]])
        smooth = targets(*args)[0]
        np.testing.assert_allclose(smooth[0], smooth[1], atol=1e-12)
        n[1] = np.array([0., 1., 1.])/np.sqrt(2)
        sharp = targets(*args)[0]
        np.testing.assert_allclose(sharp, 0., atol=1e-12)

    def test_interior_probe_detects_a_peak_hidden_from_vertices_and_centroid(self):
        from .support_probe import check
        from .splines import tensor
        knot = knots(4); grid = np.linspace(0, 1, 4)
        q = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        z = q[:, 0]*q[:, 1]*(q.sum(axis=1)-2/3)
        height = np.linalg.solve(tensor(q, knot, knot).toarray(), z)
        record = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2), knots=knot, height=height)
        source = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0]])
        fitted_samples = np.r_[source[:, :2], [[1/3, 1/3]]]
        self.assertLess(abs(tensor(fitted_samples, knot, knot)@height).max(), 1e-12)
        _, result = check((0, record, source, np.array([[0, 1, 2]])))
        self.assertGreater(result['interior_probe_max_mm'], .03)

    def test_overlap_candidates_require_a_distinct_geometric_root(self):
        from .boundary_checks import distinct_intersection
        monotone = BSpline(knots(4), np.c_[[0., .25, 0., .25], np.zeros(4)], 3)
        self.assertFalse(distinct_intersection(monotone, .48, .52))
        epsilon = .005
        power = np.array([[.25, -.125+.5*epsilon**2], [-1., .75-epsilon**2], [1., -1.5], [0., 1.]])
        poles = np.array([power[0], power[0]+power[1]/3,
                          power[0]+2*power[1]/3+power[2]/3, power.sum(axis=0)])
        loop = BSpline(knots(4), poles, 3)
        self.assertTrue(distinct_intersection(loop, .5-epsilon*1.05, .5+epsilon*1.05))

    def test_continuous_guard_rejects_loops_and_crossings_but_keeps_shared_junctions(self):
        from .boundary_checks import intersections
        frame = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.array([-2., -2.]), size=np.array([4., 4.]))
        def line(a, b, ids):
            return dict(poles=np.linspace(a, b, 4), knots=knots(4), chain=np.array(ids))
        a = line([0., 0., 0.], [1., 0., 0.], [0, 1])
        b = line([1., 0., 0.], [1., 1., 0.], [1, 2])
        self.assertEqual(intersections([a, b], [0, 1], frame), set())
        c = line([.5, -1., 0.], [.5, 1., 0.], [3, 4])
        self.assertEqual(intersections([a, c], [0, 1], frame), {0, 1})
        loop = dict(poles=np.array([[0., 0., 0.], [1., 1., 0.], [-1., 1., 0.], [.2, 0., 0.]]),
                    knots=knots(4), chain=np.array([0, 1]))
        self.assertEqual(intersections([loop], [0], frame), {0})

    def test_restriction_preserves_positions_and_derivatives_in_both_directions(self):
        poles = np.random.default_rng(8402).normal(size=(12, 3))
        knot = knots(len(poles)); parent = BSpline(knot, poles, 3)
        q = np.linspace(0, 1, 257)
        for start, end in [(0., 1.), (.13, .79), (.91, .02)]:
            cp, kt = subcurve(poles, knot, start, end)
            child = BSpline(kt, cp, 3)
            for derivative in (0, 1, 2):
                np.testing.assert_allclose(child(q, derivative),
                    parent(start+(end-start)*q, derivative)*(end-start)**derivative,
                    atol=2e-10, rtol=2e-11)

    def test_subdivision_does_not_pin_a_new_junction_to_a_raster_vertex(self):
        p = np.c_[np.linspace(0, 3, 49), .025*(-1.)**np.arange(49), np.zeros(49)]
        t = chain_parameters(p, np.arange(len(p)))
        normals = np.tile([[[0., 0, 1], [0., 0, 1]]], (len(p)-1, 1, 1))
        cp, kt, error, info = fair_curve(p, t, .12, normals, .001)
        self.assertEqual(info['method'], 'minimum_bending')
        self.assertLess(error, .12)
        parent = BSpline(kt, cp, 3); cut = float(t[23])
        lp, lk = subcurve(cp, kt, 0., cut); rp, rk = subcurve(cp, kt, cut, 1.)
        left = BSpline(lk, lp, 3); right = BSpline(rk, rp, 3)
        np.testing.assert_allclose(left(1), right(0), atol=1e-12)
        np.testing.assert_allclose(left(1, 1)/cut, right(0, 1)/(1-cut), atol=1e-10)
        self.assertGreater(np.linalg.norm(parent(cut)-p[23]), .01)
        self.assertLess(np.linalg.norm(parent(np.linspace(0, 1, 101), 2), axis=1).max(), .01)

    def test_closed_label_loop_survives_different_internal_chart_breaks(self):
        from .feature_boundary import FeatureBoundaryNetwork
        from .partition_fit import incidence
        from .skeleton_graph import skeleton
        angle = np.arange(8)*np.pi/4
        v = np.r_[np.c_[np.cos(angle), np.sin(angle), np.zeros(8)], [[0., 0., 1.], [0., 0., -1.]]]
        f = np.array([[8, i, (i+1) % 8] for i in range(8)]+
                     [[9, (i+1) % 8, i] for i in range(8)])
        labels = np.repeat([0, 1], 8); edges, owners = incidence(f)
        network = FeatureBoundaryNetwork(v, f, labels, edges, owners, .05, .003)
        domains = [dict(region=int(labels[ids[0]]), triangle_ids=np.array(ids),
                        vertex_ids=np.unique(f[ids])) for ids in [[0, 1], [2, 3, 4, 5], [6, 7], list(range(8, 16))]]
        _, curves, pairs, _, errors = skeleton(v, f, domains, edges, owners, .05,
                                               boundary_network=network)
        endpoints = {}
        for c in curves:
            for vi, p in zip(c['chain'][[0, -1]], c['poles'][[0, -1]]):
                if int(vi) in endpoints:
                    np.testing.assert_allclose(p, endpoints[int(vi)], atol=1e-12)
                endpoints[int(vi)] = p
        self.assertLessEqual(max(errors), .05)
        self.assertTrue(any(int(c['parent_interface']) >= 0 for c in curves))

    def test_bounded_supports_produce_a_closed_step_with_unchanged_labels(self):
        import tempfile
        from pathlib import Path
        from .mesh import write_stl
        from .partition_fit import Config
        from .skeleton_reconstruct import reconstruct
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); write_stl(root/'source.stl', v, f)
            np.savez_compressed(root/'partition.npz', mesh_vertex_original=v,
                                mesh_triangle=f, face_instance_id=np.arange(4))
            result = reconstruct(root/'source.stl', root/'partition.npz', root/'result',
                reference_faces=5, workers=1, rounds=2, render=False, smoothing=.05,
                boundary_mode='fair-network', normal_field='crease-fans', support_solver='bounded',
                repair_smooth_joins=True,
                config=Config(distance=.001, boundary=.001, controls=(15,), normal_weight=.01,
                              fairness=1e-6))
            self.assertTrue(result['step']['watertight'])
            self.assertEqual(result['shared_support_repair']['accepted_groups'], [])
            self.assertTrue(result['fitting']['partition_labels_unchanged'])
            self.assertEqual(result['step']['topology']['faces'], 4)
            self.assertAlmostEqual(result['step']['topology']['volume_mm3'], 1/6, places=5)

    def test_local_common_support_removes_a_false_join_without_changing_region_ownership(self):
        from .shared_support import _proposal, seam_samples
        from .partition_fit import Config
        v = np.array([[0., 0, 0], [1., 0, 0], [1., 1, 0], [0., 1, 0]])
        tri = np.array([[0, 1, 2], [0, 2, 3]])
        knot = knots(9); g = np.array([np.mean(knot[i+1:i+4]) for i in range(9)])
        x, y = np.meshgrid(g, g, indexing='ij')
        records = {i: dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                          knots=knot, height=((x-y)*(1 if i else -1)).ravel(),
                          region=np.array(i+1), union_triangle_ids=np.array([i])) for i in range(2)}
        curve = dict(chain=np.array([0, 2]), parameters=np.array([0., 1.]), owners=np.array([0, 1]),
                     knots=knots(4), poles=np.linspace(v[0], v[2], 4))
        _, before = seam_samples(curve, records)
        self.assertGreater(before.min(), 90)
        normals = np.tile([0., 0, 1], (2, 3, 1))
        fitted, info = _proposal(([0, 1], v, tri, normals, {0: curve}, records,
            Config(distance=.03, boundary=.005, controls=(15,), normal_weight=.01, fairness=1e-6), {(0, 2): 0.}))
        self.assertTrue(info['accepted'])
        self.assertLess(info['join_score_after'], 1e-8)
        self.assertEqual([int(records[i]['region']) for i in records], [1, 2])
        self.assertLess(abs(fitted['height']).max(), 1e-8)

    def test_common_support_groups_follow_actual_surfaces_after_chart_renumbering(self):
        from .shared_support import restore_groups
        template = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                        knots=knots(4), height=np.zeros(16), shared_support_group=np.array([1, 2]))
        records = {10: dict(template), 20: dict(template), 30: dict(template, height=np.ones(16))}
        self.assertEqual(restore_groups(records), {10, 20, 30})
        np.testing.assert_array_equal(records[10]['shared_support_group'], [10, 20])
        np.testing.assert_array_equal(records[20]['shared_support_group'], [10, 20])
        self.assertNotIn('shared_support_group', records[30])


if __name__ == '__main__':
    unittest.main()
