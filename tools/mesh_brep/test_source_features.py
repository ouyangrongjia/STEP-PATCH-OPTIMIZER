import json
import tempfile
import unittest
from pathlib import Path
import numpy as np
from .source_features import split


class SourceFeatureLayoutTests(unittest.TestCase):
    def test_internal_fold_is_split_without_changing_triangle_or_region_ownership(self):
        v=np.array([[0.,0,0],[1,0,0],[1,1,0],[0,1,0],[0,0,1],[1,0,1],[1,1,1],[0,1,1]])
        f=np.array([[0,2,1],[0,3,2],[4,5,6],[4,6,7],[0,1,5],[0,5,4],
                    [1,2,6],[1,6,5],[2,3,7],[2,7,6],[3,0,4],[3,4,7]])
        labels=np.zeros(12,dtype=int); assignment=np.repeat([0,1,2,3,4,0],2)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); source=root/'fit'; source.mkdir()
            for name in ('charts','curves'): (source/name).mkdir()
            np.savez_compressed(root/'partition.npz',mesh_vertex_original=v,mesh_triangle=f,face_instance_id=labels)
            np.savez_compressed(source/'chart_index.npz',face_chart=assignment,source_labels=labels)
            (source/'fitting.json').write_text('[]',encoding='utf-8')
            (source/'run.json').write_text(json.dumps(dict(partition=str(root/'partition.npz'),config=dict(feature_angle=30))),encoding='utf-8')
            result=split(source,root/'split')
            self.assertEqual(result['supports_before'],5)
            self.assertEqual(result['supports_after'],6)
            self.assertEqual(result['source_crease_edges'],1)
            with np.load(root/'split/chart_index.npz') as archive:
                actual=dict(archive)
            np.testing.assert_array_equal(actual['source_labels'],labels)
            self.assertEqual(len(actual['face_chart']),len(f))
            self.assertNotEqual(actual['face_chart'][0],actual['face_chart'][-1])
            np.testing.assert_array_equal(np.load(source/'chart_index.npz')['face_chart'],assignment)
            meta=json.loads((root/'split/run.json').read_text(encoding='utf-8'))
            self.assertTrue(meta['requires_support_refit'])
            self.assertEqual(meta['required_refit_regions'],[0])


if __name__=='__main__': unittest.main()
