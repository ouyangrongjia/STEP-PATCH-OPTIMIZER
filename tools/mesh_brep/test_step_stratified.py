import tempfile
from pathlib import Path
import unittest
import numpy as np
from OCP.BRep import BRep_Builder
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakePolygon, BRepBuilderAPI_MakeFace
from OCP.TopoDS import TopoDS_Compound
from OCP.gp import gp_Pnt
from . import cad
from .step_stratified import audit


class StratifiedTests(unittest.TestCase):
    def test_bounding_boxes_preserve_holes_and_near_boundary_classification(self):
        from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire
        from OCP.BRepClass import BRepClass_FaceClassifier
        from OCP.Geom import Geom_Circle
        from OCP.GeomConvert import GeomConvert
        from OCP.gp import gp_Ax2, gp_Dir, gp_Pln, gp_Pnt2d
        from OCP.TopAbs import TopAbs_IN, TopAbs_ON
        from OCP.TopoDS import TopoDS
        from .step_stratified import inside_trim
        polygon = BRepBuilderAPI_MakePolygon()
        for a, b in [((-1.,-1.),(1.,-1.)),((1.,-1.),(1.,1.)),
                     ((1.,1.),(-1.,1.)),((-1.,1.),(-1.,-1.))]:
            for xy in np.linspace(a, b, 17, endpoint=False):
                polygon.Add(gp_Pnt(*xy, 0.))
        polygon.Close()
        circle = Geom_Circle(gp_Ax2(gp_Pnt(0.,0.,0.), gp_Dir(0.,0.,1.)), .35)
        edge = BRepBuilderAPI_MakeEdge(GeomConvert.CurveToBSplineCurve_s(circle)).Edge()
        hole = BRepBuilderAPI_MakeWire(edge).Wire()
        maker = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0.,0.,0.), gp_Dir(0.,0.,1.)), polygon.Wire(), True)
        maker.Add(TopoDS.Wire_s(hole.Reversed())); face = maker.Face()
        q = np.random.default_rng(19).uniform(-1.2, 1.2, (500, 2))
        q = np.r_[q, [[0.,0.],[1.,0.],[.35,0.],[.35+1e-7,0.],[.35-1e-7,0.],
                      [1.-1e-7,0.],[1.+1e-7,0.],[1.+1e-10,0.]]]
        expected = [BRepClass_FaceClassifier(face, gp_Pnt2d(*x), 1e-9, False).State()
                    in (TopAbs_IN, TopAbs_ON) for x in q]
        np.testing.assert_array_equal(inside_trim(face, q), expected)
        self.assertFalse(inside_trim(face, np.array([[0.,0.]]))[0])
        self.assertTrue(inside_trim(face, np.array([[.7,0.]]))[0])

    def test_small_faces_receive_samples_and_sharp_normals_are_not_averaged(self):
        v=np.array([[0.,0,0],[1.,0,0],[0.,1,0],[0.,0,.01]])
        f=np.array([[0,2,1],[0,1,3],[1,2,3],[2,0,3]])
        builder=BRep_Builder();shape=TopoDS_Compound();builder.MakeCompound(shape)
        for tri in f:
            polygon=BRepBuilderAPI_MakePolygon()
            for point in v[tri]:polygon.Add(gp_Pnt(*point))
            polygon.Close();builder.Add(shape,BRepBuilderAPI_MakeFace(polygon.Wire()).Face())
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);cad.write_step(root/'model.step',shape)
            np.savez_compressed(root/'source.npz',mesh_vertex_original=v,mesh_triangle=f)
            r=audit(root/'model.step',root/'source.npz',root/'quality.json',samples_per_face=64)
            self.assertEqual(r['faces'],4)
            self.assertEqual(r['minimum_verified_samples_per_face'],64)
            self.assertEqual([row['worst_distance']['face'] for row in r['per_face']], list(range(4)))
            for row in r['per_face']:
                self.assertEqual(row['worst_distance']['distance_mm'], row['source_distance_mm']['max'])
            self.assertLess(r['source_distance_mm']['max'],1e-8)
            self.assertLess(r['source_flat_normal_degrees']['max'],1e-4)
            self.assertEqual(r['over_90'],0)


if __name__=='__main__':unittest.main()
