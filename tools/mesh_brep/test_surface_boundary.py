import unittest
import numpy as np
from .surface_boundary import certify_tube, fair_curve
from .splines import knots
from .mesh import Locator, chain_parameters


class SurfaceTubeTests(unittest.TestCase):
    def setUp(self):
        self.v=np.array([[-1.,-1,0],[2.,-1,0],[2.,1,0],[-1.,1,0]])
        self.f=np.array([[0,1,2],[0,2,3]])

    def test_certificate_catches_excursion_between_endpoints(self):
        cp=np.array([[0.,0,0],[1/3,0,.01],[2/3,0,.01],[1,0,0]])
        self.assertFalse(certify_tube(cp,knots(4),Locator(self.v,self.f),.003)['passed'])
        cp[:,2]*=.1
        result=certify_tube(cp,knots(4),Locator(self.v,self.f),.003)
        self.assertTrue(result['passed'])
        self.assertLessEqual(result['certified_max_mm'],.003)
        self.assertGreaterEqual(result['certified_max_mm'],.00075)

    def test_planar_staircase_fairs_without_leaving_source_mesh(self):
        from scipy.interpolate import make_interp_spline, BSpline
        p=np.c_[np.linspace(0,1,31),.01*np.sin(np.linspace(0,12*np.pi,31)),np.zeros(31)]
        t=chain_parameters(p,np.arange(len(p))); seed=make_interp_spline(t,p,k=3)
        cp,k,info=fair_curve(p,t,.05,self.v,self.f,.003,seed.c,seed.t)
        self.assertTrue(info['accepted'],info)
        xyz=BSpline(k,cp,3)(np.linspace(0,1,1001))
        self.assertLess(abs(xyz[:,1]).max(),.0001)
        self.assertLess(abs(xyz[:,2]).max(),1e-10)

    def test_curved_source_respects_shared_endpoint_tangent_planes(self):
        from scipy.interpolate import make_interp_spline, BSpline
        x=np.linspace(0,1,31)
        v=np.r_[np.c_[x,np.full(31,-.1),.1*x*x],np.c_[x,np.full(31,.1),.1*x*x]]
        f=np.array([[i,i+1,i+32] for i in range(30)]+[[i,i+32,i+31] for i in range(30)])
        p=np.c_[x,.01*np.sin(12*np.pi*x),.1*x*x]
        t=chain_parameters(p,np.arange(len(p)))
        noisy=p.copy(); noisy[:,2]+=.0005*np.sin(6*np.pi*x)
        seed=make_interp_spline(t,noisy,k=3)
        normal=np.array([[0.,0,1],[-.2,0,1]]); normal/=np.linalg.norm(normal,axis=1)[:,None]
        cp,k,info=fair_curve(p,t,.05,v,f,.003,seed.c,seed.t,endpoint_normals=normal)
        self.assertTrue(info['accepted'],info)
        tangent=BSpline(k,cp,3)([0.,1.],1)
        self.assertLess(abs(np.sum(tangent*normal,axis=1)).max(),1e-7)

    def test_separate_domain_tubes_do_not_accept_union_only_proximity(self):
        from scipy.interpolate import make_interp_spline
        v = np.r_[self.v, self.v + [0, 0, .02]]
        f = np.r_[self.f, self.f+4]
        p = np.c_[np.linspace(0,1,9), np.zeros(9), np.zeros(9)]
        t = chain_parameters(p,np.arange(len(p))); seed=make_interp_spline(t,p,k=3)
        _,_,union = fair_curve(p,t,.05,v,f,.003,seed.c,seed.t,control_counts=(6,),iterations=3)
        _,_,separate = fair_curve(p,t,.05,v,f,.003,seed.c,seed.t,control_counts=(6,),iterations=3,
                                 triangle_groups=[np.array([0,1]),np.array([2,3])])
        self.assertTrue(union['accepted'])
        self.assertFalse(separate['accepted'])


if __name__=='__main__': unittest.main()
