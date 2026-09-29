"""Regression fixtures independent of the retired adaptive layout generator."""
import tempfile
import unittest
from pathlib import Path
import numpy as np
from geometry import *
from spline import knots,to_occt,from_occt,evaluate as spline_value
from evaluate import closest
from integrate import edge_checks
from diagnose import seam_quality
from validation import distances,representation,continuity
from support_merge import compact
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace,BRepBuilderAPI_Sewing


def closed_strip():
    k=.5522847498307936
    xy=np.array([[1,0],[1,k],[k,1],[0,1],[-k,1],[-1,k],[-1,0],[-1,-k],[-k,-1],[0,-1],[k,-1],[1,-k],[1,0]])
    poles=np.empty((13,4,3));poles[:,:,:2]=xy[:,None,:];poles[:,:,2]=np.linspace(0,1,4)[None,:]
    tu=np.r_[np.zeros(4),np.repeat([.25,.5,.75],3),np.ones(4)]
    surface=to_occt(poles,tu,knots(4))
    return BRepBuilderAPI_MakeFace(surface,1e-7).Face()


def planar_grid():
    sew=BRepBuilderAPI_Sewing(1e-7)
    for i in range(3):
        for j in range(3):
            p=np.array([[[i+u,j+v,0.] for v in np.linspace(0,1,4)] for u in np.linspace(0,1,4)])
            sew.Add(BRepBuilderAPI_MakeFace(to_occt(p,knots(4),knots(4)),1e-7).Face())
    sew.Perform();return sew.SewedShape()


class RetainedGeometryTests(unittest.TestCase):
    def test_continuity_uses_native_parameter_ranges(self):
        p,tu,tv,d=from_occt(BRep_Tool.Surface_s(closed_strip()))
        face=BRepBuilderAPI_MakeFace(to_occt(p,2+2*tu,3+4*tv,d),1e-7).Face()
        result=continuity(face)
        self.assertTrue(result['knot_records'])
        self.assertTrue(np.isfinite(result['maximum_sampled_normal_jump_degrees']))
        self.assertLess(result['maximum_sampled_normal_jump_degrees'],1e-3)

    def test_closed_support_step_counts_and_two_pcurves(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'periodic.step';write_step(path,closed_strip());shape=read_step(path)
            t=topology(shape);self.assertTrue(t['brep_valid']);self.assertEqual(t['free_edges'],2)
            self.assertEqual(t['self_seam_edges'],1);self.assertEqual(t['multiple_edges'],0)
            audit=edge_checks(shape,73);self.assertEqual(audit['checks'],4)
            self.assertTrue(audit['closed_self_seams'][0]['pcurves_distinct'])
            self.assertLess(audit['max_curve_on_surface_mm'],1e-9)
            seams=seam_quality(shape);self.assertEqual(len(seams),1);self.assertTrue(seams[0]['self_seam'])
            self.assertLess(seams[0]['separation_mm'],1e-9)
            surface=BRep_Tool.Surface_s(shapes(shape,TopAbs_FACE)[0])
            self.assertTrue(surface.IsUPeriodic())
            native=(surface.NbUPoles(),surface.NbVPoles());p,tu,tv,d=from_occt(surface)
            self.assertEqual(native,(surface.NbUPoles(),surface.NbVPoles()))
            self.assertEqual(representation(shape)['actual_support_control_points'],native[0]*native[1])
            q=np.random.default_rng(17).random((71,2))
            np.testing.assert_allclose(evaluate(surface,q)[0],spline_value(p,tu,tv,q,degrees=d),atol=1e-10,rtol=0)
            truth=np.array([[.995,.3],[.005,.7]]);start=np.array([[.005,.3],[.995,.7]])
            points=evaluate(surface,truth)[0]
            found,_,_,_=closest(points,p,tu,tv,d,start,iterations=40)
            self.assertLess(found.max(),1e-8)
            report,_=distances(points,shape)
            self.assertTrue(report['kernel_agreement_pass']);self.assertLess(report['distance_mm']['max'],1e-8)
            self.assertEqual(len(shapes(shape,TopAbs_FACE)),1)

    def test_exact_merge_reduces_actual_support_and_preserves_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            original=planar_grid();result,report=compact(original,Path(directory)/'merge',128,30.)
            self.assertEqual(report['faces'],1);self.assertLess(report['step_bytes'],report['input_step_bytes'])
            self.assertTrue(topology(result)['brep_valid'])
            p=from_occt(BRep_Tool.Surface_s(shapes(result,TopAbs_FACE)[0]))[0]
            self.assertEqual(p.shape[:2],(10,10));self.assertLess(abs(p[:,:,2]).max(),1e-12)
            self.assertLess(edge_checks(result,37)['max_curve_on_surface_mm'],1e-9)

    def test_protected_edges_block_merging(self):
        with tempfile.TemporaryDirectory() as directory:
            original=planar_grid()
            protected=[i for i,(_,uses) in enumerate(edge_uses(shapes(original,TopAbs_FACE))) if len(uses)==2]
            result,report=compact(original,Path(directory)/'merge',128,30.,protected)
            self.assertEqual(report['faces'],9);self.assertEqual(report['attempted_joins'],0)
            self.assertEqual(len(shapes(result,TopAbs_EDGE)),len(shapes(original,TopAbs_EDGE)))


if __name__=='__main__':unittest.main()
