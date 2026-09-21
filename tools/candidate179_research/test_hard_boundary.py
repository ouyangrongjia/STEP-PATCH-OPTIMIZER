"""Independent analytic check of derivatives under a curved parameter map."""
import unittest
import numpy as np
from hard_boundary import operators
from spline import basis


class PhysicalDerivativeTest(unittest.TestCase):
    def test_quadratic_height_in_warped_coordinates(self):
        knots=np.r_[np.zeros(5),np.ones(5)]
        t=np.linspace(0,1,5);u,v=np.meshgrid(t,t,indexing='ij')
        x=u+.2*v*v;y=2*v;z=x*x+y*y
        b=basis(knots,4,t)
        poles=np.stack([np.linalg.solve(b,np.linalg.solve(b,a).T).T for a in (x,y,z)],axis=-1)
        uv=np.array([[.17,.23],[.39,.82],[.85,.71]])
        dx,dy,_=operators(poles,knots,knots,(4,4),uv)
        xx,xy,yy,_=operators(poles,knots,knots,(4,4),uv,True)
        height=poles[:,:,2].ravel()
        targets=(2*(uv[:,0]+.2*uv[:,1]**2),4*uv[:,1],np.full(3,2.),np.zeros(3),np.full(3,2.))
        for operation,expected in zip((dx,dy,xx,xy,yy),targets):
            np.testing.assert_allclose(operation@height,expected,atol=1e-11)


if __name__=='__main__':unittest.main()
