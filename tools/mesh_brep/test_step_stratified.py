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
            self.assertLess(r['source_distance_mm']['max'],1e-8)
            self.assertLess(r['source_flat_normal_degrees']['max'],1e-4)
            self.assertEqual(r['over_90'],0)


if __name__=='__main__':unittest.main()
