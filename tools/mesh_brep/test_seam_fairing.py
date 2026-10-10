import unittest
import numpy as np
from .seam_fairing import normal_jacobian
from .splines import knots


class SeamFairingTests(unittest.TestCase):
    def test_unit_normal_derivative_in_rotated_anisotropic_chart(self):
        angle = .7
        axes = np.array([[np.cos(angle), 0., np.sin(angle)], [0., 1., 0.],
                         [-np.sin(angle), 0., np.cos(angle)]])
        rec = dict(origin=np.array([3., -2., 7.]), axes=axes, lo=np.array([-.3, .2]),
                   size=np.array([2., .15]), knots=knots(6), height=np.random.default_rng(8).normal(0, .01, 36))
        uv = np.array([[.2, .7], [.7, .3], [.45, .55]])
        xyz = rec['origin']+(uv*rec['size']+rec['lo'])@axes[:2]
        n, jac = normal_jacobian(rec, xyz)
        np.testing.assert_allclose(np.linalg.norm(n, axis=1), 1., atol=1e-13)
        delta = np.random.default_rng(4).normal(size=36)
        eps = 1e-7
        plus, _ = normal_jacobian(dict(rec, height=rec['height']+eps*delta), xyz)
        minus, _ = normal_jacobian(dict(rec, height=rec['height']-eps*delta), xyz)
        predicted = np.column_stack([j@delta for j in jac])
        np.testing.assert_allclose(predicted, (plus-minus)/(2*eps), rtol=1e-7, atol=1e-7)
        np.testing.assert_allclose(np.sum(predicted*n, axis=1), 0., atol=1e-12)

    def test_height_translation_does_not_change_normals(self):
        rec = dict(origin=np.zeros(3), axes=np.eye(3), lo=np.zeros(2), size=np.ones(2),
                   knots=knots(5), height=np.linspace(-.1, .2, 25))
        xyz = np.array([[.2, .6, 0.], [.8, .3, 0.]])
        before, jac = normal_jacobian(rec, xyz)
        after, _ = normal_jacobian(dict(rec, height=rec['height']+4.), xyz)
        np.testing.assert_allclose(before, after, atol=1e-13)
        for j in jac:
            np.testing.assert_allclose(j@np.ones(25), 0., atol=1e-13)


if __name__ == '__main__':
    unittest.main()
