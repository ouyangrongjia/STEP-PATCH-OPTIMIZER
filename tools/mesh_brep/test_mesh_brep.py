"""Numerical invariants, topology rejection and a small complete B-Rep chain."""
import json
import tempfile
import unittest
from pathlib import Path
import numpy as np
from .mesh import read_stl, write_stl, charts, audit, mean_value, side_uv
from .generate import fixture
from .reconstruct import reconstruct
from .splines import knots, tensor, evaluate, shared_maps, side_indices


class MeshBRepTests(unittest.TestCase):
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
