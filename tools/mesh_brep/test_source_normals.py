import unittest
import numpy as np
from .source_normals import crease_fan_normals


class CreaseFanTests(unittest.TestCase):
    def test_crease_endpoint_cannot_be_bypassed_by_a_smooth_path(self):
        from scipy import sparse
        from scipy.sparse.csgraph import connected_components
        from .graph_repair import feature_components
        # Three triangles around one interior vertex. Two gentle joins form
        # a bypass around the third, sharp join, so ordinary components fail.
        v = np.array([[0.,0.,0.],[-1.,-1.,0.],[1.,-1.,0.],[0.,1.,.3]])
        f = np.array([[0,1,2],[0,2,3],[0,3,1]])
        normal = np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        normal /= np.linalg.norm(normal,axis=1)[:,None]
        pairs = np.array([[0,1],[0,2],[1,2]])
        dots = np.sum(normal[pairs[:,0]]*normal[pairs[:,1]],axis=1)
        threshold = np.cos(np.radians(25.))
        smooth = pairs[dots >= threshold]
        graph = sparse.csr_matrix((np.ones(smooth.size),(smooth.ravel(),smooth[:,::-1].ravel())),shape=(3,3))
        self.assertEqual(connected_components(graph,directed=False)[0],1)
        adjacency = sparse.csr_matrix((np.repeat(2+dots,2),(pairs.ravel(),pairs[:,::-1].ravel())),shape=(3,3))
        groups = feature_components(np.arange(3),adjacency,threshold)
        self.assertEqual(len(groups),2)
        np.testing.assert_array_equal(np.sort(np.concatenate(groups)),np.arange(3))
        for group in groups:
            self.assertFalse(1 in group and 2 in group)
            self.assertEqual(connected_components(graph[group][:,group],directed=False)[0],1)

    def test_feature_split_keeps_region_and_covers_a_creased_roof_exactly(self):
        from dataclasses import asdict
        from pathlib import Path
        import tempfile
        from .mesh import dump, write_stl, read_stl
        from .partition_fit import Config, fit_partition, boundary_edges
        from .graph_reconstruct import reconstruct
        v = np.array([[x,y,.2*abs(x)] for x in [-1.,0.,1.] for y in [-1.,1.]]+[[0.,0.,-2.]])
        roof = np.array([[0,2,3],[0,3,1],[2,4,5],[2,5,3]])
        boundary = boundary_edges(roof)
        f = np.r_[roof, np.c_[boundary[:, ::-1], np.full(len(boundary), 6)]]
        labels = np.r_[np.zeros(len(roof), dtype=int), np.arange(1,len(boundary)+1)]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); write_stl(root/'input.stl', v, f); v, f = read_stl(root/'input.stl')
            np.savez(root/'partition.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            config = Config(distance=.1, boundary=.1, controls=(4,), normal_weight=0.)
            fit = root/'fit'; fit.mkdir(); fit_partition(v, f, labels, config, fit)
            dump(fit/'run.json',dict(config=asdict(config),partition=str(root/'partition.npz'),
                 stl=str(root/'input.stl'),status='DiagnosticOnly'))
            old = list((fit/'charts').glob('*.npz'))
            result = reconstruct(root/'input.stl',root/'partition.npz',root/'split',
                from_fit=fit,feature_split_angle=20.,reference_faces=len(old)+2,render=False)
            self.assertEqual(result['fitted_charts'],len(old)+1)
            self.assertTrue(result['step']['watertight'])
            self.assertEqual(result['step']['topology']['faces'],len(old)+1)
            records = [dict(np.load(p)) for p in sorted((Path(result['native_fit'])/'charts').glob('*.npz'))]
            np.testing.assert_array_equal(np.sort(np.concatenate([r['triangle_ids'] for r in records])),np.arange(len(f)))
            roof_records = [r for r in records if int(r['region'])==0]
            self.assertEqual(len(roof_records),2)
            for rec in records:
                self.assertTrue(np.all(labels[rec['triangle_ids']]==int(rec['region'])))
            for rec in roof_records:
                tri = f[rec['triangle_ids']]
                n = np.cross(v[tri[:,1]]-v[tri[:,0]],v[tri[:,2]]-v[tri[:,0]])
                n /= np.linalg.norm(n,axis=1)[:,None]
                np.testing.assert_allclose(n,np.broadcast_to(n[0],n.shape),atol=1e-14)

    def test_narrow_curved_ribbon_uses_regular_normal_cone_frame(self):
        from .partition_fit import frame, solve_graph, boundary_edges, Config, parameters, positions
        angle = np.linspace(-.45, .45, 13)
        v = np.array([[np.cos(t), np.sin(t), z] for t in angle for z in [-.0003, .0003]])
        f = np.array([[2*i, 2*i+2, 2*i+3] for i in range(12)] +
                     [[2*i, 2*i+3, 2*i+1] for i in range(12)])
        normals = v.copy(); normals[:, 2] = 0
        config = Config(distance=.002, boundary=.002, controls=(4, 7, 11), fairness=1e-14, normal_weight=0.)
        original, _ = solve_graph(v, f, normals, boundary_edges(f), config)
        self.assertIsNone(original)
        fitted, report = solve_graph(v, f, normals, boundary_edges(f), config, condition_frame=True)
        self.assertIsNotNone(fitted, report)
        self.assertGreater(np.min(normals@fitted['axes'][2]), .8)
        self.assertLess(np.linalg.norm(positions(parameters(v, fitted), fitted)-v, axis=1).max(), .002)

    def test_cube_retains_creases_and_shares_diagonal_normals(self):
        v = np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0],
                      [0,0,1],[1,0,1],[1,1,1],[0,1,1]], dtype=float)
        f = np.array([[0,2,1],[0,3,2],[4,5,6],[4,6,7],[0,1,5],[0,5,4],
                      [1,2,6],[1,6,5],[2,3,7],[2,7,6],[3,0,4],[3,4,7]])
        n = crease_fan_normals(v, f, 30.)
        expected = np.cross(v[f[:,1]]-v[f[:,0]], v[f[:,2]]-v[f[:,0]])
        expected /= np.linalg.norm(expected, axis=1)[:,None]
        np.testing.assert_allclose(n, np.broadcast_to(expected[:,None], n.shape), atol=1e-14)
        filtered = crease_fan_normals(v, f, 30., filter_steps=12)
        np.testing.assert_allclose(filtered, n, atol=1e-14)
        # With crease splitting disabled every corner at a vertex shares a fan.
        shared = crease_fan_normals(v, f, 180.)
        for vertex in range(len(v)):
            samples = shared[f == vertex]
            np.testing.assert_allclose(samples, np.broadcast_to(samples[0], samples.shape), atol=1e-14)

    def test_filtered_normals_reduce_plane_noise_without_moving_source_or_losing_shared_fans(self):
        # A closed thin box whose upper grid has small deterministic noise.
        xy = np.stack(np.meshgrid(np.linspace(-1, 1, 13), np.linspace(-1, 1, 13)), axis=-1).reshape(-1, 2)
        from scipy.spatial import Delaunay
        top = Delaunay(xy).simplices
        z = .003*np.random.default_rng(5841).standard_normal(len(xy))
        v = np.r_[np.c_[xy, z], [[0., 0., -2.]]]
        from .partition_fit import boundary_edges
        boundary = boundary_edges(top)
        f = np.r_[top, np.c_[boundary[:, ::-1], np.full(len(boundary), len(xy))]]
        before = v.copy(); raw = crease_fan_normals(v, f, 30.)
        filtered = crease_fan_normals(v, f, 30., filter_steps=12)
        np.testing.assert_array_equal(v, before)
        interior = np.max(abs(xy[top]), axis=(1, 2)) < .8
        self.assertLess(np.mean(filtered[:len(top)][interior, :, :2]**2),
                        np.mean(raw[:len(top)][interior, :, :2]**2))
        for vertex in np.flatnonzero(np.max(abs(xy), axis=1) < .8):
            samples = filtered[f == vertex]
            np.testing.assert_allclose(samples, np.broadcast_to(samples[0], samples.shape), atol=1e-14)
        with self.assertRaises(ValueError):
            crease_fan_normals(v, f, filter_steps=-1)


if __name__ == '__main__':
    unittest.main()
