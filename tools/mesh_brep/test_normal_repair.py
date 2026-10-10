import tempfile
from pathlib import Path
import unittest
import numpy as np
from .mesh import write_stl
from .partition_fit import Config
from .skeleton_graph import reconstruct
from .normal_repair import optimize


class NormalRepairTests(unittest.TestCase):
    def test_small_interior_bump_is_repaired_without_changing_shared_curves(self):
        v=np.array([[0.,0,0],[1.,0,0],[0.,1,0],[0.,0,1]])
        f=np.array([[0,2,1],[0,1,3],[1,2,3],[2,0,3]])
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);write_stl(root/'source.stl',v,f)
            np.savez_compressed(root/'source.npz',mesh_vertex_original=v,mesh_triangle=f,face_instance_id=np.arange(4))
            m=reconstruct(root/'source.npz',root/'source.stl',root/'fit',smoothing=.01,workers=1,
                config=Config(distance=.03,boundary=.001,controls=(15,),normal_weight=.01,fairness=1e-6),
                normal_field='crease-fans',support_solver='bounded')
            fit=Path(m['source_fit']);path=fit/'charts/00000.npz';c=dict(np.load(path))
            n=len(c['knots'])-4;c['height'].reshape(n,n)[n//2,n//2]+=.02
            np.savez_compressed(path,**c)
            result=optimize(fit,root/'repaired',regions=[0],angle_limit=1.,workers=1)
            self.assertEqual(result['accepted_charts'],[0])
            self.assertEqual(result['rejected_charts'],[])
            with np.load(root/'repaired/charts/00000.npz') as actual:
                self.assertLessEqual(float(actual['interior_normal_max_degrees']),1.)
                np.testing.assert_array_equal(actual['triangle_ids'],c['triangle_ids'])
            for p in (fit/'curves').glob('*.npz'):
                with np.load(p) as old,np.load(root/'repaired/curves'/p.name) as new:
                    np.testing.assert_array_equal(old['poles'],new['poles'])
                    np.testing.assert_array_equal(old['knots'],new['knots'])


if __name__=='__main__':unittest.main()
