"""Bounded native-trim preflight and one input-neighborhood rollback.

CAD validity alone does not expose every thin-trim exchange failure. A failed
native face triggers one conservative restoration of its source neighborhood,
without subdivision or another surface solve. All shared trims remain unique.
"""
import json
from pathlib import Path
import shutil
import time
import numpy as np
from .mesh import dump
from .partition_fit import spline_graph_error


def missing_triangulations(shape, deflection=.005):
    from OCP.BRep import BRep_Tool
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.BRepTools import BRepTools
    from OCP.IMeshTools import IMeshTools_Parameters, IMeshTools_MeshAlgoType_Delabella
    from OCP.TopAbs import TopAbs_FACE
    from OCP.TopLoc import TopLoc_Location
    from .cad import shapes
    BRepMesh_IncrementalMesh(shape, deflection, False, .2, True)
    faces = shapes(shape, TopAbs_FACE)
    missing = [i for i, f in enumerate(faces) if BRep_Tool.Triangulation_s(f, TopLoc_Location()) is None]
    for i in missing:
        BRepTools.Clean_s(faces[i])
        p = IMeshTools_Parameters(); p.Deflection = deflection; p.Angle = .2
        p.InParallel = False; p.MeshAlgo = IMeshTools_MeshAlgoType_Delabella
        BRepMesh_IncrementalMesh(faces[i], p)
    return [i for i in missing if BRep_Tool.Triangulation_s(faces[i], TopLoc_Location()) is None]


def restore_neighborhood(source, fit, failed_charts, certificates, evidence):
    from .network_support import validate_wires
    from .boundary_quality import measures
    tick = time.perf_counter(); root, out = Path(source), Path(fit)
    evidence = Path(evidence); evidence.mkdir(parents=True, exist_ok=False)
    records, curves = certificates['records'], certificates['curves']
    failed = set(map(int, failed_charts))
    # Exactly one adjacency ring, selected only from this run's failed faces.
    selected = failed | {int(di) for c in curves.values() if failed.intersection(map(int, c['owners'])) for di in c['owners']}
    changed_curves = {ci for ci, c in curves.items() if selected.intersection(map(int, c['owners']))}
    for name, ids, store in (('charts', selected, records), ('curves', changed_curves, curves)):
        (evidence/name).mkdir()
        for index in sorted(ids):
            name_part = f'{index:05d}.npz'
            shutil.copy2(out/name/name_part, evidence/name/name_part)
            shutil.copy2(root/name/name_part, out/name/name_part)
            store[index] = dict(np.load(out/name/name_part))
    meta = json.loads((out/'run.json').read_text(encoding='utf-8'))
    errors = certificates['errors']
    affected = {ci for ci, c in curves.items() if selected.intersection(map(int, c['owners']))} | changed_curves
    for ci in affected:
        c = curves[ci]
        errors[ci] = [spline_graph_error(c['poles'], c['knots'], records[int(di)]) for di in c['owners']]
        if max(errors[ci]) > meta['config']['boundary']*.85:
            raise ValueError('Restored neighborhood exceeds the existing edge budget')
    with np.load(meta['partition']) as data:
        vertices, triangles = data['mesh_vertex_original'], data['mesh_triangle']
    check_charts = selected | {int(di) for ci in changed_curves for di in curves[ci]['owners']}
    failures = validate_wires(vertices, triangles, records, curves, check_charts, native_frame=True)
    if failures:
        dump(evidence/'wire_failures.json', failures)
        raise ValueError('One bounded restoration did not resolve native trim compatibility')
    report = dict(trigger='native_face_tessellation_failed_in_both_kernels', failed_charts=sorted(failed),
        restored_charts=sorted(selected), restored_curves=sorted(changed_curves), rounds=1,
        surface_resolves=0, new_faces=0, source_segmented_charts=sorted(selected),
        seconds=time.perf_counter()-tick)
    dump(evidence/'run.json', report)
    boundary_path = out/'boundary_refinement.json'
    boundary = json.loads(boundary_path.read_text(encoding='utf-8'))
    for ci in changed_curves:
        row = boundary['per_curve'][str(ci)]
        row.update(accepted=False, reason='native_trim_guard', support_error_mm=errors[ci])
    active = [int(ci) for ci, row in boundary['per_curve'].items() if row['accepted']]
    boundary.update(curves_changed=len(active),
        regions_changed=sorted({int(records[int(di)]['region']) for ci in active for di in curves[ci]['owners']}),
        native_piece_curvature_after=sum(measures(c)['curvature_squared_integral_per_mm'] for c in curves.values()),
        native_trim_guard=report)
    dump(boundary_path, boundary)
    return report
