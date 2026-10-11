import unittest
import numpy as np
from .bounded_support import bending_matrix
from .splines import knots, tensor


class BendingEnergyTests(unittest.TestCase):
    def test_short_penalty_proposal_respects_box_geometry_without_claiming_qp_convergence(self):
        from .bounded_support import penalty_proposal, solve
        from .partition_fit import Config
        from scipy import sparse
        identity = sparse.eye(3, format='csc')
        value, report = penalty_proposal(identity, np.array([2., -3., .5]), identity,
            np.full(3, -1.), np.ones(3), np.full(3, 1e6), np.array([2., -3., .5]), 1.)
        np.testing.assert_allclose(value, [1., -1., .5], atol=2.1e-6)
        self.assertLessEqual(report['iterations'], 6)
        knot = knots(9); axis = np.linspace(0, 1, 17)
        q = np.stack(np.meshgrid(axis, axis, indexing='ij'), axis=-1).reshape(-1, 2)
        on_boundary = np.any((q == 0)|(q == 1), axis=1)
        a = tensor(q, knot, knot); b = a[on_boundary]
        target = .006*np.sin(np.pi*q[on_boundary, 0])
        cfg = Config(distance=.027, normal_weight=.01, fairness=1e-6)
        height, info = solve(a, np.zeros(len(q)), b, target, q, np.tile([0., 0., 1.], (len(q), 1)),
            knot, np.ones(2), cfg, .002, quick_penalty=True, equilibrated_retry=False)
        self.assertEqual(info['proposal_source'], 'short_penalty_newton')
        self.assertFalse(info['solved'])
        self.assertLess(abs(a@height).max(), .027)
        self.assertLess(abs(b@height-target).max(), .0014)

    def test_inexact_qp_is_not_replaced_by_a_worse_warm_start(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        from .bounded_support import solve
        from .partition_fit import Config
        knot = knots(4); grid = np.linspace(0, 1, 5)
        q = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        a = tensor(q, knot, knot); z = np.zeros(len(q))
        result = SimpleNamespace(x=np.full(16, .028), info=SimpleNamespace(status='run time limit reached',
            status_val=8, run_time=.1, iter=10))
        with patch('osqp.OSQP') as solver, patch('tools.mesh_brep.bounded_support.spsolve',
                side_effect=[np.full(16, .04), np.full(16, .031)]):
            solver.return_value.solve.return_value = result
            height, report = solve(a, z, a, z, q, np.tile([0., 0, 1], (len(q), 1)), knot,
                np.ones(2), Config(distance=.027), .05, equilibrated_retry=False, retain_inexact=True)
        self.assertFalse(report['solved'])
        self.assertEqual(report['proposal_source'], 'inexact_qp_pending_geometric_check')
        np.testing.assert_allclose(height, .028)

    def test_single_projection_corrects_small_excess_without_basis_growth(self):
        from .bounded_support import correct
        knot = knots(4); grid = np.linspace(0, 1, 9)
        q = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        a = tensor(q, knot, knot); coeff = np.full(16, .0302)
        corrected, info = correct(coeff, a, np.zeros(len(q)), a, np.full(len(q), .028),
                                  .03, .002, seconds=.5)
        self.assertIsNotNone(corrected)
        self.assertEqual(corrected.shape, coeff.shape)
        self.assertEqual(info['attempts'], 1)
        self.assertLess(abs(a@corrected).max(), .03)
        self.assertLess(abs(a@corrected-.028).max(), .002)
        bad, rejected = correct(coeff, a, np.zeros(len(q)), a, np.full(len(q), .06),
                                .03, .002, seconds=.5)
        self.assertIsNone(bad)
        self.assertFalse(rejected['proposed'])

    def test_stalled_qp_only_reuses_warm_proposal_inside_actual_constraints(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        from .bounded_support import solve
        from .partition_fit import Config
        knot = knots(4); grid = np.linspace(0, 1, 7)
        q = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
        a = tensor(q, knot, knot); z = np.zeros(len(q))
        normals = np.tile([0., 0., 1.], (len(q), 1))
        result = SimpleNamespace(x=None, info=SimpleNamespace(status='run time limit reached',
                                 status_val=8, run_time=.1, iter=10))
        for target, accepted in ((.02, True), (.06, False)):
            with self.subTest(target=target), patch('osqp.OSQP') as solver:
                solver.return_value.solve.return_value = result
                height, report = solve(a, z, a, np.full(len(q), target), q, normals,
                                       knot, np.ones(2), Config(), .002)
                self.assertEqual(report['solved'], accepted)
                self.assertEqual(report['status'], 'run time limit reached')
                self.assertEqual(report['proposal_source'], 'boundary_penalty_warm_start')
                if accepted:
                    self.assertLess(np.max(abs(a@height-z)), Config().distance*.98)
                    self.assertLess(np.max(abs(a@height-target)), .002*.65)
                    self.assertEqual(report['linear_constraint_violation'], 0.)

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
