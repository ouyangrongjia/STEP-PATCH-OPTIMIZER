import unittest
import numpy as np
from .support_probe import normal_targets, probes
from .splines import knots, tensor


class SupportProbeTests(unittest.TestCase):
    def test_normal_interpolation_keeps_both_sides_of_a_crease(self):
        v=np.array([[0.,0,0],[1.,0,0],[1.,1,0],[0.,1,0]])
        f=np.array([[0,1,2],[0,2,3]])
        n=np.array([[[0.,0,1]]*3,[[.6,0,.8]]*3])
        rec=dict(origin=np.zeros(3),axes=np.eye(3),lo=np.zeros(2),size=np.ones(2))
        result=normal_targets(rec,v,f,n,np.array([[.7,.2],[.2,.7]]))
        np.testing.assert_allclose(result,[[0,0,1],[.6,0,.8]])

    def test_position_probes_also_expose_a_small_but_steep_height_wave(self):
        v=np.array([[0.,0,0],[1.,0,0],[1.,1,0],[0.,1,0]])
        f=np.array([[0,1,2],[0,2,3]])
        t=np.sort(np.r_[knots(4),.49,.495,.5,.505,.51])
        c=np.zeros((len(t)-4,len(t)-4));c[4,:]=.01
        rec=dict(origin=np.zeros(3),axes=np.eye(3),lo=np.zeros(2),size=np.ones(2),knots=t)
        q,z=probes(rec,v,f)
        error=abs(tensor(q,t,t)@c.ravel()-z)
        slope=tensor(q,t,t,1,0)@c.ravel()
        self.assertLess(error.max(),.03)
        self.assertGreater(np.degrees(np.arctan(abs(slope))).max(),20.)


if __name__=='__main__':unittest.main()
