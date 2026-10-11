"""Bounded local repairs within the shared-network fitting pipeline.

Generated STEP supplies diagnostic locations, never fitting targets. Failed
audits become tuning data; acceptance requires a new STEP audit after repair.
"""
from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import shutil
import time
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
    preserve_continuity: bool = False
    preventive_repair: bool = True
    projection_repair: bool = True
    max_repair_groups: int | None = None
    repair_seconds: float | None = None
    fit_seconds: float | None = None
    solver_seconds: float = 12.
    fit_iterations: int = 32
    short_repair_seconds: float | None = None

    def validate(self):
        if (self.max_repairs < 0 or self.samples_per_face < 1 or self.repair_samples < 1
                or min(self.seed, self.repair_seed) < 0
                or not 0 < self.fitting_fraction < self.screening_fraction < 1):
            raise ValueError('Invalid automatic distance-guard options')
        if (self.max_repair_groups is not None and self.max_repair_groups < 1
                or self.fit_iterations < 1 or not np.isfinite(self.solver_seconds) or self.solver_seconds <= 0
                or any(value is not None and (not np.isfinite(value) or value <= 0)
                       for value in (self.repair_seconds, self.fit_seconds, self.short_repair_seconds))):
            raise ValueError('Invalid local repair compute budget')


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
    if options.short_repair_seconds is not None and not preventive:
        selected = [r for r in rows.values() if r['source_distance_mm']['max'] > limit]
    else:
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


def repair(source, assembly, step, selected, output, options, *, prior_fit=None, iteration=0, resume=False):
    """Repair only measured outliers and their common-support members."""
    from .step_quality import tessellate, native
    from .skeleton_graph import fit_support
    from .boundary_checks import intersections
    from . import fit_checkpoint
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=resume)
    previous = None
    prior_report=None
    if (out/'repair.json').exists():
        prior_report=json.loads((out/'repair.json').read_text(encoding='utf-8'))
        shutil.copy2(out/'repair.json', out/f'repair_before_resume_{len(list(out.glob("repair_before_resume_*.json"))):02d}.json')
    if (out/'input').exists():
        if not fit_checkpoint.matches(root, out/'input'):
            raise ValueError('Distance-repair checkpoint input changed')
        previous=prior_report
    else:
        fit_checkpoint.save(root, out/'input')
    for name in ('probes', 'checkpoints', 'trials'):
        (out/name).mkdir(exist_ok=True)
    attempt = len(list(out.glob('repair_before_resume_*.json')))
    meta = json.loads((root/'run.json').read_text(encoding='utf-8')); cfg = Config(**meta['config'])
    records = {int(p.stem): dict(np.load(p)) for p in (root/'charts').glob('*.npz')}
    curves = [dict(np.load(p)) for p in sorted((root/'curves').glob('*.npz'))]
    with np.load(meta['partition']) as data:
        v, f = data['mesh_vertex_original'], data['mesh_triangle']
    shape = cad.read_generated(step); faces = cad.shapes(shape, TopAbs_FACE)
    mapping = match_faces(shape, assembly, records); inverse = {di: fi for fi, di in enumerate(mapping)}
    mesh, surfaces, signs = tessellate(shape, .001)
    locator = Locator(v, f); normal = None
    source_face_normals = None
    if options.short_repair_seconds is not None:
        source_face_normals = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
        source_face_normals /= np.linalg.norm(source_face_normals, axis=1)[:, None]
    report = dict(status='running', can_apply=False, options=asdict(options), events=[],
        original_distance_limit_mm=cfg.distance, probe_limit_mm=cfg.distance*options.screening_fraction,
        fitting_limit_mm=cfg.distance*options.fitting_fraction, support_boundary_limit_mm=cfg.boundary*.4,
        actual_step_face_charts=mapping, shared_curves_unchanged=True, source_CAD_used_as_target=False)
    report['refit_source_normals'] = 'Unfiltered crease-separated source fans'
    lookup = None
    if options.preserve_continuity:
        from .shared_support import source_angles
        lookup = source_angles(v, f)
    report['continuity_preservation'] = options.preserve_continuity
    changes = {}; rows = {mapping[r['face']]: r for r in selected}
    try:
        groups = support_groups(records, rows)
        if options.max_repair_groups is not None and len(groups) > options.max_repair_groups:
            raise ValueError('Local distance repair group budget exhausted')
        repair_started = time.perf_counter()
        for group in groups:
            if options.repair_seconds is not None and time.perf_counter()-repair_started >= options.repair_seconds:
                raise ValueError('Local distance repair time budget exhausted')
            incident = [c for c in curves if set(group).intersection(map(int, c['owners']))]
            ids = np.unique(np.concatenate([records[di]['triangle_ids'] for di in group]))
            used = np.unique(f[ids]); tri = np.searchsorted(used, f[ids]); original = records[group[0]]
            targets = np.r_[v[used], v[f[ids]].mean(axis=1)]; probes_world = []; seeds = []
            probe_path = out/'probes'/f'{group[0]:05d}.npz'
            cached_probe = dict(np.load(probe_path)) if probe_path.exists() else None
            expected_seeds = [options.repair_seed+iteration*len(records)+di for di in group]
            if cached_probe is not None and (not np.array_equal(cached_probe['charts'],group)
                    or not np.array_equal(cached_probe['seeds'],expected_seeds)
                    or int(cached_probe['requested_samples']) != options.repair_samples):
                raise ValueError('Distance-repair probe checkpoint has different ownership or sampling')
            for di in ([] if cached_probe is not None else group):
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
            if cached_probe is None:
                xyz = np.concatenate(probes_world)
                np.savez_compressed(probe_path, xyz=xyz, charts=group, seeds=seeds,
                                    requested_samples=options.repair_samples)
            else:
                xyz = cached_probe['xyz']; seeds = expected_seeds
                witnesses = [rows[di]['worst_distance']['xyz'] for di in group if di in rows]
                if witnesses:
                    xyz = np.r_[xyz, witnesses]
                if not np.allclose(positions(parameters(xyz, original), original), xyz, rtol=0, atol=1e-8):
                    raise ValueError('Cached repair probes no longer lie on their input support')
            event = dict(charts=group, faces=[inverse[di] for di in group], trials=[],
                         tuning_seeds=seeds, tuning_samples=len(xyz), passed=False)
            report['events'].append(event); dump(out/'repair.json', report)

            def evaluate(candidate, method, **extra):
                np.savez_compressed(out/'trials'/f'{group[0]:05d}_{attempt:02d}_{len(event["trials"]):02d}.npz', **candidate)
                projected = positions(parameters(xyz, candidate), candidate)
                distance = locator.closest(projected)[1]
                boundary = max(spline_graph_error(c['poles'], c['knots'], candidate) for c in incident)
                pq, pz = probes(candidate, v[used], tri)
                interior = float(abs(tensor(pq, candidate['knots'], candidate['knots'])@candidate['height']-pz).max(initial=0))
                q = parameters(targets, candidate); z = (targets-candidate['origin'])@candidate['axes'][2]
                training = float(abs(tensor(q, candidate['knots'], candidate['knots'])@candidate['height']-z).max(initial=0))
                if options.short_repair_seconds is not None:
                    interior = float(locator.closest(positions(pq, candidate))[1].max(initial=0.))
                    training = float(locator.closest(positions(q, candidate))[1].max(initial=0.))
                trim_valid = all(not intersections(incident, [i for i, c in enumerate(incident) if di in c['owners']], candidate)
                                 for di in group)
                screening_limit = cfg.distance if options.short_repair_seconds is not None else cfg.distance*options.screening_fraction
                passed = (np.isfinite(distance).all() and distance.max() < screening_limit
                          and boundary < cfg.boundary*.4 and max(interior, training) < cfg.distance and trim_valid)
                continuity = None
                if passed and options.preserve_continuity:
                    from .distance_projection import continuity_change
                    continuity = continuity_change(group, candidate, records, {**records, **changes}, incident, lookup)
                    passed = continuity['passed']
                trial = dict(method=method, source_probe_max_mm=float(distance.max()), continuous_boundary_mm=boundary,
                    interior_probe_max_mm=interior, training_max_mm=training, trim_valid=trim_valid,
                    continuity=continuity, accepted=bool(passed), **extra)
                event['trials'].append(trial); dump(out/'repair.json', report)
                print(dict(stage='automatic_distance_repair', charts=group, **trial), flush=True)
                if passed:
                    candidate.update(continuous_boundary_error_mm=boundary, interior_probe_max_mm=interior,
                                     sampled_source_max_mm=training, distance_repair_method=extra.get('original_method',method))
                return passed

            accepted = None
            saved_event = next((e for e in (previous or {}).get('events',[]) if e['charts']==group and e['passed']),None)
            if saved_event is not None and all((out/'checkpoints'/f'{di:05d}.npz').exists() for di in group):
                cached = [dict(np.load(out/'checkpoints'/f'{di:05d}.npz')) for di in group]
                if any(any(not np.array_equal(r[k],cached[0][k]) for k in ('origin','axes','lo','size','knots','height')) for r in cached):
                    raise ValueError('Cached distance repair split a shared support')
                trial = next(t for t in reversed(saved_event['trials']) if t['accepted'])
                method = trial.get('original_method',trial['method'])
                if evaluate(cached[0], 'validated_checkpoint', original_method=method):
                    accepted = cached[0]
            if accepted is None and options.short_repair_seconds is not None and cfg.normal_angle_limit is None:
                from .distance_projection import project
                candidate, info = project(original, v[used], tri, incident, xyz, cfg, options.fitting_fraction,
                                          seconds=options.short_repair_seconds, short=True,
                                          source_locator=locator, source_normals=source_face_normals)
                dump(out/f'short_projection_{group[0]:05d}.json', info)
                if candidate is not None and evaluate(candidate, 'single_coefficient_projection', projection=info):
                    accepted = candidate
                if accepted is None:
                    # Short mode never escalates a tiny miss into basis growth,
                    # a restarted fit or repeated local subdivision.
                    raise ValueError(f'Short local correction exhausted for charts {group}')
            if accepted is None and options.preserve_continuity and options.projection_repair and cfg.normal_angle_limit is None:
                from .distance_projection import project, refine
                candidate, info = project(original, v[used], tri, incident, xyz, cfg, options.fitting_fraction)
                dump(out/f'projection_{group[0]:05d}.json', info)
                if candidate is not None and evaluate(candidate, 'preserve_support_jets', projection=info):
                    accepted = candidate
                if accepted is None:
                    refined = refine(original, max(cfg.controls))
                    if refined is not None:
                        candidate, info = project(refined, v[used], tri, incident, xyz, cfg, options.fitting_fraction)
                        info.update(original_controls=len(original['knots'])-4, refined_controls=len(refined['knots'])-4,
                                    geometry_preserved_before_projection=True, unchanged_frame_and_basis=False)
                        dump(out/f'projection_refined_{group[0]:05d}.json', info)
                        if candidate is not None and evaluate(candidate, 'refined_support_jets', projection=info):
                            accepted = candidate
            # A coefficient blend is not known to preserve a hard normal cone.
            # The bounded solver rechecks that optional constraint on refit.
            if accepted is None and prior_fit is not None and cfg.normal_angle_limit is None:
                prior_paths = [Path(prior_fit)/'charts'/f'{di:05d}.npz' for di in group]
                if all(p.exists() for p in prior_paths):
                    prior_records = [dict(np.load(p)) for p in prior_paths]; old = prior_records[0]
                    compatible = all(all(np.array_equal(records[di][k], p[k]) for k in
                        ('origin', 'axes', 'lo', 'size', 'knots', 'triangle_ids', 'vertex_ids', 'region'))
                        for di, p in zip(group, prior_records))
                    compatible &= all(np.array_equal(p['height'], old['height']) for p in prior_records)
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
                    replace(cfg, distance=cfg.distance*options.fitting_fraction), 'bounded', None,
                    original if options.preserve_continuity else None, options.preserve_continuity),
                    max_iterations=options.fit_iterations, solver_seconds=options.solver_seconds,
                    seconds=options.fit_seconds)
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
                np.savez_compressed(out/'checkpoints'/f'{di:05d}.npz', **candidate)
            event['passed'] = True; dump(out/'repair.json', report)
        for name in ('charts', 'curves', 'routes'):
            if (root/name).exists():
                shutil.copytree(root/name, out/name, dirs_exist_ok=resume)
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


def finalize(source, output, *, reference_faces, options=None, prior_fit=None, render=False, resume=False):
    """Assemble, audit, repair locally and repeat, with a bounded retry count."""
    from .graph_reassemble import reconstruct as assemble
    from .graph_export import export
    from .step_quality import audit as area_audit
    from .step_stratified import audit as dense_audit
    from . import fit_checkpoint
    options = options or Options(); options.validate()
    root, out = Path(source), Path(output); out.mkdir(parents=True, exist_ok=resume)
    if (out/'run.json').exists():
        shutil.copy2(out/'run.json',out/f'run_before_resume_{len(list(out.glob("run_before_resume_*.json"))):02d}.json')
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    limit = float(meta['config']['distance'])
    if not np.isfinite(limit) or limit <= 0:
        raise ValueError('Invalid source-distance budget')
    report = dict(status='running', can_apply=False, options=asdict(options), history=[],
        original_distance_limit_mm=limit, source_CAD_used_as_target=False,
        scope='Finite actual STEP samples; not a Hausdorff, G1/G2 or visual-quality certificate.')
    try:
        for iteration in range(options.max_repairs+1):
            stage = out/f'round_{iteration:02d}'; stage.mkdir(exist_ok=resume)
            if (stage/'input').exists():
                if not fit_checkpoint.matches(root,stage/'input'):
                    raise ValueError('Cached STEP audit input changed')
            else:
                if any(stage.iterdir()):
                    raise ValueError('Old STEP audit has no verifiable input checkpoint')
                fit_checkpoint.save(root,stage/'input')
            row = dict(iteration=iteration, source_fit=str(root.resolve()), seed=options.seed+iteration,
                       status='running', sample_role='validation')
            report['history'].append(row); dump(out/'run.json', report)
            def completed(name):
                path=stage/name/'run.json'
                value=json.loads(path.read_text(encoding='utf-8')) if path.exists() else None
                return value if value is not None and value.get('status')=='DiagnosticOnly' else None

            def retain_incomplete(name):
                path=stage/name
                if path.exists():
                    target=stage/f'{name}_before_resume_{len(list(stage.glob(name+"_before_resume_*"))):02d}'
                    if path.resolve().parent!=stage.resolve() or target.resolve().parent!=stage.resolve():
                        raise ValueError('Invalid incomplete-stage archive path')
                    path.rename(target)

            row['assembly'] = completed('assembled')
            if row['assembly'] is None:
                retain_incomplete('assembled')
                row['assembly'] = assemble(root, stage/'assembled', smooth_segments=True, write_preview=False)
            row['step'] = completed('step')
            if row['step'] is None:
                retain_incomplete('step')
                row['step'] = export(stage/'assembled', stage/'step')
            if not row['step']['watertight'] or row['step']['topology']['faces'] >= reference_faces:
                raise ValueError('STEP topology, tolerance or face-count gate failed')
            row['quality'] = completed('quality')
            if row['quality'] is None:
                retain_incomplete('quality')
                row['quality'] = area_audit(stage/'step/model.step', meta['partition'], stage/'quality')
            dense_path=stage/'stratified.json'
            if dense_path.exists():
                row['stratified']=json.loads(dense_path.read_text(encoding='utf-8'))
                if (row['stratified']['seed']!=row['seed']
                        or row['stratified']['requested_samples_per_face']!=options.samples_per_face):
                    raise ValueError('Cached STEP audit uses different sampling')
                row['cached_audit']=True
            else:
                row['stratified'] = dense_audit(stage/'step/model.step', meta['partition'], dense_path,
                                              samples_per_face=options.samples_per_face, seed=row['seed'])
            selected, failed = select_faces(row['stratified'], row['quality'], limit, options,
                                            preventive=options.preventive_repair and iteration == 0)
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
                stage/'repaired_fit', options, prior_fit=prior_fit, iteration=iteration,
                resume=resume and (stage/'repaired_fit').exists())
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
