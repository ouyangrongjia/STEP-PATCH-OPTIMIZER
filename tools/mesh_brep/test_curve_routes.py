import unittest
import numpy as np
from scipy.interpolate import BSpline
from .curve_routes import Config, restrict, route_graph, fit_master
from .splines import knots
from .mesh import Locator


class CurveRoutesTest(unittest.TestCase):
    def test_revisited_branch_node_splits_route_before_moving_shared_endpoint(self):
        v = np.array([[0.,0,0],[.5,0,0],[.5,.5,0],[0,.5,0],[-1,0,0]])
        curves = {0: dict(chain=np.array([0,1,2,3,0]), owners=[0,1]),
                  1: dict(chain=np.array([0,4]), owners=[0,2])}
        routes = route_graph(curves, {0:0,1:1,2:2}, v, Config(cross_region_junctions=True))
        self.assertEqual(len(routes), 2)
        self.assertEqual(sorted(ci for r in routes for ci, _ in r['members']), [0,1])
        for route in routes:
            self.assertEqual(len(route['members']), 1)
        # A simple isolated ring retains its periodic interpretation.
        theta = np.linspace(0., 2*np.pi, 8, endpoint=False)
        circle = np.c_[np.cos(theta), np.sin(theta), np.zeros(len(theta))]
        ring = route_graph({0: dict(chain=np.r_[np.arange(8), 0], owners=[0,1])},
                           {0:0,1:1}, circle, Config(corner_degrees=89.))
        self.assertEqual(len(ring), 1)
        self.assertTrue(ring[0]['closed'])

    def test_admissible_curve_still_minimizes_source_normal_offset(self):
        x = np.linspace(-1., 1., 161)
        points = np.c_[x, .015*np.sin(12*np.pi*x), .2*x*x]
        vertices = np.array([[u, v, .2*u*u] for u in x for v in (-.5, .5)])
        triangles = np.array([[2*i, 2*i+2, 2*i+1] for i in range(len(x)-1)]
                             +[[2*i+1, 2*i+2, 2*i+3] for i in range(len(x)-1)])
        locator = Locator(vertices, triangles)
        loose, _ = fit_master(points, points[[0, -1]], locator,
            Config(movement=.08, tube=.012, projection_rms_fraction=.84))
        close, trials = fit_master(points, points[[0, -1]], locator,
            Config(movement=.08, tube=.012))
        self.assertIsNotNone(loose); self.assertIsNotNone(close)
        q = np.linspace(0., 1., 1001)
        def rms(master):
            distance = locator.closest(BSpline(master['knots'], master['poles'], 3)(q))[1]
            return float(np.sqrt(np.mean(distance**2)))
        self.assertLess(rms(close), .5*rms(loose))
        self.assertEqual(len(close['poles']), len(loose['poles']))
        self.assertLessEqual(len(trials[-1]['projection_rms_history_mm']), 9)
        np.testing.assert_allclose(close['poles'][[0, -1]], points[[0, -1]], atol=1e-13)

    def test_crease_master_smooths_along_ridge_but_cannot_slide_onto_one_side(self):
        from .skeleton_graph import normal_error
        x = np.linspace(0., 1., 61)
        points = np.c_[x, .001*np.sin(12*np.pi*x), np.zeros(len(x))]
        normals = np.tile([[[0., 1., 0.], [0., 0., 1.]]], (len(x)-1, 1, 1))
        vertices = np.array([[-1., -1., 0.], [2., -1., 0.], [2., 1., 0.], [-1., 1., 0.]])
        locator = Locator(vertices, np.array([[0, 1, 2], [0, 2, 3]]))
        cfg = Config(movement=.08, tube=.02, maximum_controls=8)
        master, _ = fit_master(points, points[[0, -1]], locator, cfg, plane_normals=normals)
        self.assertIsNotNone(master)
        self.assertEqual(len(master['poles']), 4)
        error, _ = normal_error(master['poles'], master['knots'], points, master['parameters'], normals)
        self.assertLess(error, cfg.crease_plane_tolerance)
        moved = points[[0, -1]]+np.array([0., .008, 0.])
        unprotected, _ = fit_master(points, moved, locator, cfg)
        self.assertIsNotNone(unprotected)
        protected, trials = fit_master(points, moved, locator, cfg, plane_normals=normals)
        self.assertIsNone(protected)
        self.assertTrue(any(r.get('reason') == 'incident_planes' for r in trials))

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

    def test_periodic_route_has_no_artificial_corner(self):
        theta = np.linspace(0, 2*np.pi, 81)
        radius = 1.+.015*np.sin(17*theta)
        points = np.column_stack((radius*np.cos(theta), radius*np.sin(theta), np.zeros(len(theta))))
        plane = np.array([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        locator = Locator(plane, np.array([[0, 1, 2], [0, 2, 3]]))
        master, _ = fit_master(points, points[[0, -1]], locator, Config(movement=.06, maximum_controls=16), closed=True)
        self.assertIsNotNone(master)
        spline = BSpline(master['knots'], master['poles'], 3)
        for d in (0, 1, 2):
            np.testing.assert_allclose(spline(0., d), spline(1., d), atol=1e-9)
        self.assertLess(np.max(np.abs(np.linalg.norm(spline(np.linspace(0, 1, 301)), axis=1)-1)), .035)

    def test_dominant_junction_crosses_region_pair_change(self):
        v = np.array([[-1., 0., 0.], [0., .04, 0.], [1., 0., 0.], [0., 1., 0.]])
        cs = {0: dict(chain=np.array([0, 1]), owners=[0, 1]), 1: dict(chain=np.array([1, 2]), owners=[0, 2]),
              2: dict(chain=np.array([1, 3]), owners=[1, 2])}
        routes = route_graph(cs, {0: 3, 1: 4, 2: 5}, v, Config(cross_region_junctions=True))
        self.assertEqual(sorted(len(r['members']) for r in routes), [1, 2])
        np.testing.assert_array_equal(next(r for r in routes if len(r['members']) == 2)['chain'], [0, 1, 2])

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
