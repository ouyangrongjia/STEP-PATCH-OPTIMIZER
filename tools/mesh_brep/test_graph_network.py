"""Geometry checks for shared curves, graph extension and regional merging."""
import copy
from dataclasses import asdict
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from scipy.interpolate import BSpline
from .splines import knots
from .partition_fit import Config, parameters, positions, spline_graph_error, solve_graph, boundary_edges
from .joint_graph_boundary import extend_support
from .regional_rectangle import compact_curve, curve_polyline_error
from .graph_merge import merge
from .mesh import read_stl, dump, vertex_normals
from .generate import fixture


class GraphNetworkTests(unittest.TestCase):
    def test_locally_positive_projection_can_still_cover_itself(self):
        from .partition_fit import projected_boundary_valid
        # A ribbon winds more than once: local triangle Jacobians are positive
        # while the global projected boundary crosses itself.
        theta = np.linspace(0, 2.5*np.pi, 31)
        uv = np.r_[np.c_[np.cos(theta), np.sin(theta)],
                   2*np.c_[np.cos(theta), np.sin(theta)]]
        tri = np.array([[i, i+31, i+32] for i in range(30)]
                       + [[i, i+32, i+1] for i in range(30)])
        p = uv[tri]; e, f = p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]
        self.assertTrue(np.all(e[:, 0]*f[:, 1]-e[:, 1]*f[:, 0] > 0))
        self.assertFalse(projected_boundary_valid(uv, boundary_edges(tri)))
        self.assertTrue(projected_boundary_valid(uv, boundary_edges(tri[:15])))

    def test_closed_cavity_and_disjoint_bodies_survive_step(self):
        from .partition_fit import incidence, loops
        from .partition_cad import assemble
        base = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        faces = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        cases = [(np.r_[base*4, base*.5+.4], np.r_[faces, faces[:, ::-1]+4], 1, (64-.125)/6),
                 (np.r_[base, base+[2., 0, 0]], np.r_[faces, faces+4], 2, 1/3)]
        for vertices, triangles, expected, volume in cases:
            with self.subTest(expected_solids=expected), tempfile.TemporaryDirectory() as temp:
                cfg = Config(distance=1e-5, boundary=1e-5, controls=(4,), normal_weight=0.)
                records = []
                for i, face in enumerate(triangles):
                    used = np.sort(face); tri = np.searchsorted(used, face)[None]
                    rec, _ = solve_graph(vertices[used], tri, vertex_normals(vertices[used], tri), boundary_edges(tri), cfg)
                    rec.update(triangle_ids=np.array([i]), vertex_ids=used, region=i,
                               loops=[used[x] for x in rec['loops']])
                    records.append(rec)
                edges, owners = incidence(triangles)
                result = assemble(vertices, triangles, records, np.arange(len(triangles)), edges, owners,
                                  cfg.boundary, Path(temp), identity_topology=True, segment_edges=True)
                self.assertTrue(result['watertight'], result)
                self.assertEqual(result['topology']['solids'], expected)
                self.assertEqual(result['topology']['shells'], 2)
                self.assertAlmostEqual(result['topology']['volume_mm3'], volume, places=6)

    def test_extension_preserves_geometry_and_derivatives(self):
        r = dict(origin=np.array([-28., 40., -7.]), axes=np.eye(3), lo=np.array([-.3, -.2]),
                 size=np.array([.2, .5]), knots=knots(11), height=np.random.default_rng(319).normal(0, .01, 121))
        original = copy.deepcopy(r); q = np.random.default_rng(51).uniform(0, 1, (100, 2))
        xyz = positions(q, original); extend_support(r, .02)
        np.testing.assert_array_equal(r['knots'][:4], np.zeros(4))
        np.testing.assert_array_equal(r['knots'][-4:], np.ones(4))
        self.assertTrue(np.isfinite(positions(np.array([[0., 0.], [1., 1.]]), r)).all())
        np.testing.assert_allclose(positions(parameters(xyz, r), r), xyz, atol=1e-12)
        for axis in range(2):
            before = positions(q, original, int(axis == 0), int(axis == 1))/original['size'][axis]
            after = positions(parameters(xyz, r), r, int(axis == 0), int(axis == 1))/r['size'][axis]
            np.testing.assert_allclose(after, before, atol=1e-11)

    def test_spatial_curve_graph_extrema_cover_dense_check(self):
        r = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                 knots=knots(11), height=np.random.default_rng(37).normal(0, .01, 121))
        poles = np.array([[.03, .07, -.003], [.2, .8, .04], [.65, .1, -.02], [.96, .91, .01]])
        t = knots(4); maximum = spline_graph_error(poles, t, r)
        points = BSpline(t, poles, 3)(np.linspace(0, 1, 60001))
        measured = np.linalg.norm(positions(points[:, :2], r)-points, axis=1).max()
        self.assertGreaterEqual(maximum, measured-1e-11)
        self.assertLess(maximum-measured, 1e-7)
        located, parameter = spline_graph_error(poles, t, r, return_parameter=True)
        peak = BSpline(t, poles, 3)(parameter)
        actual = np.linalg.norm(positions(peak[None, :2], r)[0]-peak)
        self.assertAlmostEqual(located, maximum, places=11)
        self.assertAlmostEqual(actual, maximum, places=11)

    def test_curve_fit_covers_long_and_short_source_intervals(self):
        p = np.array([[0., 0., 0.], [.4, .03, .01], [.401, .05, .01], [.9, -.04, .02],
                      [.902, -.041, .02], [1., .02, 0.]])
        t = np.r_[0., np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]; t /= t[-1]
        c, error = compact_curve(p, t, .002)
        cp = np.array([c.Pole(i).Coord() for i in range(1, c.NbPoles()+1)])
        kt = np.repeat([c.Knot(i) for i in range(1, c.NbKnots()+1)],
                       [c.Multiplicity(i) for i in range(1, c.NbKnots()+1)])
        maximum, _ = curve_polyline_error(cp, kt, p, t)
        dense = np.linspace(0, 1, 40001)
        target = np.stack([np.interp(dense, t, p[:, k]) for k in range(3)], axis=1)
        measured = np.linalg.norm(BSpline(kt, cp, 3)(dense)-target, axis=1).max()
        self.assertLessEqual(maximum, .002)
        self.assertLessEqual(measured, error+1e-10)
        self.assertGreaterEqual(maximum, measured-1e-10)

    def test_merge_retains_original_triangle_partition_and_reduces_charts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root/'input', 2, 'closed'); stl = root/'input/input.stl'
            v, f = read_stl(stl); labels = np.array(json.loads((root/'input/layout.json').read_text())['triangle_labels'])
            part = root/'partition.npz'; np.savez(part, mesh_vertex_original=v, mesh_triangle=f, face_instance_id=labels)
            fit = root/'fit'; (fit/'charts').mkdir(parents=True)
            cfg = Config(distance=.2, boundary=.05, controls=(4, 7, 11), normal_weight=0.)
            for i, face in enumerate(f):
                tri = np.array([[0, 1, 2]]); rec, _ = solve_graph(v[face], tri, vertex_normals(v[face], tri),
                                                                 boundary_edges(tri), cfg)
                rec.pop('uv'); rec.pop('loops'); rec.update(vertex_ids=face, triangle_ids=np.array([i]), region=labels[i])
                np.savez(fit/'charts'/f'{i:05d}.npz', **rec)
            np.savez(fit/'chart_index.npz', face_chart=np.arange(len(f)), source_labels=labels)
            dump(fit/'run.json', dict(partition=str(part), stl=str(stl), config=asdict(cfg)))
            report = merge(fit, root/'merged', trials=200)
            self.assertLess(report['fitted_charts'], len(f))
            index = np.load(root/'merged/chart_index.npz')['face_chart']
            recovered = np.array([int(np.load(path)['region']) for path in sorted((root/'merged/charts').glob('*.npz'))])
            np.testing.assert_array_equal(recovered[index], labels)
            self.assertEqual(report['accepted_merges'], len(f)-report['fitted_charts'])


if __name__ == '__main__':
    unittest.main()
