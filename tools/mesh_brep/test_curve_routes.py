import unittest
import numpy as np
from scipy.interpolate import BSpline
from .curve_routes import Config, restrict, route_graph, fit_master
from .splines import knots
from .mesh import Locator


class CurveRoutesTest(unittest.TestCase):
    def test_children_are_exact_restrictions_through_second_derivative(self):
        rng = np.random.default_rng(32)
        cp = rng.normal(size=(10, 3)); t = knots(10)
        master = BSpline(t, cp, 3)
        for a, b, forward in ((0., .381, True), (.381, .812, False), (.812, 1., True)):
            p, k = restrict(cp, t, a, b, forward); child = BSpline(k, p, 3)
            q = np.linspace(0., 1., 31); u = a+(b-a)*q if forward else b-(b-a)*q
            for d in (0, 1, 2):
                np.testing.assert_allclose(child(q, d), master(u, d)*((b-a)*(1 if forward else -1))**d,
                                           rtol=1e-10, atol=2e-11)

    def test_chart_junction_is_not_master_interpolation_constraint(self):
        v = np.array([[0, 0, 0], [.25, .02, 0], [.5, .04, 0], [.75, -.02, 0], [1, 0, 0]], float)
        curves = {0: dict(chain=np.array([0, 1, 2]), owners=[0, 1]),
                  1: dict(chain=np.array([4, 3, 2]), owners=[2, 1])}
        route = route_graph(curves, {0: 3, 1: 4, 2: 3}, v, Config())
        self.assertEqual(len(route), 1)
        np.testing.assert_array_equal(route[0]['chain'], np.arange(5))
        plane = np.array([[-1, -1, 0], [2, -1, 0], [2, 1, 0], [-1, 1, 0]], float)
        locator = Locator(plane, np.array([[0, 1, 2], [0, 2, 3]]))
        master, _ = fit_master(v, v[[0, -1]], locator, Config())
        self.assertIsNotNone(master)
        self.assertGreater(np.linalg.norm(BSpline(master['knots'], master['poles'], 3)(master['parameters'][2])-v[2]), .01)

    def test_true_corner_stops_route(self):
        v = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0]], float)
        cs = {0: dict(chain=np.array([0, 1]), owners=[0, 1]), 1: dict(chain=np.array([1, 2]), owners=[2, 1])}
        self.assertEqual(len(route_graph(cs, {0: 3, 1: 4, 2: 3}, v, Config())), 2)

    def test_internal_route_keeps_existing_master_junction_fixed(self):
        v = np.array([[0, 0, 0], [1, 0, 0], [2, 0, 0]], float)
        cs = {0: dict(chain=np.array([0, 1]), owners=[0, 1]),
              1: dict(chain=np.array([1, 2]), owners=[2, 1])}
        regions = {0: 3, 1: 3, 2: 3}
        cfg = Config(interface_kind='internal')
        self.assertEqual(len(route_graph(cs, regions, v, cfg)), 1)
        self.assertEqual(len(route_graph(cs, regions, v, cfg, fixed_nodes={1})), 2)
        self.assertEqual(route_graph(cs, regions, v, Config()), [])

    def test_redundant_junction_removed_without_changing_master(self):
        from .route_compact import coalesce
        cp = np.array([[0, 0, 0], [.3, .2, .1], [.7, -.1, .1], [1, 0, 0]], float)
        t = knots(4); curves = {}
        for ci, (a, b, forward) in enumerate(((0., .4, True), (.4, 1., False))):
            p, k = restrict(cp, t, a, b, forward)
            curves[ci] = dict(poles=p, knots=k, route_id=7, route_interval=np.array([a, b]),
                              route_forward=forward, chain=np.array([0, 1] if ci == 0 else [2, 1]),
                              owners=np.array([3, 4]), parameters=np.array([0., 1.]), movement_mm=.02)
        result, events = coalesce(curves, {7: dict(poles=cp, knots=t)})
        self.assertEqual(len(result), 1); self.assertEqual(len(events), 1)
        c = next(iter(result.values())); np.testing.assert_array_equal(c['chain'], [0, 1, 2])
        q = np.linspace(0, 1, 101)
        np.testing.assert_allclose(BSpline(c['knots'], c['poles'], 3)(q), BSpline(t, cp, 3)(q), atol=1e-13)
        curves[1]['owners'] = np.array([3, 5])
        self.assertEqual(len(coalesce(curves, {7: dict(poles=cp, knots=t)})[0]), 2)


if __name__ == '__main__':
    unittest.main()
