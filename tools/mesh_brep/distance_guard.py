"""Bounded local repairs within the shared-network fitting pipeline.

Generated STEP supplies diagnostic locations, never fitting targets. Failed
audits become tuning data; acceptance requires a new STEP audit after repair.
"""
from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import shutil
import numpy as np
from OCP.BRep import BRep_Builder, BRep_Tool
from OCP.BRepTools import BRepTools
from OCP.TopAbs import TopAbs_FACE, TopAbs_VERTEX, TopAbs_IN, TopAbs_ON
from OCP.TopoDS import TopoDS, TopoDS_Shape
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.gp import gp_Pnt2d
from scipy.spatial import cKDTree
from . import cad
from .mesh import Locator, dump
from .partition_fit import Config, load_partition, parameters, positions, surface_poles, spline_graph_error
from .network_support import samples
from .source_normals import crease_fan_normals
from .support_probe import probes
from .splines import tensor


@dataclass(frozen=True)
class Options:
    max_repairs: int = 3
    samples_per_face: int = 1024
    repair_samples: int = 32768
    seed: int = 810572
    repair_seed: int = 920101
    screening_fraction: float = .95
    fitting_fraction: float = .9

    def validate(self):
        if (self.max_repairs < 0 or self.samples_per_face < 1 or self.repair_samples < 1
                or min(self.seed, self.repair_seed) < 0
                or not 0 < self.fitting_fraction < self.screening_fraction < 1):
            raise ValueError('Invalid automatic distance-guard options')


def validate_fit(root, partition, stl):
    """Accept a completed network cache without regenerating its junctions."""
    root = Path(root)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    if meta.get('requires_support_refit'):
        raise ValueError('Changed curves still require incident-support refitting')
    v, f, labels, _ = load_partition(partition, stl)
    with np.load(meta['partition']) as old:
        for name, value in [('mesh_vertex_original', v), ('mesh_triangle', f), ('face_instance_id', labels)]:
            if not np.array_equal(old[name], value):
                raise ValueError('Cached fit belongs to a different source partition')
    paths = sorted((root/'charts').glob('*.npz'))
    if [int(p.stem) for p in paths] != list(range(len(paths))):
        raise ValueError('Chart IDs must be complete and contiguous')
    assignment = np.load(root/'chart_index.npz')['face_chart']
    if assignment.shape != labels.shape:
        raise ValueError('Invalid chart assignment')
    covered = np.zeros(len(f), dtype=np.int64)
    for di, path in enumerate(paths):
        rec = dict(np.load(path)); ids = rec['triangle_ids']
        if (not len(ids) or np.any(ids < 0) or np.any(ids >= len(f))
                or not np.all(labels[ids] == int(rec['region']))
                or not np.all(assignment[ids] == di)
                or not np.array_equal(np.unique(f[ids]), rec['vertex_ids'])):
            raise ValueError('Cached chart changed source ownership or labels')
        np.add.at(covered, ids, 1)
    if not np.all(covered == 1):
        raise ValueError('Cached charts have missing or duplicated source coverage')
    if not list((root/'curves').glob('*.npz')):
        raise ValueError('Missing canonical shared curves')
    return dict(meta, source_fit=str(root.resolve()), complete_source_coverage=True,
                partition_labels_unchanged=True)


def select_faces(dense, quality, limit, options, preventive=False):
    """Keep every face's witness, including faces outside a global top-k list."""
    rows = {int(r['face']): dict(r) for r in dense['per_face']}
    all_rows = list(rows.values())+quality.get('per_face_distance', [])
    if not rows or any(not np.isfinite(r['source_distance_mm']['max']) for r in all_rows):
        raise ValueError('Missing or non-finite per-face distance evidence')
    for row in quality.get('per_face_distance', []):
        fi = int(row['face'])
        if row['source_distance_mm']['max'] > rows[fi]['source_distance_mm']['max']:
            rows[fi] = dict(row)
    failed = [fi for fi, r in rows.items() if r['source_distance_mm']['max'] > limit]
    threshold = limit*options.screening_fraction
    selected = [r for r in rows.values() if r['source_distance_mm']['max'] > threshold] if failed or preventive else []
    return sorted(selected, key=lambda r: r['face']), sorted(failed)


def _poles(face):
    s = BRep_Tool.Surface_s(face)
    return np.array([[s.Pole(i, j).Coord() for j in range(1, s.NbVPoles()+1)]
                     for i in range(1, s.NbUPoles()+1)])


def match_faces(shape, assembly, records):
    """Match actual exchanged faces to native trims; do not assume face order."""
    assembly = Path(assembly)
    meta = json.loads((assembly/'run.json').read_text(encoding='utf-8'))
    original = TopoDS_Shape()
    BRepTools.Read_s(original, str(assembly/'native.brep'), BRep_Builder())
    before, after = [cad.shapes(s, TopAbs_FACE) for s in (original, shape)]
    chart_ids = meta['cad_face_charts']
    if len(after) != len(before) or sorted(chart_ids) != sorted(records):
        raise ValueError('STEP changed the one-to-one native face/chart structure')
    tolerance = 1e-8
    original_poles = [_poles(face) for face in before]
    for poles, di in zip(original_poles, chart_ids):
        expected = surface_poles(records[di])
        if poles.shape != expected.shape or not np.allclose(poles, expected, rtol=0, atol=tolerance):
            raise ValueError('Native assembly support does not match the fitting cache')
    def vertices(face):
        return np.array([BRep_Tool.Pnt_s(TopoDS.Vertex_s(v)).Coord() for v in cad.shapes(face, TopAbs_VERTEX)])
    original_vertices = [vertices(face) for face in before]
    centers = cKDTree([p.reshape(-1, 3).mean(axis=0) for p in original_poles])
    mapping = []; used = set()
    for face in after:
        poles = _poles(face); points = vertices(face)
        candidates = centers.query_ball_point(poles.reshape(-1, 3).mean(axis=0), tolerance)
        matched = []
        for i in candidates:
            p, q = original_poles[i], original_vertices[i]
            if i in used or p.shape != poles.shape or len(q) != len(points):
                continue
            if not np.allclose(p, poles, rtol=0, atol=tolerance):
                continue
            if (cKDTree(q).query(points)[0].max(initial=0) <= tolerance
                    and cKDTree(points).query(q)[0].max(initial=0) <= tolerance):
                # Equal poles alone do not determine a B-spline surface.
                a, b = BRep_Tool.Surface_s(before[i]), BRep_Tool.Surface_s(face)
                if (a.UDegree(), a.VDegree(), a.IsURational(), a.IsVRational()) != (
                        b.UDegree(), b.VDegree(), b.IsURational(), b.IsVRational()):
                    continue
                if a.IsURational() or a.IsVRational():
                    raise ValueError('Distance repair requires polynomial graph supports')
                if (a.NbUKnots(), a.NbVKnots()) != (b.NbUKnots(), b.NbVKnots()):
                    continue
                if any([getattr(a, k)(j) for j in range(1, count+1)] !=
                       [getattr(b, k)(j) for j in range(1, count+1)]
                       for k, count in [('UMultiplicity', a.NbUKnots()), ('VMultiplicity', a.NbVKnots())]):
                    continue
                if any(not np.allclose([getattr(a, k)(j) for j in range(1, count+1)],
                       [getattr(b, k)(j) for j in range(1, count+1)], rtol=0, atol=1e-12)
                       for k, count in [('UKnot', a.NbUKnots()), ('VKnot', a.NbVKnots())]):
                    continue
                matched.append(i)
        if len(matched) != 1:
            raise ValueError('Ambiguous or changed STEP support/trim identity')
        used.add(matched[0]); mapping.append(chart_ids[matched[0]])
    return mapping


def support_groups(records, selected):
    groups = []; used = set()
    for di in sorted(selected):
        if di in used:
            continue
        group = sorted(map(int, records[di].get('shared_support_group', [di])))
        if di not in group or not set(group).issubset(records):
            raise ValueError('Invalid shared-support membership')
        for other in group:
            if sorted(map(int, records[other].get('shared_support_group', [other]))) != group:
                raise ValueError('Inconsistent shared-support membership')
            if any(not np.array_equal(records[di][k], records[other][k])
                   for k in ('origin', 'axes', 'lo', 'size', 'knots', 'height')):
                raise ValueError('Shared-support members have different geometry')
        groups.append(group); used.update(group)
    return groups


def repair(source, assembly, step, selected, output, options, *, prior_fit=None, iteration=0):
    """Repair only measured outliers and their common-support members."""
    from .step_quality import tessellate, native
    from .skeleton_graph import fit_support
    from .boundary_checks import intersections
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8')); cfg = Config(**meta['config'])
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = [dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))]
    with np.load(meta['partition']) as data:
        v, f = data['mesh_vertex_original'], data['mesh_triangle']
    shape = cad.read_generated(step); faces = cad.shapes(shape, TopAbs_FACE)
    mapping = match_faces(shape, assembly, records); inverse = {di: fi for fi, di in enumerate(mapping)}
    mesh, surfaces, signs = tessellate(shape, .001)
    locator = Locator(v, f); normal = None
    report = dict(status='running', can_apply=False, options=asdict(options), events=[],
        original_distance_limit_mm=cfg.distance, probe_limit_mm=cfg.distance*options.screening_fraction,
        fitting_limit_mm=cfg.distance*options.fitting_fraction, support_boundary_limit_mm=cfg.boundary*.4,
        actual_step_face_charts=mapping, shared_curves_unchanged=True, source_CAD_used_as_target=False)
    report['refit_source_normals'] = 'Unfiltered crease-separated source fans'
    changes = {}; rows = {mapping[r['face']]: r for r in selected}
    try:
        for group in support_groups(records, rows):
            incident = [c for c in curves if set(group).intersection(map(int, c['owners']))]
            ids = np.unique(np.concatenate([records[di]['triangle_ids'] for di in group]))
            used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids]); original = records[group[0]]
            targets = np.r_[v[used], v[f[ids]].mean(axis=1)]; probes_world = []; seeds = []
            for di in group:
                fi = inverse[di]; ti = np.flatnonzero(mesh['owners'] == fi)
                xyz = mesh['points'][mesh['triangles'][ti]]
                area = np.linalg.norm(np.cross(xyz[:, 1]-xyz[:, 0], xyz[:, 2]-xyz[:, 0]), axis=1)/2
                seed = options.repair_seed+iteration*len(records)+di; seeds.append(seed)
                rng = np.random.default_rng(seed); choices = rng.choice(ti, options.repair_samples, p=area/area.sum())
                u = rng.random((len(choices), 2)); s = np.sqrt(u[:, 0]); b = np.c_[1-s, s*(1-u[:, 1]), s*u[:, 1]]
                q = np.einsum('ij,ijk->ik', b, mesh['uv'][mesh['triangles'][choices]])
                inside = np.array([BRepClass_FaceClassifier(faces[fi], gp_Pnt2d(*p), 1e-9).State()
                                   in (TopAbs_IN, TopAbs_ON) for p in q])
                xyz = native(surfaces[fi], q[inside], signs[fi])[0]
                if not len(xyz):
                    raise ValueError('No verified trimmed repair samples')
                if di in rows:
                    xyz = np.r_[xyz, [rows[di]['worst_distance']['xyz']]]
                if not np.allclose(positions(parameters(xyz, records[di]), records[di]), xyz, rtol=0, atol=1e-8):
                    raise ValueError('STEP witness does not lie on its matched cached support')
                probes_world.append(xyz)
            xyz = np.concatenate(probes_world)
            event = dict(charts=group, faces=[inverse[di] for di in group], trials=[],
                         tuning_seeds=seeds, tuning_samples=len(xyz), passed=False)
            report['events'].append(event); dump(out/'repair.json', report)

            def evaluate(candidate, method, **extra):
                projected = positions(parameters(xyz, candidate), candidate)
                distance = locator.closest(projected)[1]
                boundary = max(spline_graph_error(c['poles'], c['knots'], candidate) for c in incident)
                pq, pz = probes(candidate, v[used], tri)
                interior = float(abs(tensor(pq, candidate['knots'], candidate['knots'])@candidate['height']-pz).max(initial=0))
                q = parameters(targets, candidate); z = (targets-candidate['origin'])@candidate['axes'][2]
                training = float(abs(tensor(q, candidate['knots'], candidate['knots'])@candidate['height']-z).max(initial=0))
                trim_valid = all(not intersections(incident, [i for i, c in enumerate(incident) if di in c['owners']], candidate)
                                 for di in group)
                passed = (np.isfinite(distance).all() and distance.max() < cfg.distance*options.screening_fraction
                          and boundary < cfg.boundary*.4 and max(interior, training) < cfg.distance and trim_valid)
                trial = dict(method=method, source_probe_max_mm=float(distance.max()), continuous_boundary_mm=boundary,
                    interior_probe_max_mm=interior, training_max_mm=training, trim_valid=trim_valid, accepted=bool(passed), **extra)
                event['trials'].append(trial); dump(out/'repair.json', report)
                print(dict(stage='automatic_distance_repair', charts=group, **trial), flush=True)
                if passed:
                    candidate.update(continuous_boundary_error_mm=boundary, interior_probe_max_mm=interior,
                                     sampled_source_max_mm=training, distance_repair_method=method)
                return passed

            accepted = None
            # A coefficient blend is not known to preserve a hard normal cone.
            # The bounded solver rechecks that optional constraint on refit.
            if prior_fit is not None and cfg.normal_angle_limit is None:
                prior_paths = [Path(prior_fit)/'charts'/f'{di:05d}.npz' for di in group]
                if all(p.exists() for p in prior_paths):
                    previous = [dict(np.load(p)) for p in prior_paths]; old = previous[0]
                    compatible = all(all(np.array_equal(records[di][k], p[k]) for k in
                        ('origin', 'axes', 'lo', 'size', 'knots', 'triangle_ids', 'vertex_ids', 'region'))
                        for di, p in zip(group, previous))
                    compatible &= all(np.array_equal(p['height'], old['height']) for p in previous)
                    if compatible and not np.array_equal(original['height'], old['height']):
                        for alpha in (.875, .75, .5, .25, 0.):
                            candidate = dict(original, height=alpha*original['height']+(1-alpha)*old['height'])
                            if evaluate(candidate, 'backtrack', alpha=alpha):
                                accepted = candidate; break
            if accepted is None:
                if cfg.boundary_normal_weight:
                    raise ValueError('Automatic distance refit does not support coupled boundary-normal objectives')
                if normal is None:
                    normal = crease_fan_normals(v, f, cfg.feature_angle)
                _, candidate, info = fit_support((group[0], v[used], tri, normal[ids],
                    np.concatenate([samples(c) for c in incident]), incident,
                    replace(cfg, distance=cfg.distance*options.fitting_fraction), 'bounded'))
                dump(out/f'refit_{group[0]:05d}.json', info)
                if candidate is not None and evaluate(candidate, 'refit_with_margin'):
                    accepted = candidate
            if accepted is None:
                raise ValueError(f'No bounded local repair for charts {group}')
            for di in group:
                candidate = dict(accepted)
                # Retain per-chart ownership, never the representative's labels.
                candidate.update({k: records[di][k] for k in ('region', 'triangle_ids', 'vertex_ids')})
                if len(group) > 1:
                    candidate['shared_support_group'] = np.array(group)
                changes[di] = candidate
            event['passed'] = True; dump(out/'repair.json', report)
        for name in ('charts', 'curves', 'routes'):
            if (root/name).exists():
                shutil.copytree(root/name, out/name)
        for name in ('chart_index.npz', 'fitting.json'):
            shutil.copy2(root/name, out/name)
        for di, rec in changes.items():
            np.savez_compressed(out/'charts'/f'{di:05d}.npz', **rec)
        report.update(status='complete', changed_charts=sorted(changes))
        meta.update(source_fit=str(out.resolve()), automatic_distance_repair=report)
        dump(out/'run.json', meta)
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        dump(out/'repair.json', report)
    return report


def finalize(source, output, *, reference_faces, options=None, prior_fit=None, render=False):
    """Assemble, audit, repair locally and repeat, with a bounded retry count."""
    from .graph_reassemble import reconstruct as assemble
    from .graph_export import export
    from .step_quality import audit as area_audit
    from .step_stratified import audit as dense_audit
    options = options or Options(); options.validate()
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=False)
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    limit = float(meta['config']['distance'])
    if not np.isfinite(limit) or limit <= 0:
        raise ValueError('Invalid source-distance budget')
    report = dict(status='running', can_apply=False, options=asdict(options), history=[],
        original_distance_limit_mm=limit, source_CAD_used_as_target=False,
        scope='Finite actual STEP samples; not a Hausdorff, G1/G2 or visual-quality certificate.')
    try:
        for iteration in range(options.max_repairs+1):
            stage = out/f'round_{iteration:02d}'; stage.mkdir()
            row = dict(iteration=iteration, source_fit=str(root.resolve()), seed=options.seed+iteration,
                       status='running', sample_role='validation')
            report['history'].append(row); dump(out/'run.json', report)
            row['assembly'] = assemble(root, stage/'assembled', smooth_segments=True, write_preview=False)
            row['step'] = export(stage/'assembled', stage/'step')
            if not row['step']['watertight'] or row['step']['topology']['faces'] >= reference_faces:
                raise ValueError('STEP topology, tolerance or face-count gate failed')
            row['quality'] = area_audit(stage/'step/model.step', meta['partition'], stage/'quality')
            row['stratified'] = dense_audit(stage/'step/model.step', meta['partition'], stage/'stratified.json',
                                          samples_per_face=options.samples_per_face, seed=row['seed'])
            selected, failed = select_faces(row['stratified'], row['quality'], limit, options, preventive=iteration == 0)
            row.update(over_budget_faces=failed, selected_faces=[r['face'] for r in selected])
            if not selected:
                row['status'] = 'passed'
                report.update(status='DiagnosticOnly', sampled_distance_gate_passed=True,
                    source_fit=str(root.resolve()), final_round=str(stage.resolve()), repairs=iteration,
                    changed_charts=sorted({di for r in report['history'] for di in r.get('repair', {}).get('changed_charts', [])}))
                if render:
                    from .regional_quality import render as draw
                    draw(stage/'quality', None, '重建 STEP（原生曲面法向）')
                break
            row.update(status='repair_required', sample_role='tuning_after_local_repair')
            dump(out/'run.json', report)
            if iteration == options.max_repairs:
                raise ValueError('Automatic distance repair budget exhausted')
            row['repair'] = repair(root, stage/'assembled', stage/'step/model.step', selected,
                stage/'repaired_fit', options, prior_fit=prior_fit, iteration=iteration)
            root = stage/'repaired_fit'
            row['status'] = 'repaired_pending_new_audit'
            dump(out/'run.json', report)
        else:
            raise ValueError('No validated STEP candidate')
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                      sampled_distance_gate_passed=False, error=f'{type(error).__name__}: {error}')
        raise
    finally:
        dump(out/'run.json', report)
    return report
