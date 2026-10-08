"""Localize an existing independent audit without replacing its samples."""
import argparse
import json
from pathlib import Path
import numpy as np
from OCP.BRep import BRep_Tool
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.TopAbs import TopAbs_FACE
from OCP.gp import gp_Pnt2d
from . import cad
from .mesh import Locator, dump
from .step_quality import native


def track_source_points(step, assembly, fit, points, output):
    """Recheck fixed source triangle/barycentric points after chart changes.

    Verified generated support identity and actual trim membership are both
    required. Correspondence distance is not a nearest-distance statistic.
    """
    from OCP.TopAbs import TopAbs_REVERSED, TopAbs_IN, TopAbs_ON
    from .partition_fit import parameters
    from .step_identity import match_supports
    if Path(output).exists():
        raise FileExistsError(output)
    root = Path(fit); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(meta['partition']); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    index = np.load(root/'chart_index.npz')['face_chart']
    faces = cad.shapes(cad.read_generated(step), TopAbs_FACE)
    _, _, identity = match_supports(faces, range(len(faces)), assembly, fit)
    mapping = {}
    for row in identity['verified']:
        for chart in row['current_charts']:
            mapping.setdefault(chart, []).append(row['face'])
    assembly_meta = json.loads((Path(assembly)/'run.json').read_text(encoding='utf-8'))
    if Path(assembly_meta['source_fit']).resolve() != root.resolve():
        raise ValueError('Fixed-point validation requires the actual exported fit cache')
    records = []
    for entry in points:
        triangle = int(entry['source_triangle']); weights = np.asarray(entry['source_barycentric'], dtype=float)
        if (not 0 <= triangle < len(f) or weights.shape != (3,) or not np.isfinite(weights).all()
                or np.any(weights < 0) or abs(weights.sum()-1) > 1e-10):
            raise ValueError('Invalid source triangle or barycentric coordinates')
        chart = int(index[triangle]); candidates = mapping.get(chart, [])
        row = dict(source_triangle=triangle, source_barycentric=weights.tolist(), chart=chart,
                   actual_step_faces=candidates, support_verified=len(candidates) == 1)
        if len(candidates) == 1:
            rec = dict(np.load(root/'charts'/f'{chart:05d}.npz'))
            if assembly_meta.get('boundary_smoothing_budget_mm', 0):
                from .joint_graph_boundary import extend_support
                extend_support(rec, assembly_meta['boundary_smoothing_budget_mm']*1.1)
            target = weights@v[f[triangle]]; uv = parameters(target[None], rec)[0]
            face = faces[candidates[0]]
            sign = -1 if face.Orientation() == TopAbs_REVERSED else 1
            xyz, normal, _, _ = native(BRep_Tool.Surface_s(face), uv[None], sign)
            state = BRepClass_FaceClassifier(face, gp_Pnt2d(*uv), 1e-9).State()
            a, b, c = v[f[triangle]]; nn = np.cross(b-a, c-a); nn /= np.linalg.norm(nn)
            row.update(source_position=target.tolist(), actual_position=xyz[0].tolist(), uv=uv.tolist(),
                trim_classification=str(state), inside_trim=state in (TopAbs_IN, TopAbs_ON),
                correspondence_distance_mm=float(np.linalg.norm(xyz[0]-target)),
                source_face_normal_angle_degrees=float(np.degrees(np.arccos(np.clip(normal[0]@nn, -1, 1)))))
        records.append(row)
    result = dict(status='DiagnosticOnly', step=str(Path(step).resolve()), source_fit=str(root.resolve()),
        source_partition=meta['partition'], records=records,
        scope='Frozen source locations on actual STEP supports; failed or ambiguous identity is unverified. '
              'These correspondence measurements do not replace global distance or smoothness diagnostics.')
    dump(output, result); return result


def localize_forward(quality, output, maximum=20):
    """Recheck the unchanged worst forward samples against native face geometry.

    A compound of faces avoids the zero inside-solid distance convention. The
    original tessellation distances remain intact, including adverse values.
    """
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
    from OCP.BRepExtrema import BRepExtrema_DistShapeShape
    from OCP.TopoDS import TopoDS_Compound
    from OCP.gp import gp_Pnt
    from .mesh import sample
    if Path(output).exists():
        raise FileExistsError(output)
    root = Path(quality); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(meta['partition']); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    points, _, _, triangle_ids, _ = sample(v, f, meta['source_samples'], 930601)
    saved = np.load(root/'quality_samples.npz')['forward']
    indices = np.argsort(saved)[-maximum:][::-1]
    display = np.load(root/'display_mesh.npz')
    nearest, distance, _, _ = Locator(display['points'], display['triangles']).closest(points[indices])
    np.testing.assert_allclose(distance, saved[indices], rtol=1e-7, atol=1e-9)
    shape = cad.read_generated(meta['step']); builder = BRep_Builder(); faces = TopoDS_Compound()
    builder.MakeCompound(faces)
    for face in cad.shapes(shape, TopAbs_FACE):
        builder.Add(faces, face)
    records = []
    for index, point, d in zip(indices, points[indices], distance):
        solver = BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*point)).Vertex(), faces)
        row = dict(sample=int(index), source_position=point.tolist(), tessellation_distance_mm=float(d),
                   source_triangle=int(triangle_ids[index]),
                   source_region=int(source['face_instance_id'][triangle_ids[index]]),
                   native_solver_done=bool(solver.IsDone()))
        if solver.IsDone():
            row.update(native_face_distance_mm=solver.Value(),
                       nearest_native_position=list(solver.PointOnShape2(1).Coord()))
        records.append(row)
        dump(output, dict(source_audit=str(root.resolve()), unchanged_sampling=True,
            status='running', scope='Worst original forward samples only; not a replacement full-sample statistic.', records=records))
    dump(output, dict(source_audit=str(root.resolve()), unchanged_sampling=True, status='completed',
        scope='Worst original forward samples only; not a replacement full-sample statistic.', records=records))
    return records


def localize(quality, output, maximum=80):
    root = Path(quality); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    display = np.load(root/'display_mesh.npz'); samples = np.load(root/'quality_samples.npz')
    source = np.load(meta['partition']); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    labels = source['face_instance_id']; tri = display['points'][display['triangles']]
    areas = np.linalg.norm(np.cross(tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0]), axis=1)/2
    rng = np.random.default_rng(930602)
    choices = rng.choice(len(areas), size=meta['reverse_samples'], p=areas/areas.sum())
    random = rng.random((len(choices), 2)); w = np.sqrt(random[:, 0])
    bary = np.c_[1-w, w*(1-random[:, 1]), w*random[:, 1]]
    ids = np.argsort(samples['reverse'])[-maximum:][::-1]
    q = np.einsum('ij,ijk->ik', bary[ids], display['uv'][display['triangles'][choices[ids]]])
    owners = display['owners'][choices[ids]]; shape = cad.read_generated(meta['step'])
    faces = cad.shapes(shape, TopAbs_FACE); xyz = []; states = []
    for face_id, uv in zip(owners, q):
        face = faces[int(face_id)]
        xyz.append(native(BRep_Tool.Surface_s(face), uv[None])[0][0])
        states.append(str(BRepClass_FaceClassifier(face, gp_Pnt2d(*uv), 1e-9).State()))
    nearest, distance, triangle, weights = Locator(v, f).closest(np.array(xyz))
    np.testing.assert_allclose(distance, samples['reverse'][ids], rtol=1e-7, atol=1e-9)
    records = [dict(sample=int(i), face=int(face), uv=uv.tolist(), position=p.tolist(), distance_mm=float(d),
                    trim_classification=state, nearest_source_triangle=int(t), nearest_region=int(labels[t]))
               for i, face, uv, p, d, state, t in zip(ids, owners, q, xyz, distance, states, triangle)]
    dump(output, dict(source_audit=str(root.resolve()), unchanged_sampling=True, records=records))
    print(records[:6], flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--quality', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); localize(a.quality, a.output)
