"""Native budget, label isolation and geometric diagnostic regressions."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from .mesh import write_stl,read_stl
from .regional_reconstruct import quad_layout
from .whole_reconstruct import reconstruct
from .intersections import self_intersections
from .safe_regional import sampled_intersections


class NativeBudgetTests(unittest.TestCase):
    def test_sparse_tensor_matches_dense_derivatives_at_repeated_knots(self):
        from .splines import tensor,basis
        tu=np.r_[np.zeros(4),.2,.5,.5,.5,.8,np.ones(4)]
        tv=np.r_[np.zeros(4),.1,.4,.4,.7,np.ones(4)]
        uv=np.r_[np.random.default_rng(914).uniform(0,1,(70,2)),[[0,0],[1,1],[.5,.4]]]
        for du,dv in [(0,0),(1,0),(0,1),(2,0),(1,1),(0,2)]:
            expected=np.einsum('ni,nj->nij',basis(tu,uv[:,0],du),basis(tv,uv[:,1],dv)).reshape(len(uv),-1)
            actual=tensor(uv,tu,tv,du,dv)
            np.testing.assert_allclose(actual.toarray(),expected,atol=1e-10,rtol=1e-12)
            self.assertLessEqual(actual.nnz,16*len(uv))

    def tetrahedron(self):
        v=np.array([[0.,0,0],[1,0,0],[0,1,0],[0,0,1]])
        f=np.array([[0,2,1],[0,1,3],[1,2,3],[2,0,3]])
        return v,f

    def test_no_pairing_across_supplied_regions(self):
        v,f=self.tetrahedron();r=np.arange(4)
        layout=quad_layout(v,f,r,matching='maximum')
        self.assertEqual(len(layout),4)
        self.assertEqual(sorted(p['region'] for p in layout),list(r))
        self.assertEqual(sorted(i for p in layout for i in p['source']),list(range(4)))

    def test_impossible_reference_budget_keeps_failure_and_emits_no_step(self):
        v,f=self.tetrahedron()
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);stl=root/'input.stl';write_stl(stl,v,f)
            sv,sf=read_stl(stl);partition=root/'partition.npz'
            np.savez(partition,mesh_vertex_original=sv,mesh_triangle=sf,face_instance_id=np.arange(4))
            with self.assertRaisesRegex(ValueError,'No native layout'):
                reconstruct(stl,partition,root/'result',reference_faces=4,layout_errors=[.01],render=False)
            report=json.loads((root/'result/run.json').read_text(encoding='utf-8'))
            self.assertEqual(report['status'],'failed')
            self.assertFalse(report['commercial_engine_used'])
            self.assertEqual(report['maximum_reconstructed_faces'],3)
            self.assertEqual(list((root/'result').rglob('*.step')),[])

    def test_graph_entry_exports_closed_step_and_enforces_reference_budget(self):
        from .graph_reconstruct import reconstruct as graph_reconstruct
        from .partition_fit import Config
        v, f = self.tetrahedron()
        with tempfile.TemporaryDirectory() as name:
            root = Path(name); stl = root/'input.stl'; write_stl(stl, v, f)
            sv, sf = read_stl(stl); partition = root/'partition.npz'
            np.savez(partition, mesh_vertex_original=sv, mesh_triangle=sf, face_instance_id=np.arange(4))
            with self.assertRaisesRegex(ValueError, 'strict reference face budget'):
                graph_reconstruct(stl, partition, root/'rejected', reference_faces=4, render=False)
            self.assertEqual(json.loads((root/'rejected/run.json').read_text())['status'], 'failed')
            self.assertFalse(list((root/'rejected').rglob('*.step')))
            result = graph_reconstruct(stl, partition, root/'accepted', reference_faces=5,
                config=Config(distance=1e-5, boundary=1e-5, controls=(4,), normal_weight=0.),
                render=False, condition_normals=True)
            self.assertTrue(result['step']['watertight'])
            self.assertEqual(result['step']['topology']['faces'], 4)
            self.assertLess(result['quality']['reverse_native_surface_distance_mm']['max'], 1e-6)
            self.assertFalse(result['assembly']['step_roundtrip_performed'])
            self.assertFalse((root/'accepted/assembled/model.step').exists())
            self.assertTrue((root/'accepted/step/model.step').exists())
            self.assertFalse(result['can_apply'])
            self.assertNotIn('creo', result)
            # Reorder generated faces: source selection must follow the full
            # support geometry, not STEP numbering or neighboring trim votes.
            from . import cad
            from .graph_repair import failed_step_charts
            from OCP.TopAbs import TopAbs_FACE
            from OCP.BRep import BRep_Builder
            from OCP.TopoDS import TopoDS_Compound
            original = cad.shapes(cad.read_generated(root/'accepted/step/model.step'), TopAbs_FACE)
            before = {}
            expected = failed_step_charts(root/'accepted/step/model.step', [3], result['native_fit'],
                assembly=root/'accepted/assembled', details=before)
            self.assertEqual(len(expected), 1)
            self.assertTrue(before['all_supports_verified'])
            builder = BRep_Builder(); reordered = TopoDS_Compound(); builder.MakeCompound(reordered)
            for face in reversed(original):
                builder.Add(reordered, face)
            cad.write_step(root/'reordered.step', reordered)
            after = {}
            actual = failed_step_charts(root/'reordered.step', [0], result['native_fit'],
                assembly=root/'accepted/assembled', details=after)
            self.assertTrue(after['all_supports_verified'])
            self.assertEqual(actual, expected)
            from .step_outliers import track_source_points
            tracked = track_source_points(root/'reordered.step', root/'accepted/assembled',
                result['native_fit'], [dict(source_triangle=0, source_barycentric=[.2, .3, .5])],
                root/'tracked.json')['records'][0]
            self.assertTrue(tracked['support_verified'])
            self.assertTrue(tracked['inside_trim'])
            self.assertLess(tracked['correspondence_distance_mm'], 1e-10)
            self.assertLess(tracked['source_face_normal_angle_degrees'], 1e-5)
            from .export_regions import export
            regional = export(root/'accepted/assembled', root/'regions')
            self.assertEqual(regional['exported_regions'], 4)
            self.assertEqual(regional['exported_faces'], result['step']['topology']['faces'])
            self.assertEqual(sorted(r['region'] for r in regional['files']), list(range(4)))

            # Creo can leave a closed quilt with invalid faces and no free
            # edges. Exercise selection and refitting on actual STEP geometry;
            # only the external application's failure report is simulated.
            from unittest.mock import patch
            from .mesh import dump
            history = root/'curve_history'; history.mkdir()
            dump(history/'run.json', dict(stl=str(stl),partition=str(partition),attempts=[
                dict(source_fit=result['native_fit'],smoothing_disabled_charts=[0])]))
            closed_failure = dict(abnormal_edges=[], topology=dict(invalid_faces=[0]))
            with patch('tools.mesh_brep.creo_validation.creo_check', return_value={}), \
                 patch('tools.mesh_brep.creo_validation.native_body_check', side_effect=[
                     dict(native_closed_solid=False), dict(native_closed_solid=True)]), \
                 patch('tools.mesh_brep.creo_validation.export_native_part',
                       return_value=root/'accepted/step/model.step'), \
                 patch('tools.mesh_brep.creo_boundary_audit.audit', return_value=closed_failure):
                recovered = graph_reconstruct(stl, partition, root/'closed_invalid', reference_faces=5,
                    from_fit=result['native_fit'], render=False, creo_root=root, native_probe=root,
                    max_refinements=1,boundary_history=[history])
            self.assertTrue(recovered['native_Creo_solid_passed'])
            self.assertEqual(recovered['attempts'][0]['creo_invalid_faces'], [0])
            self.assertTrue(recovered['attempts'][0]['refined_source_charts'])
            self.assertEqual(recovered['attempts'][1]['smoothing_disabled_charts'],[0])
            self.assertEqual(recovered['step']['topology']['faces'], 4)
            self.assertFalse(recovered['can_apply'])
            with patch('tools.mesh_brep.creo_validation.creo_check', return_value={}), \
                 patch('tools.mesh_brep.creo_validation.native_body_check', side_effect=[
                     dict(native_closed_solid=False), dict(native_closed_solid=True)]), \
                 patch('tools.mesh_brep.creo_validation.export_native_part',
                       return_value=root/'accepted/step/model.step'), \
                 patch('tools.mesh_brep.creo_boundary_audit.audit', return_value=closed_failure):
                curves = graph_reconstruct(stl, partition, root/'curve_recovery', reference_faces=5,
                    from_fit=result['native_fit'], render=False, creo_root=root, native_probe=root,
                    max_refinements=1,boundary_smoothing=1e-7)
            self.assertTrue(curves['native_Creo_solid_passed'])
            self.assertEqual(curves['attempts'][0]['recovery'],'restore_source_shared_curves')
            self.assertNotIn('refined_source_charts',curves['attempts'][0])
            self.assertEqual(curves['step']['topology']['faces'],4)

    def test_affine_parent_filter_retains_real_crossing(self):
        a=np.array([[0.,0,0],[2,0,0],[0,2,0]])
        b=np.array([[.5,.5,-1],[.5,.5,1],[1,.5,0]])
        patches=[dict(vertices=list(range(3)),poles=np.array([[p[0],p[2]],[p[1],p[2]]])) for p in [a,b]]
        bad,_=sampled_intersections(patches,resolution=5)
        self.assertEqual(bad.tolist(),[0,1])
        v=np.r_[a,b];f=np.arange(6).reshape(2,3)
        self.assertEqual(self_intersections(v,f,all_candidates=True,return_pairs=True).tolist(),[[0,1]])


if __name__=='__main__':unittest.main()
