import unittest
import numpy as np
from scipy.interpolate import BSpline

from . import transition_fair as fair
from .splines import knots
from .surface_jets import jets


class TransitionFairTests(unittest.TestCase):
    def test_fraction_respects_movement_box(self):
        x = np.array([.1, -.1]); d = np.array([.4, -.2])
        alpha = fair.feasible_fraction(x, d, -.2, .2)
        self.assertAlmostEqual(alpha, .25)
        self.assertTrue(np.all(abs(x+alpha*d) <= .2+1e-14))

    def test_world_curvature_pullback(self):
        angle = .6
        axes = np.array([[np.cos(angle), 0, np.sin(angle)], [0, 1, 0], [-np.sin(angle), 0, np.cos(angle)]])
        record = dict(axes=axes)
        shape = axes.T@np.diag([2., 3., 0.])@axes
        h, good = fair.hessian_target(record, axes[2][None], shape[None])
        self.assertTrue(good[0]); np.testing.assert_allclose(h[0], np.diag([2., 3.]), atol=1e-12)

    def state(self, angle):
        knot = knots(9); u = np.linspace(0, 1, 9)
        basis = BSpline(knot, np.eye(9), 3)(u)
        coeff = np.linalg.solve(basis, .02*u*(u-1))
        v = np.array([[-1, 0, 0], [0, 0, 0], [0, 1, 0], [-1, 1, 0],
                      [0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=float)
        f = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
        records = {i: dict(origin=np.array([i-1., 0, 0]), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                           knots=knot, height=np.repeat(coeff, 9), triangle_ids=np.array([2*i, 2*i+1]),
                           vertex_ids=np.arange(4*i, 4*i+4)) for i in range(2)}
        curve = dict(knots=knots(4), poles=np.array([[0, y, 0] for y in np.linspace(0, 1, 4)]),
                     owners=np.array([0, 1]), parameters=np.array([0., 1.]), chain=np.array([1, 2]))
        return dict(config=fair.Config(normal_weight=0., variation_length=1e-6, movement=.02), records=records,
                    curves={0: curve}, vertices=v, triangles=f, incident={0: [0], 1: [0]},
                    angles={0: np.array([angle])}, normals=np.tile([0., 0., 1.], (4, 3, 1)), distance=.03)

    def test_smooth_seam_improves_without_changing_curve(self):
        state = self.state(0.); fair._initialize(state)
        curve = state['curves'][0]['poles'].copy(); xyz = np.array([[0., .5, 0.]])
        before = [jets(state['records'][i], xyz)[0] for i in (0, 1)]
        after = []
        for i in (0, 1):
            _, height, report = fair._worker(i)
            self.assertTrue(report['accepted']); self.assertLess(report['source_max_after_mm'], .03)
            after.append(jets(dict(state['records'][i], height=height), xyz)[0])
        self.assertLess(np.linalg.norm(after[0]-after[1]), np.linalg.norm(before[0]-before[1])*.5)
        np.testing.assert_array_equal(curve, state['curves'][0]['poles'])

    def test_source_crease_does_not_get_common_tangent(self):
        state = self.state(90.); fair._initialize(state)
        _, height, _ = fair._worker(0)
        np.testing.assert_allclose(height, state['records'][0]['height'], atol=1e-9)

    def test_thin_domain_receives_relative_displacement_limit(self):
        state = self.state(0.); state['widths'] = {0: .001, 1: 1.}
        fair._initialize(state)
        _, height, report = fair._worker(0)
        self.assertTrue(report['accepted'])
        self.assertLessEqual(float(abs(height-state['records'][0]['height']).max()), .00005*(1+1e-8))
        self.assertAlmostEqual(report['local_movement_budget_mm'], .00005)

    def test_source_width_tracks_a_narrow_strip_and_units(self):
        from .shared_support import domain_widths
        v=np.array([[0.,0.,0.],[4.,0.,0.],[4.,.02,0.],[0.,.02,0.]])
        f=np.array([[0,1,2],[0,2,3]])
        records={0:dict(triangle_ids=np.arange(2)),1:dict(triangle_ids=np.arange(2))}
        curves={0:dict(chain=np.array([0,1,2,3,0]),owners=np.array([0,1]))}
        a=domain_widths(records,curves,v,f)[0]
        self.assertGreater(a,.019);self.assertLess(a,.02)
        self.assertAlmostEqual(domain_widths(records,curves,v*10,f)[0],a*10)


if __name__ == '__main__':
    unittest.main()
