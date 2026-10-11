import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from OCP.BRep import BRep_Builder
from OCP.BRepTools import BRepTools
from OCP.TopAbs import TopAbs_FACE
from OCP.TopoDS import TopoDS_Compound, TopoDS_Shape
from . import cad
from .distance_guard import Options, select_faces, support_groups, match_faces, validate_fit, repair
from . import fit_checkpoint
from .mesh import write_stl
from .partition_fit import Config, parameters
from .skeleton_graph import reconstruct as fit
from .skeleton_reconstruct import reconstruct
from .splines import knots, tensor


class DistanceGuardTests(unittest.TestCase):
    def test_all_outliers_have_witnesses_and_area_audit_can_trigger_repair(self):
        rows = [dict(face=i, source_distance_mm=dict(max=.031), worst_distance=dict(xyz=[i, 0, 0]))
                for i in range(30)]
        selected, failed = select_faces(dict(per_face=rows), {}, .03, Options())
        self.assertEqual(len(selected), 30)
        self.assertEqual(len(failed), 30)
        row = dict(face=2, source_distance_mm=dict(max=.035), worst_distance=dict(xyz=[2, 1, 0]))
        selected, _ = select_faces(dict(per_face=rows), dict(per_face_distance=[row]), .03, Options())
        self.assertEqual(selected[2]['worst_distance']['xyz'], [2, 1, 0])
        good = dict(per_face=[dict(face=0, source_distance_mm=dict(max=.029))])
        self.assertEqual(select_faces(good, {}, .03, Options())[0], [])
        self.assertEqual(len(select_faces(good, {}, .03, Options(), preventive=True)[0]), 1)
        mixed = dict(per_face=good['per_face']+[dict(face=1, source_distance_mm=dict(max=.0301))])
        short, bad = select_faces(mixed, {}, .03, Options(short_repair_seconds=2.))
        self.assertEqual([r['face'] for r in short], [1])
        self.assertEqual(bad, [1])
        rows[0]['source_distance_mm']['max'] = float('nan')
        with self.assertRaisesRegex(ValueError, 'non-finite'):
            select_faces(dict(per_face=rows), {}, .03, Options())

    def test_shared_support_members_are_atomic(self):
        rec = {k: np.ones(3) for k in ('origin', 'axes', 'lo', 'size', 'knots', 'height')}
        rec['shared_support_group'] = np.array([0, 1])
        records = {0: dict(rec), 1: dict(rec), 2: {k: v for k, v in rec.items() if k != 'shared_support_group'}}
        self.assertEqual(support_groups(records, [1, 2]), [[0, 1], [2]])
        records[1]['height'] = np.zeros(3)
        with self.assertRaisesRegex(ValueError, 'different geometry'):
            support_groups(records, [0])

    def test_real_bump_is_repaired_before_delivery_and_exhaustion_keeps_failure(self):
        v = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [0., 0, 1]])
        f = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); write_stl(root/'source.stl', v, f)
            np.savez_compressed(root/'partition.npz', mesh_vertex_original=v, mesh_triangle=f, face_instance_id=np.arange(4))
            cfg = Config(distance=.001, boundary=.001, controls=(21,), normal_weight=0., fairness=1e-8)
            fitted = fit(root/'partition.npz', root/'source.stl', root/'first', smoothing=.05,
                         workers=1, rounds=2, face_limit=4, config=cfg)
            source = Path(fitted['source_fit']); path = source/'charts/00000.npz'; rec = dict(np.load(path))
            rec['knots'] = knots(21); rec['height'] = np.zeros(21*21)
            q = parameters(v[f[0]].mean(axis=0)[None], rec)
            index = tensor(q, rec['knots'], rec['knots']).toarray()[0].argmax()
            rec['height'][index] = .012
            np.savez_compressed(path, **rec)
            curves = {p.name: p.read_bytes() for p in (source/'curves').glob('*.npz')}
            options = Options(max_repairs=1, samples_per_face=128, repair_samples=1024, seed=44500,
                              preserve_continuity=True)
            with self.assertRaisesRegex(ValueError, 'budget exhausted'):
                reconstruct(root/'source.stl', root/'partition.npz', root/'rejected', reference_faces=5,
                    surface_fit=source, render=False, distance_options=Options(max_repairs=0, samples_per_face=128,
                                                                            seed=44500, preventive_repair=False))
            failure = json.loads((root/'rejected/distance_guard/run.json').read_text())
            self.assertEqual(failure['status'], 'failed')
            self.assertFalse((root/'rejected/regions').exists())
            self.assertFalse((root/'rejected/step').exists())
            result = reconstruct(root/'source.stl', root/'partition.npz', root/'rejected', reference_faces=5,
                surface_fit=source, render=False, distance_options=options, resume=True)
            guard = result['distance_guard']; history = guard['history']
            self.assertEqual(guard['repairs'], 1)
            self.assertEqual(guard['changed_charts'], [0])
            self.assertTrue(history[0]['repair']['continuity_preservation'])
            self.assertTrue(history[0]['cached_audit'])
            self.assertTrue(history[0]['over_budget_faces'])
            self.assertEqual(history[0]['sample_role'], 'tuning_after_local_repair')
            self.assertEqual(history[1]['seed'], history[0]['seed']+1)
            self.assertLess(result['stratified']['source_distance_mm']['max'], cfg.distance)
            final = Path(result['surface_fit'])
            self.assertEqual(curves, {p.name: p.read_bytes() for p in (final/'curves').glob('*.npz')})
            self.assertEqual(json.loads((final/'run.json').read_text())['config']['distance'], cfg.distance)
            validate_fit(final, root/'partition.npz', root/'source.stl')
            # A saved candidate must pass the physical and continuity gates again.
            first = root/'rejected/distance_guard/round_00'
            selected, _ = select_faces(history[0]['stratified'], history[0]['quality'],
                                       cfg.distance, options, preventive=True)
            repeated = repair(source, first/'assembled', first/'step/model.step', selected,
                              first/'repaired_fit', options, resume=True)
            self.assertEqual(repeated['status'], 'complete')
            self.assertTrue(all(e['trials'][0]['method'] == 'validated_checkpoint'
                                and e['trials'][0]['accepted'] for e in repeated['events']))
            # The explicit bounded-refit option retains its previous behavior.
            limited = repair(source, first/'assembled', first/'step/model.step', selected,
                root/'limited_repair', Options(max_repairs=1, repair_samples=1024, preserve_continuity=True,
                    projection_repair=False, max_repair_groups=2, repair_seconds=30.,
                    solver_seconds=2., fit_seconds=20., fit_iterations=20))
            self.assertEqual(limited['status'], 'complete')
            self.assertTrue(all(e['passed'] and e['trials'][-1]['method'] == 'refit_with_margin'
                                for e in limited['events']))
            from unittest.mock import patch
            with patch('tools.mesh_brep.skeleton_graph.fit_support', side_effect=AssertionError('No refitting')):
                short = repair(source, first/'assembled', first/'step/model.step', selected,
                    root/'short_repair', Options(max_repairs=1, repair_samples=1024, preserve_continuity=True,
                        projection_repair=False, max_repair_groups=2, repair_seconds=30., short_repair_seconds=2.))
            self.assertEqual(short['status'], 'complete')
            self.assertTrue(all(e['passed'] and e['trials'][-1]['method'] == 'single_coefficient_projection'
                                for e in short['events']))
            self.assertTrue(fit_checkpoint.matches(source, first/'input'))
            changed = dict(np.load(path)); changed['height'] = changed['height']+.001
            np.savez_compressed(path, **changed)
            self.assertFalse(fit_checkpoint.matches(source, first/'input'))
            with self.assertRaisesRegex(ValueError, 'checkpoint input changed'):
                repair(source, first/'assembled', first/'step/model.step', selected,
                       first/'repaired_fit', options, resume=True)
            # Actual STEP face ordering is not a stable chart identifier.
            assembly = Path(guard['final_round'])/'assembled'
            native = TopoDS_Shape(); builder = BRep_Builder()
            BRepTools.Read_s(native, str(assembly/'native.brep'), builder)
            compound = TopoDS_Compound(); builder.MakeCompound(compound)
            for face in cad.shapes(native, TopAbs_FACE)[::-1]:
                builder.Add(compound, face)
            records = {int(p.stem): dict(np.load(p)) for p in (final/'charts').glob('*.npz')}
            expected = json.loads((assembly/'run.json').read_text())['cad_face_charts'][::-1]
            self.assertEqual(match_faces(compound, assembly, records), expected)
            records[0]['height'] = records[0]['height']+.1
            with self.assertRaisesRegex(ValueError, 'does not match'):
                match_faces(compound, assembly, records)


if __name__ == '__main__':
    unittest.main()
