import unittest
import numpy as np
from scipy.interpolate import BSpline

from .mesh import Locator
from .splines import knots
from .network_fairing import Config, junctions, solve_network, line_candidate


class NetworkFairingTests(unittest.TestCase):
    def fixture(self):
        center = np.array([0., .04, 0.])
        ends = [np.array([-1., 0., 0.]), np.array([1., 0., 0.]), np.array([0., 1., 0.])]
        curves = {}; points = {}
        for i, end in enumerate(ends):
            s = np.linspace(0, 1, 9)
            cp = center[None]*(1-s[:, None])+end[None]*s[:, None]
            cp[:, 1 if i < 2 else 0] += .045*np.sin(4*np.pi*s)
            cp[0] = center; cp[-1] = end
            q = np.linspace(0, 1, 33); t = knots(9)
            curves[i] = dict(chain=np.r_[0, np.arange(10+i*40, 41+i*40), i+1], parameters=q,
                             poles=cp, knots=t, movement_budget_mm=.18)
            points[i] = BSpline(t, cp, 3)(q)
            tail = end+end/max(np.linalg.norm(end), 1e-30)*.5
            cp2 = np.linspace(end, tail, 4)
            curves[10+i] = dict(chain=np.array([i+1, 100+i]), parameters=np.array([0., 1.]),
                                 poles=cp2, knots=knots(4))
        v = np.array([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        loc = Locator(v, np.array([[0, 1, 2], [0, 2, 3]]))
        loc.source_normals = np.array([[0., 0., 1.], [0., 0., 1.]])
        return curves, points, {i: loc for i in range(3)}

    def test_network_moves_shared_junction_inside_mesh_tube(self):
        curves, points, locators = self.fixture()
        new, report = solve_network(curves, [0, 1, 2], points, locators, {},
                                    Config(movement=.18, tube=.004, fairness_length=.3, iterations=4))
        self.assertIsNotNone(new, report)
        self.assertTrue(report['accepted'])
        self.assertGreater(report['moved_junctions'], 0)
        self.assertTrue(np.array_equal(new[0]['poles'][0], new[1]['poles'][0]))
        self.assertTrue(np.array_equal(new[0]['poles'][0], new[2]['poles'][0]))
        for i in range(3):
            np.testing.assert_array_equal(new[i]['poles'][-1], curves[i]['poles'][-1])
            q = BSpline(new[i]['knots'], new[i]['poles'], 3)(np.linspace(0, 1, 1001))
            self.assertLess(locators[i].closest(q)[1].max(), .004)
        self.assertLess(report['totals']['after']['curvature_squared_integral_per_mm'],
                        report['totals']['before']['curvature_squared_integral_per_mm'])

    def test_three_crease_junction_is_fixed(self):
        curves, _, _ = self.fixture()
        result = junctions(curves, {0, 1, 2}, {0: True, 1: True, 2: True}, 30.)
        self.assertTrue(result[0]['fixed'])
        self.assertTrue(result[0]['feature_corner'])

    def test_inconsistent_shared_input_is_rejected(self):
        curves, _, _ = self.fixture(); curves[1]['poles'][0, 0] += .01
        with self.assertRaisesRegex(ValueError, 'shared junction'):
            junctions(curves, {0, 1, 2}, {}, 30.)

    def test_straight_model_removes_planar_noise_and_preserves_ends(self):
        curves, points, locators = self.fixture()
        c = curves[0]
        g = np.array([c['knots'][i+1:i+4].mean() for i in range(9)])
        c = dict(c, poles=(1-g[:, None])*c['poles'][0]+g[:, None]*c['poles'][-1])
        c['poles'][2:7, 1] += .005
        p = BSpline(c['knots'], c['poles'], 3)(c['parameters'])
        result, report = line_candidate(c, p, locators[0], .02, .004)
        self.assertTrue(report['accepted'])
        self.assertEqual(len(result['poles']), 4)
        np.testing.assert_array_equal(result['poles'][[0, -1]], c['poles'][[0, -1]])

    def test_straight_model_cannot_cut_across_a_hole_in_source_domain(self):
        v = np.array([[-1., -.1, 0.], [-.1, -.1, 0.], [-1., .1, 0.],
                      [.1, -.1, 0.], [1., -.1, 0.], [1., .1, 0.]])
        loc = Locator(v, np.array([[0, 1, 2], [3, 4, 5]]))
        p = np.array([[-.8, -.05, 0.], [.8, -.05, 0.]])
        c = dict(poles=np.linspace(*p, 4), knots=knots(4), parameters=np.array([0., 1.]))
        result, report = line_candidate(c, p, loc, .1, .01)
        self.assertIsNone(result)
        self.assertFalse(report['source_certificate']['passed'])


if __name__ == '__main__':
    unittest.main()
