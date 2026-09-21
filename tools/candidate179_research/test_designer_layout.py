"""Exercise exact seam elimination against OCCT on synthetic planar geometry."""
import unittest
import numpy as np
from geometry import evaluate, xyz
from designer_layout import build_layout, variable_maps, uv_on_side
from spline import from_occt, to_occt
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace
from OCP.BRep import BRep_Builder
from OCP.TopoDS import TopoDS_Shell
from OCP.gp import gp_Pln, gp_Pnt, gp_Dir


class SharedSeamTest(unittest.TestCase):
    def test_random_interior_changes_preserve_outer_curves_and_shared_seams(self):
        face=BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0,0,0),gp_Dir(0,0,1)),-2.,2.,-1.,1.).Face()
        region=TopoDS_Shell();builder=BRep_Builder();builder.MakeShell(region);builder.Add(region,face)
        x,y=np.meshgrid(np.linspace(-2,2,11),np.linspace(-1,1,9))
        data=dict(center=np.zeros(3),axes=np.eye(3),train_points=np.c_[x.ravel(),y.ravel(),x.ravel()*0])
        protocol=dict(corner_edge_indices=[0,1,2,3],inner_scale=.45,seam_control_points=8,
                      radial_refinement=[.02,.06],source_curve_join_tolerance_mm=1e-10)
        layout=build_layout(data,region,protocol,[.02,.06]);maps,_,size=variable_maps(layout)
        delta=np.random.default_rng(179).normal(0,.01,size);changed=[]
        for surface,mapping in zip(layout['surfaces'],maps):
            p,tu,tv,d=from_occt(surface);p[:,:,2]+=(mapping@delta).reshape(p.shape[:2]);changed.append(to_occt(p,tu,tv,d))
        traces={};t=np.linspace(0,1,1001)
        for fi,sides in enumerate(layout['sides']):
            for side in sides:
                uv=uv_on_side(side['mapping'],t);p,_=evaluate(changed[fi],uv)
                if side['outer']:
                    old,_=evaluate(layout['surfaces'][fi],uv);np.testing.assert_allclose(p,old,atol=1e-11)
                else:traces.setdefault(side['seam'],[]).append(p)
        self.assertEqual(len(traces),8)
        for pairs in traces.values():
            self.assertEqual(len(pairs),2);np.testing.assert_allclose(pairs[0],pairs[1],atol=1e-11)
        # Refinement imported through a seam must not be inserted twice in a patch.
        for surface,sides in zip(layout['surfaces'],layout['sides']):
            for side in sides:
                if not side['outer']:continue
                mapping=side['mapping'];axis=mapping['axis'];knots=surface.UKnot if axis==0 else surface.VKnot
                mult=surface.UMultiplicity if axis==0 else surface.VMultiplicity
                count=surface.NbUKnots() if axis==0 else surface.NbVKnots()
                for fraction in [.02,.06]:
                    value=fraction if mapping['fixed']==0 else 1-fraction
                    found=[i for i in range(1,count+1) if abs(knots(i)-value)<1e-10]
                    self.assertEqual(len(found),1);self.assertEqual(mult(found[0]),1)


if __name__=='__main__':unittest.main()
