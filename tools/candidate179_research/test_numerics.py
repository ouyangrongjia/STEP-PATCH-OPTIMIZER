"""Independent analytic checks for the research evaluator."""
import unittest
import numpy as np
from geometry import evaluate as kernel_evaluate
from spline import knots,to_occt,evaluate
from evaluate import closest


class NumericalGeometryTests(unittest.TestCase):
    def test_known_normal_offset_of_paraboloid(self):
        # Cubic Bezier representation of (u, v, u^2 + v^2).
        quadratic=[0.,0.,1/3,1.]
        poles=np.array([[[i/3,j/3,quadratic[i]+quadratic[j]] for j in range(4)] for i in range(4)])
        tu=tv=knots(4)
        uv=np.array([[.3,.4],[.6,.3],[.2,.65]])
        on_surface=np.c_[uv,np.sum(uv*uv,axis=1)]
        normal=np.c_[-2*uv,np.ones(len(uv))];normal/=np.linalg.norm(normal,axis=1)[:,None]
        points=on_surface+normal*.03
        distance,_,found,projection=closest(points,poles,tu,tv,(3,3),uv+.17,iterations=35)
        np.testing.assert_allclose(distance,.03,atol=1e-10)
        np.testing.assert_allclose(found,uv,atol=2e-8)
        np.testing.assert_allclose(projection,on_surface,atol=3e-8)

    def test_basis_matches_occt_values_and_normals(self):
        rng=np.random.default_rng(3);tu=knots(9);tv=knots(7)
        poles=rng.normal(size=(9,7,3));uv=rng.uniform(.01,.99,(80,2))
        actual,normals=kernel_evaluate(to_occt(poles,tu,tv),uv)
        np.testing.assert_allclose(evaluate(poles,tu,tv,uv),actual,atol=1e-12)
        n=np.cross(evaluate(poles,tu,tv,uv,1,0),evaluate(poles,tu,tv,uv,0,1));n/=np.linalg.norm(n,axis=1)[:,None]
        np.testing.assert_allclose(n,normals,atol=1e-11)


if __name__=='__main__':unittest.main()
