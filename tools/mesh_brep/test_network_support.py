import unittest
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import numpy as np
from .network_support import guard, validate_wires
from .splines import knots


class NetworkSupportTests(unittest.TestCase):
    def fixture(self):
        v = np.array([[0., 0., 0.], [1., 0., 0.], [1., 1., 0.], [0., 1., 0.]])
        f = np.array([[0, 1, 2], [0, 2, 3]])
        records = {0: dict(vertex_ids=np.arange(4), triangle_ids=np.arange(2))}
        curves = {i: dict(chain=np.array([i, (i+1)%4]), poles=np.linspace(v[i], v[(i+1)%4], 4),
                          parameters=np.array([0., 1.]), knots=knots(4)) for i in range(4)}
        return v, f, records, curves

    def test_closed_unmoved_trim_is_accepted(self):
        v, f, records, curves = self.fixture()
        self.assertEqual(validate_wires(v, f, records, curves, {0}), [])

    def test_crossing_between_shared_junctions_is_rejected(self):
        v, f, records, curves = self.fixture()
        curves[1]['poles'][1:3, 0] = -.8
        failures = validate_wires(v, f, records, curves, {0})
        self.assertTrue(any(r['kind'] == 'continuous_intersection' for r in failures), failures)

    def test_unrefitted_network_cannot_enter_cad_assembly(self):
        from .graph_reassemble import reconstruct
        with TemporaryDirectory() as folder:
            root = Path(folder); source = root/'source'; source.mkdir()
            meta = dict(partition='unused.npz', stl='unused.stl', config=dict(boundary=.005),
                        requires_support_refit=True)
            (source/'run.json').write_text(json.dumps(meta), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'incident-support validation'):
                reconstruct(source, root/'cad')
            self.assertEqual(json.loads((root/'cad/run.json').read_text(encoding='utf-8'))['status'], 'failed')

    def test_planar_boundary_bump_is_restored_even_when_trim_is_valid(self):
        v, f, records, curves = self.fixture()
        for c in curves.values():
            c['owners'] = np.array([0])
        with TemporaryDirectory() as folder:
            root = Path(folder); partition = root/'partition.npz'
            np.savez_compressed(partition, mesh_vertex_original=v, mesh_triangle=f)
            for name in ('baseline', 'candidate'):
                source = root/name; (source/'charts').mkdir(parents=True); (source/'curves').mkdir()
                np.savez_compressed(source/'chart_index.npz', face_chart=np.zeros(len(f), dtype=int))
                np.savez_compressed(source/'charts/00000.npz', **records[0])
                for ci, c in curves.items():
                    np.savez_compressed(source/'curves'/f'{ci:05d}.npz', **c)
                (source/'fitting.json').write_text('[]', encoding='utf-8')
                (source/'run.json').write_text(json.dumps(dict(partition=str(partition),
                    required_refit_charts=[0], source_plane_budget_mm=.01, smoothing_mm=.1)), encoding='utf-8')
            curves[0]['poles'][1:3, 1] = .04
            self.assertEqual(validate_wires(v, f, records, curves, {0}), [])
            np.savez_compressed(root/'candidate/curves/00000.npz', **curves[0])
            report = guard(root/'candidate', root/'baseline', root/'guarded', geometric_guard=True)
            self.assertTrue(report['passed'])
            self.assertEqual(report['restored_curves'], [0])
            with np.load(root/'guarded/curves/00000.npz') as actual:
                np.testing.assert_array_equal(actual['poles'], np.linspace(v[0], v[1], 4))


if __name__ == '__main__':
    unittest.main()
