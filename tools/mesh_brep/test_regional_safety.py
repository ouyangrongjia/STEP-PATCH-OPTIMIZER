"""Geometric regression cases independent of the production model."""
import unittest
import numpy as np

from .intersections import confirm_pairs, self_intersections
from .subdivision_fit import connectivity, limit_stencil
from .regional_reconstruct import triangular_net, evaluate
from .regional_step import affine_triangle
from .surface_safeguard import blend


class GeometricSafetyTests(unittest.TestCase):
    def test_local_blend_is_exact_and_preserves_shared_edge(self):
        rng=np.random.default_rng(715);base=np.array([[[0,0,0],[0,1,0]],[[1,0,0],[1,1,0]]],dtype=float)
        cp=rng.normal(size=(4,4,3));weights=np.array([[.2,.3],[.8,1.]])
        mixed=blend(cp,base,weights);uv=rng.uniform(0,1,(30,2))
        p=evaluate(cp,uv)[0];b=evaluate(base,uv)[0];u,v=uv.T
        w=(1-u)*(1-v)*weights[0,0]+u*(1-v)*weights[1,0]+(1-u)*v*weights[0,1]+u*v*weights[1,1]
        np.testing.assert_allclose(evaluate(mixed,uv)[0],b+w[:,None]*(p-b),atol=1e-12)
        other=cp.copy();other[:,1:]+=rng.normal(size=(4,3,3));other_weights=weights.copy();other_weights[:,1]=[0.,.4]
        np.testing.assert_allclose(blend(other,base,other_weights)[:,0],mixed[:,0],atol=1e-12)

    def pair(self,a,b):
        v=np.array(a+b,dtype=float);f=np.arange(6,dtype=np.int64).reshape(2,3)
        return bool(confirm_pairs(v,f,np.array([[0,1]],dtype=np.int64),1e-9)[0])

    def test_intersections_distinguish_touch_overlap_crossing_and_separation(self):
        tri=[[0,0,0],[2,0,0],[0,2,0]]
        self.assertFalse(self.pair(tri,[[2,0,0],[2,2,0],[0,2,0]]))
        self.assertTrue(self.pair(tri,[[.2,.2,0],[1,.2,0],[.2,1,0]]))
        self.assertTrue(self.pair(tri,[[.5,.5,-1],[.5,.5,1],[1,.5,0]]))
        self.assertFalse(self.pair(tri,[[.2,.2,.001],[1,.2,.001],[.2,1,.001]]))
        self.assertFalse(self.pair(tri,[[0,0,0],[2,0,0],[0,0,2]]))

    def test_creased_linear_refinement_does_not_create_false_collisions(self):
        v=np.array([[0,0,0],[1,0,0],[0,1,0],[0,0,1]],dtype=float)+[123.4,-31.7,18.9]
        f=np.array([[0,2,1],[0,1,3],[1,2,3],[2,0,3]])
        e,_,_=connectivity(f)
        stencil,faces,_,_=limit_stencil(v,f,set(map(tuple,e)),2)
        self.assertEqual(len(self_intersections(stencil@v,faces)),0)

    def test_triangular_trim_conversion_preserves_curved_geometry(self):
        corners=np.array([[0,0,0],[1,0,0],[.2,1,.1]],dtype=float)
        curves=[]
        for a,b in zip(corners,np.roll(corners,-1,axis=0)):
            c=a+np.linspace(0,1,4)[:,None]*(b-a);c[1:3,2]+=.07;curves.append(c)
        cp,_=triangular_net(corners,curves,5);affine=affine_triangle(cp)
        q=np.random.default_rng(621).uniform(.01,.99,(100,2))
        xyz,_,_=evaluate(cp,q);mapped=np.c_[q[:,0]*(1-q[:,1]),q[:,1]]
        other,_,_=evaluate(affine,mapped)
        np.testing.assert_allclose(xyz,other,atol=1e-11)


if __name__=='__main__':unittest.main()
