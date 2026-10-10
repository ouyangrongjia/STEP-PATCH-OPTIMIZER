import unittest
import numpy as np
from .bounded_support import bending_matrix
from .splines import knots, tensor


class BendingEnergyTests(unittest.TestCase):
    def test_third_derivatives_preserve_quadratics_and_measure_cubic_variation(self):
        knot=knots(4); grid=np.linspace(0,1,4)
        q=np.stack(np.meshgrid(grid,grid,indexing='ij'),axis=-1).reshape(-1,2)
        size=np.array([3.,.2]); x,y=(q*size).T
        basis=tensor(q,knot,knot).toarray(); matrix=bending_matrix(knot,size,3)
        coefficients=np.linalg.solve(basis,x*x+2*x*y+3*y*y)
        self.assertLess(abs(float(coefficients@matrix@coefficients)),1e-5)
        coefficients=np.linalg.solve(basis,x**3+2*x*x*y+3*x*y*y+4*y**3)
        self.assertAlmostEqual(float(coefficients@matrix@coefficients),768.,places=5)

    def test_bicubic_mode_hidden_from_two_point_quadrature_has_positive_energy(self):
        knot = knots(4)
        grid = np.linspace(0, 1, 4)
        q = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        x, y = (2*q-1).T
        z = x**3*y**3-(x**3*y+x*y**3)/3-x*y/3
        coefficients = np.linalg.solve(tensor(q, knot, knot).toarray(), z)
        # Analytic integral on [-1,1]^2 is 48128/3150. The energy here is
        # averaged over that physical domain, whose area is four.
        energy = float(coefficients@bending_matrix(knot, np.array([2., 2.]))@coefficients)
        self.assertAlmostEqual(energy, 6016/1575, places=10)

    def test_affine_nullspace_and_physical_scale_on_nonuniform_knots(self):
        knot = np.sort(np.r_[knots(4), .03, .27, .61, .96])
        count = len(knot)-4
        g = np.array([np.mean(knot[i+1:i+4]) for i in range(count)])
        q = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
        size = np.array([3., .2]); x, y = (q*size).T
        basis = tensor(q, knot, knot).toarray()
        matrix = bending_matrix(knot, size)
        plane = np.linalg.solve(basis, 1+2*x-3*y)
        self.assertLess(abs(float(plane@matrix@plane)), 1e-5)
        paraboloid = np.linalg.solve(basis, x*x+2*x*y+3*y*y)
        self.assertAlmostEqual(float(paraboloid@matrix@paraboloid), 48., places=4)

    def test_trimmed_energy_integrates_polynomial_over_triangle_across_knots(self):
        from .trimmed_fairness import source_polygon, bending_matrix as trimmed_matrix
        uv = np.array([[0., 0.], [1., 0.], [0., 1.]])
        polygon = source_polygon(uv, np.array([[0, 1, 2]]))
        knot = np.sort(np.r_[knots(4), .23, .67])
        count = len(knot)-4
        g = np.array([np.mean(knot[i+1:i+4]) for i in range(count)])
        q = np.stack(np.meshgrid(g, g, indexing='ij'), axis=-1).reshape(-1, 2)
        coefficients = np.linalg.solve(tensor(q, knot, knot).toarray(), q[:, 0]**3*q[:, 1]**3)
        # Integral of x^a y^b over the unit right triangle is a! b!/(a+b+2)!.
        # h_xx^2+2h_xy^2+h_yy^2 = 36x^2y^6+162x^4y^4+36x^6y^2.
        import math
        integral = (72*math.factorial(2)*math.factorial(6)+162*math.factorial(4)**2)/math.factorial(10)
        matrix = trimmed_matrix(polygon, knot, np.ones(2))
        self.assertAlmostEqual(float(coefficients@matrix@coefficients), 2*integral, places=10)

    def test_trimmed_quadrature_excludes_hole(self):
        from shapely.geometry import Polygon
        from .trimmed_fairness import quadrature
        polygon = Polygon([(0,0), (1,0), (1,1), (0,1)], [[(.2,.3), (.8,.3), (.8,.7), (.2,.7)]])
        q, w = quadrature(polygon, np.sort(np.r_[knots(4), .13, .51, .83]))
        self.assertFalse(np.any((q[:,0] > .2)&(q[:,0] < .8)&(q[:,1] > .3)&(q[:,1] < .7)))
        self.assertAlmostEqual(float(w@(q[:,0]**2)), (1/3-(.8**3-.2**3)/3*.4)/.76, places=11)


if __name__ == '__main__':
    unittest.main()
