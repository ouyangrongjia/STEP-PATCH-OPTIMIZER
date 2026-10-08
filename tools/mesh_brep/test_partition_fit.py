"""Geometric and input-contract tests for fixed-partition full coverage."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np

from .generate import fixture
from .mesh import read_stl, write_stl
from .partition_fit import Config, fit_partition, load_partition, loops
from .partition_cad import assemble, shared_chains
from .partition_fan import FanConfig, fit_curve, reconstruct


class PartitionTests(unittest.TestCase):
    def test_planar_geometry_survives_conflicting_neighbourhood_normals(self):
        from .partition_fit import solve_graph, boundary_edges, parameters, positions
        v = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]])+[-25., 40., -8.]
        f = np.array([[0, 1, 2]])
        normals = np.tile([.8, 0., .6], (3, 1))
        rec, report = solve_graph(v, f, normals, boundary_edges(f),
            Config(distance=1e-7, boundary=1e-7, controls=(4,), normal_weight=1., boundary_normal_weight=1.))
        self.assertIsNotNone(rec)
        self.assertEqual(report['reason'], 'exact_planar_geometry_fallback')
        points = np.r_[v, v.mean(axis=0)[None]]
        np.testing.assert_allclose(positions(parameters(points, rec), rec), points, atol=1e-12)

    def test_graph_boundary_extrema_cover_peaks_between_fixed_samples(self):
        from .partition_fit import polyline_graph_error, positions
        from .splines import knots
        rec = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                   knots=knots(17), height=np.random.default_rng(492).normal(0, .01, 17*17))
        points = np.array([[.05, .1, .02], [.9, .8, -.01], [.9, .3, .005]])
        maximum = polyline_graph_error(points, rec)
        dense = np.concatenate([a+(b-a)*np.linspace(0, 1, 50001)[:, None]
                                for a, b in zip(points[:-1], points[1:])])
        dense_max = np.linalg.norm(positions(dense[:, :2], rec)-dense, axis=1).max()
        self.assertGreaterEqual(maximum, dense_max-1e-10)
        self.assertLess(maximum-dense_max, 1e-7)
        coarse = np.concatenate([a+(b-a)*np.linspace(0, 1, 9)[:, None]
                                 for a, b in zip(points[:-1], points[1:])])
        coarse_max = np.linalg.norm(positions(coarse[:, :2], rec)-coarse, axis=1).max()
        self.assertGreater(maximum-coarse_max, 1e-4)

    def test_shared_curve_refinement_resolves_unequal_segments(self):
        points = np.array([[0., 0., 0.], [1., 0., 0.], [1., .01, 0.], [2., .02, 0.]])
        distance = np.r_[0., np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
        t = distance/distance[-1]
        c, _ = fit_curve(points, t, .002)
        q = np.unique(np.r_[np.linspace(0., 1., 5001), t])
        target = np.stack([np.interp(q, t, points[:, k]) for k in range(3)], axis=1)
        predicted = np.array([c.Value(float(x)).Coord() for x in q])
        self.assertLess(np.linalg.norm(predicted-target, axis=1).max(), .002)
        self.assertEqual(c.Degree(), 3)
        self.assertTrue(all(c.Multiplicity(i) == 1 for i in range(2, c.NbKnots())))
        np.testing.assert_allclose(predicted[[0, -1]], points[[0, -1]], atol=1e-12)

    def test_shared_curve_closed_fit_keeps_fixed_cad_tolerance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root/'input', 8, 'closed')
            stl = root/'input/input.stl'; v, f = read_stl(stl)
            labels = np.array(json.loads((root/'input/layout.json').read_text())['triangle_labels'])
            partition = root/'partition.npz'
            np.savez(partition, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            reconstruct(partition, stl, root/'fit', FanConfig(curve_tolerance=.01, samples_per_patch=81))
            run = json.loads((root/'fit/run.json').read_text())
            self.assertTrue(run['watertight'], run)
            self.assertEqual(run['topology']['solids'], 1)
            self.assertLessEqual(run['topology']['maximum_edge_tolerance_mm'], 1.01e-7)
            self.assertLessEqual(run['max_curve_on_surface_mm'], 1e-7)
            with np.load(root/'fit/source_domains.npz') as index:
                np.testing.assert_array_equal(index['source_labels'], labels)
                np.testing.assert_array_equal(index['domain_region'][index['face_domain']], labels)

    def test_closed_multiregion_fit_step_roundtrip(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root/'input', 12, 'closed')
            stl = root/'input/input.stl'; v, f = read_stl(stl)
            labels = np.array(json.loads((root/'input/layout.json').read_text())['triangle_labels'])
            source = root/'partition.npz'
            np.savez(source, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            v, f, loaded, report = load_partition(source, stl)
            self.assertEqual(report['euler'], 2)
            np.testing.assert_array_equal(loaded, labels)
            out = root/'fit'; out.mkdir()
            config = Config(distance=.02, boundary=.001, controls=(4, 7, 11))
            rec, assigned, edges, owners = fit_partition(v, f, labels, config, out)
            self.assertTrue(np.all(assigned >= 0))
            self.assertEqual(sum(len(r['triangle_ids']) for r in rec), len(f))
            self.assertTrue(all(np.all(labels[r['triangle_ids']] == r['region']) for r in rec))
            chains, adjacent = shared_chains(edges, owners, assigned)
            self.assertEqual(sum(len(c)-1 for c in chains), np.sum(assigned[owners[:, 0]] != assigned[owners[:, 1]]))
            result = assemble(v, f, rec, assigned, edges, owners, config.boundary, out)
            self.assertTrue(result['watertight'], result)
            self.assertEqual(result['topology']['solids'], 1)
            self.assertEqual(result['topology']['free_edges'], 0)
            with np.load(source) as original:
                np.testing.assert_array_equal(original['face_instance_id'], labels)
            np.savez(root/'wrong.npz', mesh_vertex_original=v+.001, mesh_triangle=f, face_instance_id=labels)
            with self.assertRaisesRegex(ValueError, 'coordinates/order'):
                load_partition(root/'wrong.npz', stl)

    def test_small_translated_regions_retain_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root/'input', 8, 'closed')
            stl = root/'input/input.stl'; v, f = read_stl(stl)
            labels = np.array(json.loads((root/'input/layout.json').read_text())['triangle_labels'])
            write_stl(root/'small.stl', v*.05+[-25., 40., -8.], f)
            v, f = read_stl(root/'small.stl')
            np.savez(root/'small.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            reconstruct(root/'small.npz', root/'small.stl', root/'fit',
                        FanConfig(curve_tolerance=.0005, samples_per_patch=81))
            run = json.loads((root/'fit/run.json').read_text())
            self.assertTrue(run['watertight'], run)
            from .partition_fan_evaluate import metrics
            quality = metrics(root/'fit', forward_samples=600, reverse_per_patch=16)
            self.assertLess(quality['sampled_source_to_surface_upper_bound_mm']['max'], .002)
            self.assertLess(quality['sampled_surface_to_source_mesh_mm']['max'], .002)

    def test_pinched_boundary_rejected_and_hole_kept(self):
        boundary = np.array([[0, 1], [1, 2], [2, 3], [3, 0], [4, 7], [7, 6], [6, 5], [5, 4]])
        self.assertEqual(len(loops(boundary)), 2)
        with self.assertRaisesRegex(ValueError, 'Pinched'):
            loops(np.array([[0, 1], [1, 2], [2, 0], [0, 3], [3, 4], [4, 0]]))


if __name__ == '__main__':
    unittest.main()
