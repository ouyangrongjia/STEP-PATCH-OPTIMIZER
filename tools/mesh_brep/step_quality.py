"""Independent trimmed STEP audit against the supplied STL partition.

The CAD input is the newly generated result. Sampling uses its actual surfaces,
trimmed tessellation and face orientation; display normals are native derivatives.
Nearest source labels are diagnostic votes, not a replacement segmentation.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import time

import numpy as np
from OCP.BRep import BRep_Tool
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.TopAbs import TopAbs_FACE, TopAbs_EDGE, TopAbs_REVERSED
from OCP.TopLoc import TopLoc_Location
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS
from OCP.gp import gp_Pnt, gp_Vec

from . import cad
from .mesh import Locator, dump, stats, vertex_normals
from .partition_cad import topology
from .regional_quality import weighted_stats


class UntriangulatedFaces(ValueError):
    def __init__(self, faces):
        self.faces = list(faces)
        super().__init__(f'Faces have no triangulation: {self.faces}')


def native(surface, uv, sign=1):
    xyz = np.empty((len(uv), 3)); du = xyz.copy(); dv = xyz.copy()
    point, u, v = gp_Pnt(), gp_Vec(), gp_Vec()
    for i, (a, b) in enumerate(uv):
        surface.D1(float(a), float(b), point, u, v)
        xyz[i], du[i], dv[i] = point.Coord(), u.Coord(), v.Coord()
    cross = np.cross(du, dv); jac = np.linalg.norm(cross, axis=1)
    return xyz, sign*cross/np.maximum(jac[:, None], 1e-30), du, dv


def tessellate(shape, deflection=.005, mesh_algorithm='default'):
    if mesh_algorithm == 'default':
        BRepMesh_IncrementalMesh(shape, deflection, False, .2, True)
    else:
        from OCP.IMeshTools import IMeshTools_Parameters, IMeshTools_MeshAlgoType_Delabella, IMeshTools_MeshAlgoType_Watson
        if mesh_algorithm not in ('delabella', 'watson'):
            raise ValueError('Unknown STEP display meshing algorithm')
        p = IMeshTools_Parameters(); p.Deflection = deflection; p.Angle = .2; p.InParallel = True
        p.MeshAlgo = IMeshTools_MeshAlgoType_Delabella if mesh_algorithm == 'delabella' else IMeshTools_MeshAlgoType_Watson
        BRepMesh_IncrementalMesh(shape, p)
    faces = cad.shapes(shape, TopAbs_FACE)
    missing = [i for i, face in enumerate(faces) if BRep_Tool.Triangulation_s(face, TopLoc_Location()) is None]
    retried = []
    if missing and mesh_algorithm == 'default':
        from OCP.BRepTools import BRepTools
        from OCP.IMeshTools import IMeshTools_Parameters, IMeshTools_MeshAlgoType_Delabella
        for fi in missing:
            # A valid narrow trim can fail Watson while Delabella succeeds at
            # the same deflection. Keep the STEP unchanged and disclose the
            # retry; never omit an unmeshed face from geometric evaluation.
            BRepTools.Clean_s(faces[fi])
            p = IMeshTools_Parameters(); p.Deflection = deflection; p.Angle = .2; p.InParallel = False
            p.MeshAlgo = IMeshTools_MeshAlgoType_Delabella
            BRepMesh_IncrementalMesh(faces[fi], p)
            retried.append(fi)
        missing = [i for i in missing if BRep_Tool.Triangulation_s(faces[i], TopLoc_Location()) is None]
    if missing:
        raise UntriangulatedFaces(missing)
    points = []; triangles = []; uv = []; normals = []; owners = []; surfaces = []; signs = []
    offset = 0; deviations = []
    for fi, face in enumerate(faces):
        loc = TopLoc_Location(); mesh = BRep_Tool.Triangulation_s(face, loc)
        if mesh is None: raise ValueError(f'Face {fi} has no triangulation')
        xyz = np.array([mesh.Node(i).Transformed(loc.Transformation()).Coord() for i in range(1, mesh.NbNodes()+1)])
        q = np.array([mesh.UVNode(i).Coord() for i in range(1, mesh.NbNodes()+1)])
        tri = np.array([mesh.Triangle(i).Get() for i in range(1, mesh.NbTriangles()+1)], dtype=np.int64)-1
        sign = -1 if face.Orientation() == TopAbs_REVERSED else 1
        if sign < 0: tri = tri[:, [0, 2, 1]]
        surface = BRep_Tool.Surface_s(face)
        evaluated, nn, _, _ = native(surface, q, sign)
        # Sewing may place boundary tessellation nodes on a 3D edge whose
        # tolerance hides a support gap. Display/evaluate the actual support.
        deviations.append(float(np.max(np.linalg.norm(xyz-evaluated, axis=1))))
        points.append(evaluated); uv.append(q); normals.append(nn); triangles.append(tri+offset)
        owners.extend([fi]*len(tri)); surfaces.append(surface); signs.append(sign); offset += len(xyz)
    return dict(points=np.concatenate(points), triangles=np.concatenate(triangles),
                normals=np.concatenate(normals), uv=np.concatenate(uv), owners=np.array(owners),
                delabella_retried_faces=np.array(retried, dtype=np.int64),
                tessellation_to_native_surface_max_mm=np.array(deviations)), surfaces, signs


def edge_audit(shape, surfaces, signs, source_locator, source_labels):
    index = TopTools_IndexedMapOfShape(); uses = []
    faces = cad.shapes(shape, TopAbs_FACE)
    for fi, face in enumerate(faces):
        for edge in cad.shapes(face, TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge): continue
            j = index.Add(edge)-1
            if j == len(uses): uses.append([])
            uses[j].append(fi)
    angles = []; gaps = []; records = []; weights = []
    for j, owners in enumerate(uses):
        edge = TopoDS.Edge_s(index.FindKey(j+1)); a, b = BRep_Tool.Range_s(edge)
        t = np.linspace(a, b, 19); curve = BRep_Tool.Curve_s(edge, a, b)
        if curve is None: continue
        xyz = np.array([curve.Value(float(x)).Coord() for x in t])
        length = float(np.linalg.norm(np.diff(xyz, axis=0), axis=1).sum())
        record = dict(edge=j, owners=owners, length_mm=length, center=xyz[len(xyz)//2].tolist(),
                      tolerance_mm=BRep_Tool.Tolerance_s(edge))
        if len(owners) == 2:
            samples = []; normal = []
            for fi in owners:
                pc = BRep_Tool.CurveOnSurface_s(edge, faces[fi], a, b)
                if pc is None: raise ValueError('Missing edge pcurve')
                q = np.array([pc.Value(float(x)).Coord() for x in t[1:-1]])
                p, n, _, _ = native(surfaces[fi], q, signs[fi]); samples.append(p); normal.append(n)
            gap = np.linalg.norm(samples[0]-samples[1], axis=1)
            degree = np.degrees(np.arccos(np.clip(np.sum(normal[0]*normal[1], axis=1), -1, 1)))
            angles.extend(degree); gaps.extend(gap); weights.extend([length/len(degree)]*len(degree))
            record.update(max_surface_gap_mm=float(gap.max()), normal_degrees=stats(degree))
        records.append(record)
    return dict(records=records, all_join_normal_degrees=stats(angles),
                length_weighted_all_join_normal_degrees=weighted_stats(angles, weights) if angles else None,
                surface_gap_mm=stats(gaps))


def _audit_impl(step, partition, output, deflection=.005, count=30000, render=False, camera=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    report = dict(status='running', step=str(Path(step).resolve()), partition=str(Path(partition).resolve()),
                  units='mm', source_CAD_read=False, source_label_assignment='Nearest STL votes, diagnostic only')
    dump(out/'run.json', report)
    if Path(step).suffix.lower() == '.brep':
        from OCP.BRep import BRep_Builder
        from OCP.BRepTools import BRepTools
        from OCP.TopoDS import TopoDS_Shape
        shape = TopoDS_Shape(); BRepTools.Read_s(shape, str(step), BRep_Builder())
    else: shape = cad.read_generated(step)
    report['topology'] = topology(shape)
    display, surfaces, signs = tessellate(shape, deflection)
    report['delabella_retried_faces'] = display['delabella_retried_faces'].tolist()
    spline_surfaces = [s for s in surfaces if hasattr(s, 'NbUPoles')]
    report['representation'] = dict(file_bytes=Path(step).stat().st_size,
        spline_faces=len(spline_surfaces), actual_control_points=sum(s.NbUPoles()*s.NbVPoles() for s in spline_surfaces),
        maximum_direction_poles=max((max(s.NbUPoles(),s.NbVPoles()) for s in spline_surfaces),default=0))
    report['tessellation_to_native_surface_max_mm'] = float(display['tessellation_to_native_surface_max_mm'].max())
    np.savez_compressed(out/'display_mesh.npz', **display)
    data = np.load(partition); v, f, labels = data['mesh_vertex_original'], data['mesh_triangle'], data['face_instance_id']
    original = Locator(v, f); loc = Locator(display['points'], display['triangles'])
    from .mesh import sample
    x, _, _, _, _ = sample(v, f, count, 930601)
    projected, distance, ids, bary = loc.closest(x)
    # A tessellation distance includes its separately stated chordal tolerance.
    report['forward_tessellation_distance_mm'] = stats(distance)
    vertices = display['points'][display['triangles']]
    areas = np.linalg.norm(np.cross(vertices[:, 1]-vertices[:, 0], vertices[:, 2]-vertices[:, 0]), axis=1)/2
    rng = np.random.default_rng(930602); cumulative = np.cumsum(areas)
    choices = rng.choice(len(areas), size=max(count, 100*len(surfaces)), p=areas/areas.sum())
    random = rng.random((len(choices), 2)); root = np.sqrt(random[:, 0])
    bary = np.c_[1-root, root*(1-random[:, 1]), root*random[:, 1]]
    q = np.einsum('ij,ijk->ik', bary, display['uv'][display['triangles'][choices]])
    owners = display['owners'][choices]; fitted = np.empty((len(q), 3)); normals = fitted.copy()
    for fi in np.unique(owners):
        selected = owners == fi
        fitted[selected], normals[selected], _, _ = native(surfaces[fi], q[selected], signs[fi])
    _, reverse, nearest, nearest_bary = original.closest(fitted)
    source_normals = vertex_normals(v, f)
    sn = np.einsum('ij,ijk->ik', nearest_bary, source_normals[f[nearest]])
    sn /= np.maximum(np.linalg.norm(sn, axis=1)[:, None], 1e-30)
    dot = np.sum(sn*normals, axis=1); angles = np.degrees(np.arccos(np.clip(dot, -1, 1)))
    report.update(reverse_native_surface_distance_mm=stats(reverse), source_normal_difference_degrees=stats(angles),
                  opposed_normal_sample_fraction=float(np.mean(dot < 0)), tessellation_area_mm2=float(areas.sum()),
                  tessellation_triangles=len(areas), tessellation_deflection_mm=deflection,
                  source_samples=count, reverse_samples=len(reverse))
    votes = []; majority = []
    for fi in range(len(surfaces)):
        selected = owners == fi
        if not np.any(selected):
            # Explicitly retain unsampled tiny faces instead of inventing labels.
            votes.append(dict(face=fi, samples=0)); majority.append(-1); continue
        unique, counts = np.unique(labels[nearest[selected]], return_counts=True)
        order = np.argsort(-counts); region = int(unique[order[0]]); majority.append(region)
        votes.append(dict(face=fi, samples=int(selected.sum()), majority_region=region,
                          majority_fraction=float(counts[order[0]]/counts.sum()),
                          votes={str(int(k)): int(n) for k, n in zip(unique[order], counts[order])}))
    dump(out/'source_region_votes.json', dict(faces=votes, majority_regions=sorted(set(majority))))
    joins = edge_audit(shape, surfaces, signs, original, labels); dump(out/'edges.json', joins)
    report.update(all_join_normal_degrees=joins['all_join_normal_degrees'],
                  length_weighted_all_join_normal_degrees=joins['length_weighted_all_join_normal_degrees'],
                  surface_gap_mm=joins['surface_gap_mm'], seconds=time.perf_counter()-started,
                  status='DiagnosticOnly', can_apply=False,
                  scope='Independent finite samples, not a certified Hausdorff or self-intersection bound. Join angles include designed creases.')
    np.savez_compressed(out/'quality_samples.npz', forward=distance, reverse=reverse, normal_degrees=angles)
    dump(out/'run.json', report); print(json.dumps(report), flush=True)
    if render:
        from .regional_quality import render as draw
        draw(out, camera, '重建 STEP（原生曲面法向）')
    return report


def audit(step, partition, output, deflection=.005, count=30000, render=False, camera=None):
    existed = Path(output).exists(); started = time.perf_counter()
    try:
        return _audit_impl(step, partition, output, deflection, count, render, camera)
    except Exception as error:
        record = Path(output)/'run.json'
        if not existed and record.is_file():
            report = json.loads(record.read_text(encoding='utf-8'))
            report.update(status='failed', can_apply=False, error=f'{type(error).__name__}: {error}',
                          seconds=time.perf_counter()-started)
            if isinstance(error, UntriangulatedFaces):
                report['untriangulated_faces'] = error.faces
            dump(record, report)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--step', required=True)
    parser.add_argument('--partition', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--deflection', type=float, default=.005); parser.add_argument('--render', action='store_true')
    parser.add_argument('--camera'); args = parser.parse_args()
    audit(args.step, args.partition, args.output, args.deflection, render=args.render, camera=args.camera)
