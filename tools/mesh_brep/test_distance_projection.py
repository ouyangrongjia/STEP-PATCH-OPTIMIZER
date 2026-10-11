import unittest
import numpy as np
from .distance_projection import project, continuity_change, refine, damp
from .partition_fit import Config, parameters, positions, spline_graph_error
from .splines import knots, tensor


class DistanceProjectionTests(unittest.TestCase):
    def test_one_analytic_step_balances_a_tiny_distance_and_boundary_excess(self):
        v, f, old, curves, _ = self.fixture()
        t = old['knots']; g = np.array([t[i+1:i+4].mean() for i in range(len(t)-4)])
        q = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
        bubble = 16*q[:, 0]*(1-q[:, 0])*q[:, 1]*(1-q[:, 1]); basis = tensor(q, t, t).toarray()
        old['height'] = np.linalg.solve(basis, .0021+(.026-.0021)*bubble)
        proposal = dict(old, height=np.linalg.solve(basis, .0015+(.031-.0015)*bubble))
        grid = np.linspace(0, 1, 41)
        probes = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        before = max(spline_graph_error(c['poles'], c['knots'], old) for c in curves)
        after = max(spline_graph_error(c['poles'], c['knots'], proposal) for c in curves)
        result, info = damp(old, proposal, probes, v, f, .03, before, after, .002)
        self.assertIsNotNone(result)
        self.assertEqual(info['solver_calls'], 0)
        self.assertLess(abs(positions(probes, result)[:, 2]).max(), .03)
        self.assertLess(max(spline_graph_error(c['poles'], c['knots'], result) for c in curves), .002)
        np.testing.assert_array_equal(old['knots'], result['knots'])
        # There is no admissible step if both endpoints exceed the real budget.
        bad_old = dict(old, height=np.full_like(old['height'], .04))
        bad_new = dict(old, height=np.full_like(old['height'], .05))
        rejected, rejected_info = damp(bad_old, bad_new, probes, v, f, .03, .0021, .0015, .002)
        self.assertIsNone(rejected)
        self.assertFalse(rejected_info['proposed'])

    def test_refinement_preserves_geometry_normals_and_curvature(self):
        from .surface_jets import jets
        _, _, record, _, _ = self.fixture()
        record['height'] = np.random.default_rng(41).normal(0, .02, len(record['height']))
        refined = refine(record, 21)
        self.assertEqual(len(refined['knots'])-4, 21)
        q = np.random.default_rng(18).random((97, 2)); xyz = positions(q, record)
        np.testing.assert_allclose(positions(q, refined), xyz, rtol=0, atol=1e-12)
        for a, b in zip(jets(record, xyz), jets(refined, xyz)):
            np.testing.assert_allclose(a, b, rtol=0, atol=1e-10)
        self.assertIsNone(refine(refined, 21))

    def fixture(self, count=9):
        vertices = np.array([[0., 0, 0], [1., 0, 0], [1., 1, 0], [0., 1, 0]])
        triangles = np.array([[0, 1, 2], [0, 2, 3]])
        record = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2),
                      size=np.ones(2), knots=knots(count), height=np.zeros(count*count))
        curves = [dict(poles=np.linspace(vertices[a], vertices[b], 4), knots=knots(4),
                       chain=np.array([a, b]), parameters=np.array([0., 1.]), owners=np.array([0, 1]))
                  for a, b in ((0, 1), (1, 2), (2, 3), (3, 0))]
        lookup = {tuple(sorted(map(int, c['chain']))): 0. for c in curves}
        return vertices, triangles, record, curves, lookup

    def test_distance_bump_repair_preserves_boundary_jets_and_basis(self):
        v, f, record, curves, lookup = self.fixture(13)
        record['height'][6*13+6] = .08
        q = np.stack(np.meshgrid(np.linspace(0, 1, 41), np.linspace(0, 1, 41)), axis=-1).reshape(-1, 2)
        xyz = positions(q, record)
        self.assertGreater(xyz[:, 2].max(), .03)
        cfg = Config(distance=.01, boundary=.001, fairness=1e-8)
        candidate, info = project(record, v, f, curves, xyz, cfg)
        self.assertIsNotNone(candidate, info)
        self.assertLess(np.abs(positions(parameters(xyz, candidate), candidate)[:, 2]).max(), .01)
        for key in ('origin', 'axes', 'lo', 'size', 'knots'):
            np.testing.assert_array_equal(record[key], candidate[key])
        self.assertLess(max(spline_graph_error(c['poles'], c['knots'], candidate) for c in curves), cfg.boundary*.4)
        flat = dict(record, height=np.zeros_like(record['height']))
        references = {0: record, 1: flat}
        result = continuity_change([0], candidate, references, references, curves, lookup)
        self.assertTrue(result['passed'], result)

    def test_distance_only_candidate_cannot_introduce_a_normal_jump(self):
        _, _, record, curves, lookup = self.fixture()
        t = record['knots']; g = np.array([t[i+1:i+4].mean() for i in range(len(t)-4)])
        candidate = dict(record, height=np.repeat(.1*g, len(g)))
        references = {0: record, 1: record}
        result = continuity_change([0], candidate, references, references, curves, lookup)
        self.assertFalse(result['passed'])
        self.assertGreater(result['worst_normal_p95_increase_degrees'], 5.)

    def test_curvature_jump_is_rejected_even_with_matching_edge_normals(self):
        _, _, record, curves, lookup = self.fixture()
        t = record['knots']; count = len(t)-4
        quadratic = np.array([(t[i+1]*t[i+2]+t[i+1]*t[i+3]+t[i+2]*t[i+3])/3 for i in range(count)])
        candidate = dict(record, height=np.tile(quadratic, count))
        references = {0: record, 1: record}
        result = continuity_change([0], candidate, references, references, curves[:1], lookup)
        self.assertFalse(result['passed'])
        self.assertLess(result['worst_normal_p95_increase_degrees'], 1e-5)
        self.assertGreater(result['worst_shape_p95_increase_per_mm'], 1.9)


if __name__ == '__main__':
    unittest.main()
