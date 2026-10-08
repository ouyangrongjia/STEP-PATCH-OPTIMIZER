"""Boundary-domain and curve reduction regressions for regional fitting."""
import unittest
import numpy as np
from .regional_rectangle import SquareChart, compact_curve, concatenate
from .mesh import chain_parameters, vertex_normals


class RegionalRectangleTests(unittest.TestCase):
    def test_convex_chart_retains_nonzero_area_boundary_ear(self):
        vertices = np.array([[0., 0, 0], [.5, -.2, 0], [1, 0, 0],
                             [1, 1, 0], [0, 1, 0], [.5, .5, 0]])
        triangles = np.array([[0, 1, 2], [0, 2, 5], [2, 3, 5], [3, 4, 5], [4, 0, 5]])
        domain = dict(vertices=np.arange(6), triangles=np.arange(5),
                      loop=np.arange(5), corners=np.array([0, 2, 3, 4]))
        normals = vertex_normals(vertices, triangles)
        flat = SquareChart(vertices, triangles, domain, normals)
        self.assertGreater(flat.report['collapsed_source_area_fraction'], 0)
        rounded = SquareChart(vertices, triangles, domain, normals, roundness=.08)
        self.assertEqual(rounded.report['collapsed_boundary_triangles'], 0)
        # Every source triangle, including the boundary ear, remains queryable.
        query = rounded.uv[triangles].mean(axis=1)
        positions, _ = rounded.query(query)
        np.testing.assert_allclose(positions, vertices[triangles].mean(axis=1), atol=1e-12)

    def test_compact_shared_curve_preserves_endpoints_and_concatenation(self):
        x = np.linspace(0, 1, 41)
        points = np.c_[x, .04*np.sin(2*np.pi*x), .01*x*x]
        parameters = chain_parameters(points, np.arange(len(points)))
        tolerance = .002
        first, _ = compact_curve(points, parameters, tolerance)
        t = np.linspace(0, 1, 2001)
        expected = np.stack([np.interp(t, parameters, points[:, k]) for k in range(3)], axis=1)
        actual = np.array([first.Value(float(s)).Coord() for s in t])
        self.assertLess(np.linalg.norm(actual-expected, axis=1).max(), tolerance)
        np.testing.assert_allclose(actual[[0, -1]], points[[0, -1]], atol=1e-14)
        self.assertLess(first.NbPoles(), len(points)//2)
        second_points = np.array([points[-1], points[-1]+[.3, .1, .02]])
        second, _ = compact_curve(second_points, np.array([0., 1.]), tolerance)
        joined, ranges = concatenate([first, second], [1., .3])
        for curve, (a, b) in zip([first, second], ranges):
            errors = [joined.Value(float(a+(b-a)*s)).Distance(curve.Value(float(s)))
                      for s in np.linspace(0, 1, 101)]
            self.assertLess(max(errors), 1e-12)

    def test_shared_curve_endpoint_tangents_use_common_source_planes(self):
        from OCP.gp import gp_Pnt, gp_Vec
        angle = np.linspace(0, np.pi/2, 25)
        points = np.c_[np.cos(angle), np.sin(angle), angle*0]
        normals = points[[0, -1]]
        curve, _ = compact_curve(points, angle/(np.pi/2), .004, normals)
        for t, normal in zip([0., 1.], normals):
            p, tangent = gp_Pnt(), gp_Vec()
            curve.D1(t, p, tangent)
            self.assertLess(abs(np.dot(tangent.Coord(), normal)), 1e-10)


if __name__ == '__main__':
    unittest.main()
