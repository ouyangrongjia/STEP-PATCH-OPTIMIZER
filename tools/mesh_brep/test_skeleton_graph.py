import tempfile
import json
from pathlib import Path
import unittest
import numpy as np
from scipy.interpolate import BSpline
from .mesh import chain_parameters, vertex_normals
from .partition_fit import Config, frame, incidence, loops, boundary_edges
from .partition_cad import assemble
from .skeleton_graph import fit_boundary, stable_frame, skeleton, fit_support, connected_ring


class SkeletonGraphTests(unittest.TestCase):
    def test_unsupported_fitting_options_are_not_silently_ignored(self):
        v=np.array([[0.,0,0],[1.,0,0],[0.,1,0]]); f=np.array([[0,1,2]])
        for config in (Config(area_weighted=True),Config(normal_angle_limit=10),Config(fairness_order=3)):
            with self.assertRaisesRegex(ValueError, 'require the bounded support solver'):
                fit_support((0,v,f,np.tile([0.,0,1],(3,1)),v,[],config,'least-squares'))

    def test_partial_region_export_is_rejected_before_creating_cad(self):
        from .skeleton_local import export
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fit = root/'fit'; (fit/'charts').mkdir(parents=True)
            np.savez_compressed(root/'partition.npz', mesh_vertex_original=np.zeros((4, 3)),
                mesh_triangle=np.array([[0, 1, 2], [0, 2, 3]]), face_instance_id=np.array([0, 0]))
            (fit/'run.json').write_text(json.dumps(dict(partition=str(root/'partition.npz'))), encoding='utf-8')
            np.savez_compressed(fit/'charts/00000.npz', triangle_ids=np.array([0]), region=0)
            with self.assertRaisesRegex(ValueError, 'missing or duplicated source coverage'):
                export(fit, root/'bad_export', [0])

    def test_resume_reuses_only_complete_unchanged_shared_supports(self):
        from .skeleton_graph import reconstruct
        from .mesh import write_stl
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); write_stl(root/'source.stl', v, f)
            np.savez_compressed(root/'partition.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=np.arange(4))
            cfg = Config(distance=.001, boundary=.001, controls=(15,), normal_weight=0., fairness=1e-8)
            first = reconstruct(root/'partition.npz', root/'source.stl', root/'first', smoothing=.05,
                                workers=1, rounds=2, face_limit=4, config=cfg)
            second = reconstruct(root/'partition.npz', root/'source.stl', root/'second', smoothing=.05,
                                 workers=1, rounds=2, face_limit=4, from_fit=first['source_fit'])
            self.assertTrue(second['complete_source_coverage'])
            self.assertTrue(second['partition_labels_unchanged'])
            self.assertEqual(second['reused_charts'], 4)
            self.assertEqual(second['refitted_charts'], 0)
            checkpoint=Path(second['source_fit'])/'run.json'
            state=json.loads(checkpoint.read_text(encoding='utf-8'))
            state.update(requires_support_refit=True,required_refit_regions=[1])
            checkpoint.write_text(json.dumps(state),encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'require explicit support refitting'):
                reconstruct(root/'partition.npz',root/'source.stl',root/'wrong_selective',smoothing=.05,
                            workers=1,from_fit=second['source_fit'],refit_regions=[2])
            third=reconstruct(root/'partition.npz',root/'source.stl',root/'selective',smoothing=.05,
                              workers=1,from_fit=second['source_fit'],refit_regions=[1])
            self.assertTrue(third['complete_source_coverage'])
            self.assertEqual(third['reused_charts'],3)
            self.assertEqual(third['refitted_charts'],1)
            from .skeleton_reconstruct import reconstruct as solid
            result = solid(root/'source.stl', root/'partition.npz', root/'solid',
                reference_faces=5, smoothing=.05, workers=1, from_fit=third['source_fit'], render=False)
            self.assertTrue(result['step']['watertight'])
            self.assertTrue(result['face_budget_passed'])
            self.assertEqual(result['regions']['exported_regions'], 4)
            self.assertEqual(result['regions']['exported_faces'], 4)
            self.assertFalse(result['native_solid_verified'])
            self.assertTrue(result['visual_review_required'])

    def test_internal_cut_slivers_are_absorbed_without_losing_cells(self):
        from scipy import sparse
        original = np.array([0, 0, 1, 0, 1, 1, 1]); saved = original.copy()
        graph = sparse.diags([np.ones(6), np.ones(6)], [-1, 1], shape=(7, 7)).tocsr()
        result = connected_ring(original, graph)
        np.testing.assert_array_equal(original, saved)
        self.assertEqual(len(result), len(original))
        self.assertEqual(set(result), {0, 1})
        for label in np.unique(result):
            ids = np.flatnonzero(result == label)
            self.assertEqual(sparse.csgraph.connected_components(graph[ids][:, ids], directed=False)[0], 1)

    def test_smoothing_can_slide_in_plane_without_normal_departure(self):
        p = np.c_[np.linspace(0, 2, 61), .025*(-1.)**np.arange(61), np.zeros(61)]
        t = chain_parameters(p, np.arange(len(p)))
        normal = np.tile([[[0., 0, 1], [0., 0, 1]]], (len(p)-1, 1, 1))
        cp, kt, error = fit_boundary(p, t, .055, normal, .0001)
        q = np.linspace(0, 1, 4001); c = BSpline(kt, cp, 3)
        source = np.stack([np.interp(q, t, p[:, i]) for i in range(3)], axis=1)
        self.assertLessEqual(np.linalg.norm(c(q)-source, axis=1).max(), .055+1e-10)
        self.assertLessEqual(abs(c(q)[:, 2]).max(), 1e-12)
        self.assertLess(len(cp), len(p))
        self.assertGreater(np.dot(c(0, 1), p[1]-p[0]), 0)
        self.assertGreater(np.dot(c(1, 1), p[-1]-p[-2]), 0)

    def test_both_incident_planes_bound_rounding_of_a_feature(self):
        p = np.c_[np.linspace(0, 1, 17), .02*np.sin(np.arange(17)), .015*np.cos(np.arange(17))]
        t = chain_parameters(p, np.arange(len(p)))
        normal = np.tile([[[0., 0, 1], [0., 1, 0]]], (len(p)-1, 1, 1))
        cp, kt, error = fit_boundary(p, t, .08, normal, .002)
        q = np.linspace(0, 1, 10001)
        source = np.stack([np.interp(q, t, p[:, i]) for i in range(3)], axis=1)
        residual = BSpline(kt, cp, 3)(q)-source
        self.assertLessEqual(abs(residual[:, 1:]).max(), .002+1e-10)

    def test_narrow_curved_ribbon_uses_source_normal_cone(self):
        x = np.linspace(0, 2, 81)
        v = np.r_[np.c_[x, np.zeros(len(x)), .1*np.sin(3*x)],
                  np.c_[x, np.full(len(x), .004), .1*np.sin(3*x)]]
        f = np.array([[i, i+1, i+82] for i in range(80)]+[[i, i+82, i+81] for i in range(80)])
        self.assertLess(frame(v, f)[-1].min(), .05)
        self.assertGreater(stable_frame(v, f)[-1].min(), .9)

    def test_shared_skeleton_survives_closed_step_roundtrip(self):
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        domains = [dict(region=i, triangle_ids=np.array([i]), vertex_ids=np.sort(tri)) for i, tri in enumerate(f)]
        edges, owners = incidence(f)
        assigned, curves, pairs, samples, errors = skeleton(v, f, domains, edges, owners, .05)
        cfg = Config(distance=.001, boundary=.001, controls=(15,), normal_weight=0., fairness=1e-8)
        records = []
        for di, d in enumerate(domains):
            used = d['vertex_ids']; tri = np.searchsorted(used, f[d['triangle_ids']]); use = np.flatnonzero(np.any(np.asarray(pairs) == di, axis=1))
            _, rec, info = fit_support((di, v[used], tri, vertex_normals(v[used], tri),
                np.concatenate([samples[i] for i in use]), [curves[i] for i in use], cfg))
            self.assertTrue(info['accepted'], info)
            rec.update(**d, loops=[used[x] for x in loops(boundary_edges(tri))]); records.append(rec)
        for c, pair in zip(curves, pairs):
            c['owners'] = pair
        with tempfile.TemporaryDirectory() as temp:
            result = assemble(v, f, records, assigned, edges, owners, cfg.boundary, Path(temp),
                              identity_topology=True, shared_curve_records=curves, maximum_edge_length=.5)
            self.assertTrue(result['watertight'], result)
            self.assertEqual(result['topology']['faces'], 4)
            self.assertAlmostEqual(result['topology']['volume_mm3'], 1/6, places=6)
            from .graph_export import export
            from .mesh import dump
            from OCP.Interface import Interface_Static
            dump(Path(temp)/'run.json', dict(partition='test partition', stl='test source',
                                           boundary_tolerance_mm=cfg.boundary))
            previous = Interface_Static.IVal_s('write.surfacecurve.mode')
            exported = export(temp, Path(temp)/'without_pcurves', write_pcurves=False)
            self.assertTrue(exported['watertight'])
            self.assertEqual(exported['master_3d_curves'], 0)
            self.assertEqual(Interface_Static.IVal_s('write.surfacecurve.mode'), previous)


if __name__ == '__main__':
    unittest.main()
