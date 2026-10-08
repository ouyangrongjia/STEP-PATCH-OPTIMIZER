"""Independent CAD fixtures for exact common-boundary reconstruction."""
from pathlib import Path
import tempfile
import unittest
import numpy as np
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeFace, BRepBuilderAPI_Sewing
from OCP.BRepTools import BRepTools
from OCP.gp import gp_Pnt
from .regional_step import surface
from .repair_spline_seams import reconcile


class CommonBoundaryTests(unittest.TestCase):
    def test_refuse_to_replace_a_triangle_trim_with_its_rectangle(self):
        from .repair_spline_seams import split_rectangle
        cp=np.zeros((4,4,3));cp[:,:,0]=np.linspace(0,1,4)[:,None];cp[:,:,1]=np.linspace(0,1,4)[None,:]
        triangle=split_rectangle(surface(cp),1e-7)[0]
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);BRepTools.Write_s(triangle,str(root/'triangle.brep'))
            with self.assertRaisesRegex(ValueError,'rectang'):
                reconcile(root/'triangle.brep',root/'rejected')

    def test_exact_concatenation_with_unequal_knots_and_degree(self):
        from .merge_spline_faces import concatenate
        u=np.linspace(0,1,4)[:,None];v=np.linspace(0,1,4)[None,:]
        a=np.empty((4,4,3));b=a.copy()
        a[:,:,0]=u;a[:,:,1]=v;a[:,:,2]=.2*u*v
        b[:,:,0]=1+u;b[:,:,1]=v;b[:,:,2]=.2*(1+u)*v+.1*u*u
        sa,sb=surface(a),surface(b)
        sa.InsertVKnot(.37,1,1e-12,False);sb.InsertVKnot(.63,1,1e-12,False)
        sb.IncreaseDegree(4,3)
        first=BRepBuilderAPI_MakeFace(sa,1e-7).Face();second=BRepBuilderAPI_MakeFace(sb,1e-7).Face()
        sew=BRepBuilderAPI_Sewing(1e-7);sew.Add(first);sew.Add(second);sew.Perform()
        from . import cad
        from OCP.TopAbs import TopAbs_FACE
        first,second=cad.shapes(sew.SewedShape(),TopAbs_FACE)
        face,report=concatenate(first,second,(1,1),(1,0))
        self.assertLess(report['maximum_sampled_geometry_change_mm'],1e-12)
        from OCP.BRepCheck import BRepCheck_Analyzer
        self.assertTrue(BRepCheck_Analyzer(face,True).IsValid())

    def test_mismatched_cube_edge_with_different_knot_spaces(self):
        vertices = np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0],
                             [0,0,1],[1,0,1],[1,1,1],[0,1,1]], dtype=float)
        loops = [[0,3,2,1],[4,5,6,7],[0,1,5,4],[1,2,6,5],[2,3,7,6],[3,0,4,7]]
        sew = BRepBuilderAPI_Sewing(1e-6, True, True, True, False)
        for fi, loop in enumerate(loops):
            p = vertices[loop]; u = np.linspace(0,1,4)[:,None,None]; v = np.linspace(0,1,4)[None,:,None]
            cp = (1-u)*(1-v)*p[0]+u*(1-v)*p[1]+u*v*p[2]+(1-u)*v*p[3]
            if fi == 0: cp[1:3,0,2] += .02
            s = surface(cp)
            if fi == 0: s.InsertUKnot(.37, 1, 1e-12, False)
            sew.Add(BRepBuilderAPI_MakeFace(s,1e-7).Face())
        sew.Perform()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root/'source.brep'
            BRepTools.Write_s(sew.SewedShape(),str(source))
            report = reconcile(source,root/'repaired',max_gap=.05)
            self.assertEqual(report['before']['free_edges'],2)
            self.assertEqual(report['reread']['faces'],6)
            self.assertEqual(report['reread']['solids'],1)
            self.assertEqual(report['reread']['free_edges'],0)
            self.assertTrue(report['reread']['brep_valid'])
            self.assertLessEqual(report['reread']['maximum_edge_tolerance_mm'],1.01e-7)
            self.assertGreater(report['repairs'][0]['maximum_control_displacement_mm'],.001)


if __name__ == '__main__': unittest.main()
