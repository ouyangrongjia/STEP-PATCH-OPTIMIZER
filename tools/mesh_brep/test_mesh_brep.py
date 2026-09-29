"""Numerical invariants, topology rejection and a small complete B-Rep chain."""
import json
import tempfile
import unittest
from pathlib import Path
import numpy as np
from .mesh import read_stl, write_stl, charts, audit, mean_value, side_uv
from .generate import fixture
from .reconstruct import reconstruct
from .splines import knots, tensor, evaluate, shared_maps, side_indices, quadrature, joint_fit


class MeshBRepTests(unittest.TestCase):
    def test_bicubic_fairness_integral(self):
        t = knots(4); p = np.zeros(16); p[-1] = 1.  # u^3 v^3
        q, w = quadrature(t, t, 4)
        self.assertAlmostEqual(float(w@np.square(tensor(q, t, t, 2, 0)@p)), 12/7, places=12)
        midpoint, mw = quadrature(t, t, 1)
        self.assertGreater(abs(float(mw@np.square(tensor(midpoint, t, t, 2, 0)@p))-12/7), 1.)

    def test_hard_tangent_resists_conflicting_data(self):
        from scipy import sparse
        t = knots(4); q, _ = quadrature(t, t, 5)
        spec = dict(uv=q, points=np.c_[q, .1*q[:, 0]], normals=np.tile([0., 0., 1.], (len(q), 1)),
                    tu=t, tv=t, lengths=[1., 1.])
        probe = np.array([[.5, .5]])
        poles, report = joint_fit([spec], [sparse.eye(16)], [np.zeros((16, 3))], [],
                                 normal_weight=0., fairness=0.,
                                 hard_tangents=[(0, probe, np.array([[0., 0., 1.]]), [1., 0.])])
        derivative = tensor(probe, t, t, 1, 0)@poles[0].reshape(-1, 3)
        self.assertLess(abs(derivative[0, 2]), 1e-10)
        self.assertLess(report['hard_tangent_max_residual'], 1e-10)

    def test_inconsistent_hard_positions_are_rejected(self):
        from scipy import sparse
        t = knots(4); q, _ = quadrature(t, t, 5)
        spec = dict(uv=q, points=np.c_[q, q[:, 0]*0], normals=np.tile([0., 0., 1.], (len(q), 1)),
                    tu=t, tv=t, lengths=[1., 1.])
        uv = np.array([[.5, .5]])
        with self.assertRaisesRegex(ValueError, 'hard constraints'):
            joint_fit([spec], [sparse.eye(16)], [np.zeros((16, 3))], [],
                      hard_positions=[(0, uv, np.array([[.5, .5, 0.]])), (0, uv, np.array([[.5, .5, 1.]]))])

    def test_compact_network_preserves_shared_geometry_with_unequal_bases(self):
        from .compact import build, variable_maps, unpack, side_uv as mapped_side
        from .generate import emit
        outer = np.array([[0., 0.], [2., 0.], [2., 2.], [0., 2.]])
        inner = .5+.5*outer
        polygons = [np.array([outer[i], outer[(i+1) % 4], inner[(i+1) % 4], inner[i]]) for i in range(4)]+[inner]
        vertices, triangles, labels, corners = [], [], [], []
        for fi, poly in enumerate(polygons):
            q = np.linspace(0, 1, 17); u, v = np.meshgrid(q, q, indexing='ij')
            p = ((1-u)*(1-v))[..., None]*poly[0]+(u*(1-v))[..., None]*poly[1]+(u*v)[..., None]*poly[2]+((1-u)*v)[..., None]*poly[3]
            xyz = np.dstack([p, .07*np.sin(p[:, :, 0]*2)*np.cos(p[:, :, 1])])
            idx = np.arange(17*17).reshape(17, 17)+len(vertices); vertices.extend(xyz.reshape(-1, 3))
            for i in range(16):
                for j in range(16):
                    a, b, c, d = idx[i, j], idx[i+1, j], idx[i+1, j+1], idx[i, j+1]
                    triangles.extend([[a, b, c], [a, c, d]]); labels.extend([fi, fi])
            corners.append(idx[[0, -1, -1, 0], [0, 0, -1, -1]].tolist())
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)/'mesh'; emit(folder, vertices, triangles, labels, corners, {'kind': 'unit-test-only'})
            v, tri = read_stl(folder/'input.stl'); meta = json.loads((folder/'layout.json').read_text())
            atlas = charts(v, tri, meta); layout = build(v, atlas, 9, 7); maps = variable_maps(layout)
            delta = np.random.default_rng(81).normal(scale=.05, size=(maps[0].shape[1], 3))
            values = []
            for surface, mapping in zip(layout['surfaces'], maps):
                p, tu, tv = unpack(surface); values.append(((p.reshape(-1, 3)+mapping@delta).reshape(p.shape), tu, tv))
            for sid in range(8):
                curves = []
                for fi, sides in enumerate(layout['sides']):
                    for side in sides:
                        if not side['outer'] and side['seam'] == sid:
                            p, tu, tv = values[fi]
                            curves.append(evaluate(p, tu, tv, mapped_side(side['mapping'], np.linspace(0, 1, 137)))[0])
                np.testing.assert_allclose(curves[0], curves[1], atol=1e-11)
            for fi in range(4):
                uv = mapped_side(layout['sides'][fi][0]['mapping'], np.linspace(0, 1, 137))
                initial, tu, tv = unpack(layout['surfaces'][fi]); changed = values[fi][0]
                np.testing.assert_allclose(evaluate(initial, tu, tv, uv)[0], evaluate(changed, tu, tv, uv)[0], atol=1e-12)

    def test_stl_does_not_weld_thin_wall(self):
        p = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]])
        vertices = np.r_[p, p+np.array([0, 0, 1e-5])]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'thin.stl'; write_stl(path, vertices, np.array([[0, 1, 2], [3, 5, 4]]))
            v, t = read_stl(path)
            self.assertEqual(len(v), 6)
            self.assertEqual(audit(v, t)['components'], 2)

    def test_mvc_affine_reproduction(self):
        vertices = np.array([[0., 0, 0], [2., 0, 0], [2., 1, 0], [0., 1, 0], [.73, .41, 0]])
        tri = np.array([[0, 1, 4], [1, 2, 4], [2, 3, 4], [3, 0, 4]])
        uv, residual = mean_value(vertices, tri, np.arange(4), vertices[:4, :2]/[2, 1])
        np.testing.assert_allclose(uv[4], vertices[4, :2]/[2, 1], atol=1e-12)
        self.assertLess(residual, 1e-12)

    def test_shared_boundary_coefficients(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)/'input'; fixture(folder, 8, 'closed')
            v, t = read_stl(folder/'input.stl'); meta = json.loads((folder/'layout.json').read_text(encoding='utf-8'))
            atlas = charts(v, t, meta); maps, offsets, uses, free = shared_maps(v, atlas, 7)
            rng = np.random.default_rng(7); coeff = rng.normal(size=(free, 3))
            poles = [(m@coeff+p0).reshape(7, 7, 3) for m, p0 in zip(maps, offsets)]
            samples = np.linspace(0, 1, 117); knot = knots(7)
            for pair in uses.values():
                self.assertEqual(len(pair), 2); values = []
                for e in pair:
                    uv = side_uv(e['side'], 1-samples if e['reverse'] else samples)
                    values.append(evaluate(poles[e['face']], knot, knot, uv)[0])
                np.testing.assert_allclose(values[0], values[1], atol=1e-12)

    def test_boundary_projection_on_skew_plane(self):
        from .cad import construct
        from .evaluate import distances
        t = knots(4); g = np.array([0., 1/3, 2/3, 1.])
        u, v = np.meshgrid(g, g, indexing='ij')
        p = np.stack([u+.6*v, v, u*0], axis=-1)
        shape = construct([p], [(t, t)], [1], False)
        point = np.array([[-.2, .6, .03]])
        report, arrays = distances(point, shape)
        optimum = np.dot(point[0, :2], [.6, 1])/(1+.6**2)
        expected = np.linalg.norm(point[0]-[.6*optimum, optimum, 0.])
        self.assertAlmostEqual(arrays['distance'][0], expected, places=9)
        self.assertTrue(report['kernel_agreement_pass'])

    def test_freeform_closed_stl_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)/'input'; fixture(folder, 12, 'closed')
            report = reconstruct(folder/'input.stl', folder/'layout.json', Path(tmp)/'result', 9, 6000)
            self.assertEqual(report['topology']['solids'], 1)
            self.assertEqual(report['topology']['free_edges'], 0)
            self.assertTrue(report['topology']['brep_valid'])
            self.assertEqual(report['cad_checks']['same_parameter_or_range_failures'], 0)
            self.assertLess(report['cad_checks']['max_curve_on_surface_mm'], 1e-7)
            self.assertEqual(report['expression']['actual_support_control_points'], 486)
            self.assertFalse(report['can_apply'])
            # Reject undeclared exact geometry even if it would otherwise be ignored.
            metadata = json.loads((folder/'layout.json').read_text(encoding='utf-8')); metadata['cad_uv'] = []
            (folder/'bad.json').write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, 'undeclared'):
                reconstruct(folder/'input.stl', folder/'bad.json', Path(tmp)/'bad', 9, 6000)


if __name__ == '__main__':
    unittest.main()
