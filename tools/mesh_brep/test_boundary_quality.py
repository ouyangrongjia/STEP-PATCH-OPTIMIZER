import unittest
import numpy as np
from scipy.interpolate import make_interp_spline
from .boundary_quality import measures


class BoundaryQualityTests(unittest.TestCase):
    def test_turning_distinguishes_small_waves_from_a_straight_edge(self):
        t=np.linspace(0,1,129)
        wave=make_interp_spline(t,np.c_[t,.002*np.sin(20*np.pi*t),np.zeros(len(t))])
        line=make_interp_spline(t,np.c_[t,np.zeros((len(t),2))])
        a=measures(dict(knots=wave.t,poles=wave.c));b=measures(dict(knots=line.t,poles=line.c))
        self.assertGreater(a['total_tangent_turn_degrees'],200.)
        self.assertGreater(a['curvature_squared_integral_per_mm'],1.)
        self.assertLess(b['total_tangent_turn_degrees'],1e-3)
        self.assertAlmostEqual(b['length_mm'],1.,places=10)


if __name__=='__main__':unittest.main()
