"""Read-only independent evaluation. Source CAD may be read only in this module."""
import argparse
import json
import sys
import time
from pathlib import Path
import numpy as np
from .mesh import read_stl, sample as sample_mesh, vertex_normals, Locator, stats, dump
from .splines import tensor, evaluate as eval_spline

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, shapes, TopAbs_FACE, BRep_Tool, TopAbs_REVERSED
from validation import distances as seeded_distances, sample as sample_cad, representation, continuity, angles
from diagnose import seam_quality
from spline import from_occt


def distances(points, shape, truth_normals=None):
    """Repair the retained Newton evaluator's physical-boundary minima.

    Clipped 2D Newton is not an edge-constrained minimizer. Query all physical
    boundary and nonstationary results, not just favorable/worst holdout points.
    The original seed-based report remains attached for traceability.
    """
    from OCP.BRepTools import BRepTools
    from OCP.BRepExtrema import BRepExtrema_DistShapeShape
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
    from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf
    from OCP.gp import gp_Pnt
    from geometry import evaluate as occt_evaluate
    report, arrays = seeded_distances(points, shape, truth_normals)
    faces = shapes(shape, TopAbs_FACE); query_ids = []
    for fi, face in enumerate(faces):
        ids = np.flatnonzero(arrays['patch'] == fi)
        if not len(ids):
            continue
        uv = arrays['uv'][ids]; s = BRep_Tool.Surface_s(face)
        p, tu, tv, degree = from_occt(s)
        from spline import evaluate as value
        q = value(p, tu, tv, uv, degrees=degree)
        du, dv = value(p, tu, tv, uv, 1, 0, degree), value(p, tu, tv, uv, 0, 1, degree)
        stationarity = np.maximum(abs(np.einsum('ij,ij->i', q-points[ids], du))/np.maximum(np.linalg.norm(du, axis=1), 1e-30),
                                 abs(np.einsum('ij,ij->i', q-points[ids], dv))/np.maximum(np.linalg.norm(dv, axis=1), 1e-30))
        u0, u1, v0, v1 = BRepTools.UVBounds_s(face)
        boundary = (np.minimum(uv[:, 0]-u0, u1-uv[:, 0]) < 1e-8) | (np.minimum(uv[:, 1]-v0, v1-uv[:, 1]) < 1e-8)
        query_ids.extend(ids[boundary | (stationarity > 1e-8)])
    corrections = []; initial_report = {k: report[k] for k in ['distance_mm', 'kernel_agreement_pass', 'kernel_checks']}
    if 'normal_degrees' in report:
        initial_report['normal_degrees'] = report['normal_degrees']
    for i in sorted(set(query_ids)):
        query = BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*points[i])).Vertex(), shape)
        if not query.IsDone():
            raise RuntimeError('Boundary/stationarity CAD query failed')
        point = query.PointOnShape2(1); options = []
        previous = int(arrays['patch'][i])
        for fi in [previous]+[j for j in range(len(faces)) if j != previous]:
            face = faces[fi]; surface = BRep_Tool.Surface_s(face)
            project = GeomAPI_ProjectPointOnSurf(point, surface, *BRepTools.UVBounds_s(face), 1e-10)
            if project.NbPoints():
                options.append((project.LowerDistance(), fi, project.LowerDistanceParameters()))
                if fi == previous and project.LowerDistance() < 1e-8:
                    break
        if not options:
            raise RuntimeError('Cannot evaluate normal at kernel closest point')
        _, fi, uv = min(options, key=lambda x: x[0]); old = float(arrays['distance'][i])
        arrays['distance'][i] = query.Value(); arrays['uv'][i] = uv; arrays['patch'][i] = fi
        if truth_normals is not None:
            _, n = occt_evaluate(BRep_Tool.Surface_s(faces[fi]), np.asarray([uv]))
            n *= -1 if faces[fi].Orientation() == TopAbs_REVERSED else 1
            arrays['normal_angle'][i] = angles(n, truth_normals[i:i+1])[0]
        if abs(old-query.Value()) > 1e-9:
            corrections.append(dict(sample=int(i), before_mm=old, after_mm=query.Value()))
    updated = []
    for old in report['kernel_checks']:
        q = dict(old); q['numerical_mm'] = float(arrays['distance'][q['sample']]); q['difference_mm'] = abs(q['numerical_mm']-q['kernel_mm']); updated.append(q)
    report.update(distance_mm=stats(arrays['distance']), kernel_checks=updated,
                  kernel_agreement_pass=all(x['difference_mm'] <= 1e-6 for x in updated),
                  boundary_stationarity_audit=dict(queried=len(set(query_ids)), corrections=corrections, initial_report=initial_report))
    if truth_normals is not None:
        report['normal_degrees'] = stats(arrays['normal_angle'])
    if not report['kernel_agreement_pass']:
        raise RuntimeError('Distance verification remains inconsistent after boundary audit')
    return report, arrays


def regularity(shape):
    result = []
    for i, face in enumerate(shapes(shape, TopAbs_FACE)):
        p, tu, tv, degree = from_occt(BRep_Tool.Surface_s(face))
        if tuple(degree) != (3, 3):
            result.append(dict(face=i, status='non-bicubic')); continue
        uq = np.unique(np.r_[np.linspace(0, 1, 129), np.unique(tu)])
        vq = np.unique(np.r_[np.linspace(0, 1, 129), np.unique(tv)])
        uv = np.stack(np.meshgrid(uq, vq, indexing='ij'), axis=-1).reshape(-1, 2)
        a = tensor(uv, tu, tv, 1, 0)@p.reshape(-1, 3)
        b = tensor(uv, tu, tv, 0, 1)@p.reshape(-1, 3)
        singular = np.linalg.svd(np.stack([a, b], axis=2), compute_uv=False)
        jacobian = np.linalg.norm(np.cross(a, b), axis=1)
        nn = np.cross(a, b)/np.maximum(jacobian[:, None], 1e-30)
        e, f, g = np.sum(a*a, axis=1), np.sum(a*b, axis=1), np.sum(b*b, axis=1)
        l = np.sum((tensor(uv, tu, tv, 2, 0)@p.reshape(-1, 3))*nn, axis=1)
        m = np.sum((tensor(uv, tu, tv, 1, 1)@p.reshape(-1, 3))*nn, axis=1)
        n = np.sum((tensor(uv, tu, tv, 0, 2)@p.reshape(-1, 3))*nn, axis=1)
        mean = (e*n-2*f*m+g*l)/(2*np.maximum(e*g-f*f, 1e-30))
        result.append(dict(face=i, samples=len(uv), jacobian_mm2=stats(jacobian),
                           singular_samples=int(np.sum(jacobian < 1e-10)),
                           tangent_condition=stats(singular[:, 0]/np.maximum(singular[:, 1], 1e-30)),
                           absolute_mean_curvature_per_mm=stats(abs(mean))))
    return dict(faces=result, continuous_self_intersection_proof=False,
                note='Dense derivative checks and kernel BRepCheck do not prove continuous injectivity.')


def evaluate_mesh(stl, candidate, out, count, seed, reverse_count, source=None):
    out = Path(out)
    if out.exists():
        raise FileExistsError(out)
    started = time.perf_counter(); timings = {}
    vertices, triangles = read_stl(stl); shape = read_step(candidate)
    normals = vertex_normals(vertices, triangles)
    p, ns, _, chosen, bary = sample_mesh(vertices, triangles, count, seed, normals=normals)
    q, qn = sample_cad(shape, reverse_count, seed+1)
    timings['read_and_sample'] = time.perf_counter()-started
    tick = time.perf_counter(); forward, farrays = distances(p, shape, ns)
    timings['mesh_to_surface'] = time.perf_counter()-tick
    tick = time.perf_counter(); locator = Locator(vertices, triangles)
    closest, d, ti, bc = locator.closest(q)
    nn = np.einsum('ij,ijk->ik', bc, normals[triangles[ti]])
    nn /= np.maximum(np.linalg.norm(nn, axis=1)[:, None], 1e-30)
    reverse = dict(distance_mm=stats(d), normal_degrees=stats(angles(qn, nn)), sample_count=len(q))
    facet = np.cross(vertices[triangles[ti, 1]]-vertices[triangles[ti, 0]], vertices[triangles[ti, 2]]-vertices[triangles[ti, 0]])
    facet /= np.linalg.norm(facet, axis=1)[:, None]
    reverse['facet_normal_degrees'] = stats(angles(qn, facet))
    timings['surface_to_mesh'] = time.perf_counter()-tick
    tick = time.perf_counter(); seams = seam_quality(shape); inside = continuity(shape); regular = regularity(shape)
    timings['seams_and_differentials'] = time.perf_counter()-tick
    result = dict(mesh=str(Path(stl).resolve()), candidate=str(Path(candidate).resolve()),
                  seed=seed, count=count, reverse_count=reverse_count, mesh_to_surface=forward, surface_to_mesh=reverse,
                  internal_seams=seams, interior_continuity=inside, regularity=regular,
                  expression=representation(shape), step_bytes=Path(candidate).stat().st_size,
                  normal_source='Area-weighted mesh vertex normals; reverse facet normals separately reported',
                  max_is_continuous_bound=False, timing_seconds=timings)
    arrays = dict(mesh_points=p, mesh_normal=ns, distance=farrays['distance'], normal_angle=farrays['normal_angle'],
                  reverse_points=q, reverse_distance=d)
    if source:
        tick = time.perf_counter(); reference = read_step(source)
        cp, cn = sample_cad(reference, count, seed)
        cad_forward, ca = distances(cp, shape, cn)
        cad_reverse, _ = distances(q, reference)
        _, mesh_error, _, _ = locator.closest(cp)
        mesh_to_cad, _ = distances(p, reference)
        result.update(step_to_surface=cad_forward, surface_to_step=cad_reverse,
                      step_to_mesh=dict(distance_mm=stats(mesh_error), sample_count=len(cp)), mesh_to_step=mesh_to_cad)
        arrays.update(cad_points=cp, cad_normal=cn, cad_distance=ca['distance'], cad_normal_angle=ca['normal_angle'])
        timings['independent_source_cad'] = time.perf_counter()-tick
    result['wall_seconds'] = time.perf_counter()-started
    dump(out, result); np.savez_compressed(out.with_suffix('.npz'), **arrays)
    print(json.dumps(dict(candidate=str(candidate), forward_mm=forward['distance_mm']['max'],
                          reverse_mm=reverse['distance_mm']['max'], normal_deg=forward['normal_degrees']['max'],
                          seconds=result['wall_seconds'])), flush=True)
    return result


def main():
    p = argparse.ArgumentParser(); p.add_argument('--stl', required=True); p.add_argument('--candidate', required=True)
    p.add_argument('--output', required=True); p.add_argument('--count', type=int, default=9000)
    p.add_argument('--reverse-count', type=int, default=6000); p.add_argument('--seed', type=int, default=2909180)
    p.add_argument('--source', help='Evaluation-only original CAD; never consumed by reconstruction')
    a = p.parse_args(); evaluate_mesh(a.stl, a.candidate, a.output, a.count, a.seed, a.reverse_count, a.source)


if __name__ == '__main__':
    main()
