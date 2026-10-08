"""Input identity gates for explicit fitting reuse."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from .legacy_wrap_reconstruct import reuse_fit
from .mesh import dump


class ReuseTests(unittest.TestCase):
    def fixture(self, root):
        dirs=[root/'old',root/'new']
        for out in dirs:
            out.mkdir()
            (out/'input.stl').write_bytes(b'identity gate fixture')
            np.savez(out/'labels.npz',mesh_vertex_original=np.eye(3),mesh_triangle=[[0,1,2]],face_instance_id=[7])
            dump(out/'input.json',dict(units='mm',distance_mm=.03,num_patches=0,geometry='Organic',
                                      sharp_boundaries=False,partition=str(out/'labels.npz')))
            dump(out/'constraints.json',dict(edges=[[0,0,0,1,0,0]],junctions=[]))
        dump(dirs[0]/'run.json',dict(stage='complete'))
        (dirs[0]/'surfaces.igs').write_text('fixture-only',encoding='ascii')
        return dirs

    def test_reuse_identical_inputs_and_preserve_origin(self):
        with tempfile.TemporaryDirectory() as folder:
            old,new=self.fixture(Path(folder));report=reuse_fit(old,new)
            self.assertEqual(report['reused_from'],str(old.resolve()))
            self.assertEqual((new/'surfaces.igs').read_text(),'fixture-only')

    def test_reject_same_geometry_with_different_partition(self):
        with tempfile.TemporaryDirectory() as folder:
            old,new=self.fixture(Path(folder))
            np.savez(new/'labels.npz',mesh_vertex_original=np.eye(3),mesh_triangle=[[0,1,2]],face_instance_id=[8])
            with self.assertRaisesRegex(ValueError,'face_instance_id'):reuse_fit(old,new)
            self.assertFalse((new/'surfaces.igs').exists())

    def test_reject_different_engine_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            old,new=self.fixture(Path(folder))
            config=json.loads((old/'input.json').read_text());config['engine_merge']=True;dump(old/'input.json',config)
            with self.assertRaisesRegex(ValueError,'engine merging'):reuse_fit(old,new)

    def test_reject_changed_stl_snapshot(self):
        with tempfile.TemporaryDirectory() as folder:
            old,new=self.fixture(Path(folder));(new/'input.stl').write_bytes(b'changed input')
            with self.assertRaisesRegex(ValueError,'STL snapshot'):reuse_fit(old,new)


if __name__=='__main__':unittest.main()
