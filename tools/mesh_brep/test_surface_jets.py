import unittest
import numpy as np
from .surface_jets import jets, COMPONENTS
from .splines import knots, tensor


class SurfaceJetsTests(unittest.TestCase):
    def test_shared_support_identity_survives_missing_metadata(self):
        from .shared_support import restore_groups
        rec=dict(origin=np.zeros(3),axes=np.eye(3),lo=np.zeros(2),size=np.ones(2),knots=knots(4),height=np.zeros(16))
        records={0:dict(rec),1:dict(rec),2:dict(rec,height=np.ones(16)*1e-6,shared_support_group=np.array([0,2]))}
        restore_groups(records)
        np.testing.assert_array_equal(records[0]['shared_support_group'],[0,1])
        np.testing.assert_array_equal(records[1]['shared_support_group'],[0,1])
        self.assertNotIn('shared_support_group',records[2])

    def test_seam_collocation_resolves_narrow_surface_spans_on_a_curved_parameterization(self):
        from .surface_jets import support_span_parameters
        curve = dict(knots=knots(4), poles=np.array([[0., .4, 0.], [0., .4, 0.], [0., .4, 0.], [1., .4, 0.]]),
                     parameters=np.array([0., 1.]))
        t = np.sort(np.r_[knots(4), .985, .99, .995])
        rec = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2), knots=t)
        q = support_span_parameters(curve, [rec])
        crossed = np.cbrt([.985, .99, .995])
        for cut in crossed:
            self.assertLess(abs(q-cut).min(), 1e-12)
        self.assertGreaterEqual(((q > crossed[0]) & (q < crossed[1])).sum(), 3)
        independent = support_span_parameters(curve, [rec], quadrature=True)
        self.assertFalse(np.array_equal(q, independent))

    def test_parallel_batches_never_optimize_both_sides_of_a_seam(self):
        from .global_continuity import independent_batches
        blocks = [[0, 1], [2], [3, 4], [5], [6]]
        pairs = [(0, 1), (1, 2), (2, 3), (4, 5), (5, 0), (6, 3)]
        curves = {i: dict(owners=pair) for i, pair in enumerate(pairs)}
        batches = independent_batches(blocks, curves)
        self.assertEqual(sorted(i for batch in batches for i in batch), list(range(len(blocks))))
        self.assertLess(len(batches), len(blocks))
        owner = {di: i for i, ids in enumerate(blocks) for di in ids}
        for batch in batches:
            for a, b in pairs:
                if owner[a] != owner[b]:
                    self.assertFalse(owner[a] in batch and owner[b] in batch)

    def test_global_blocks_cover_disconnected_components_and_keep_shared_support_atomic(self):
        from .global_continuity import neighborhoods
        r = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2), knots=knots(4), height=np.zeros(16))
        records = {i: dict(r) for i in range(8)}
        records[0]['shared_support_group'] = records[1]['shared_support_group'] = np.array([0, 1])
        curves = {i: dict(owners=np.array(pair), chain=np.array([0, 1]), knots=knots(4), parameters=np.array([0., 1.]),
                          poles=np.column_stack((np.linspace(0, 1, 4), np.ones(4)*.5, np.zeros(4))))
                  for i, pair in enumerate(((0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (6, 7)))}
        blocks, coverage = neighborhoods(records, curves, {(0, 1): 0.}, max_variables=64, max_charts=4)
        self.assertEqual(sorted(i for block in blocks for i in block), list(range(8)))
        self.assertEqual(len(blocks), 2)
        self.assertTrue(all(len(block) == 4 for block in blocks))
        self.assertTrue(any(0 in block and 1 in block for block in blocks))
        self.assertEqual(coverage['eligible_charts'], 8)
        subset,coverage=neighborhoods(records,curves,{(0,1):0.},max_variables=64,max_charts=4,selected_charts=[1,2])
        self.assertEqual(sorted(i for block in subset for i in block),[0,1,2])
        self.assertEqual(coverage['outside_requested_charts'],[3,4,5,6,7])

    def test_curvature_jacobian_in_rotated_anisotropic_frame(self):
        angle = .7
        axes = np.array([[np.cos(angle), 0., np.sin(angle)], [0., 1., 0.], [-np.sin(angle), 0., np.cos(angle)]])
        rng = np.random.default_rng(8)
        r = dict(origin=np.array([3., -2., 7.]), axes=axes, lo=np.array([-.3, .2]), size=np.array([2., .15]),
                 knots=knots(6), height=rng.normal(0, .01, 36))
        q = np.array([[.2, .7], [.7, .3], [.45, .55]])
        xyz = r['origin']+(q*r['size']+r['lo'])@axes[:2]
        n, s, nj, sj = jets(r, xyz, True)
        np.testing.assert_allclose(s, s.transpose(0, 2, 1), atol=1e-12)
        np.testing.assert_allclose(np.einsum('nij,nj->ni', s, n), 0., atol=1e-12)
        delta = rng.normal(size=36); eps = 1e-7
        np_, sp = jets(dict(r, height=r['height']+eps*delta), xyz)
        nm, sm = jets(dict(r, height=r['height']-eps*delta), xyz)
        np.testing.assert_allclose(np.column_stack([j@delta for j in nj]), (np_-nm)/(2*eps), atol=1e-7, rtol=1e-6)
        for (a, b), j in zip(COMPONENTS, sj):
            np.testing.assert_allclose(j@delta, (sp[:, a, b]-sm[:, a, b])/(2*eps), atol=1e-6, rtol=1e-6)

    def test_matching_tangent_planes_can_have_different_curvature(self):
        q = np.stack(np.meshgrid(np.linspace(0, 1, 5), np.linspace(0, 1, 5)), -1).reshape(-1, 2)
        t = knots(4); a = tensor(q, t, t).toarray()
        r = dict(origin=np.zeros(3), axes=np.eye(3), lo=-np.ones(2), size=np.ones(2)*2, knots=t)
        z = .5*(2*(2*q[:, 0]-1)**2+3*(2*q[:, 1]-1)**2)
        r['height'] = np.linalg.lstsq(a, z, rcond=None)[0]
        n, s = jets(r, np.zeros((1, 3)))
        n2, s2 = jets(dict(r, height=r['height']*2), np.zeros((1, 3)))
        np.testing.assert_allclose(n, n2, atol=1e-13)
        np.testing.assert_allclose(s[0], np.diag([2., 3., 0.]), atol=1e-12)
        self.assertGreater(np.linalg.norm(s-s2), 3.)


if __name__ == '__main__':
    unittest.main()
