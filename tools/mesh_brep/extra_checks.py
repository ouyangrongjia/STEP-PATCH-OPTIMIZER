"""Independent CAD boundary, shared-edge and intersection diagnostics."""
import argparse
import json
import sys
import time
from pathlib import Path
import numpy as np
from .mesh import read_stl, stats, dump, audit
from .splines import evaluate as spline_eval
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import *
from integrate import integrate, edge_checks
from designer_audit import original_boundaries
from OCP.BRepAlgoAPI import BRepAlgoAPI_Section
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
from OCP.BRepGProp import BRepGProp
from OCP.GProp import GProp_GProps


def pair_sections(path):
    started = time.perf_counter(); shape = read_step(path); faces = shapes(shape, TopAbs_FACE); reports = []
    for i in range(len(faces)):
        for j in range(i):
            common = [e for e in shapes(faces[i], TopAbs_EDGE) if any(e.IsSame(b) for b in shapes(faces[j], TopAbs_EDGE))]
            op = BRepAlgoAPI_Section(faces[i], faces[j], False); op.Approximation(True); op.Build()
            if not op.IsDone():
                reports.append(dict(faces=[j, i], status='kernel_section_failed')); continue
            section = op.Shape(); edges = shapes(section, TopAbs_EDGE); worst = 0.; off_boundary = 0
            if common:
                boundary = compound(common)
                for e in edges:
                    _, points = edge_samples(e, 33)
                    for p in points:
                        query = BRepExtrema_DistShapeShape(BRepBuilderAPI_MakeVertex(gp_Pnt(*p)).Vertex(), boundary)
                        if not query.IsDone():
                            raise RuntimeError('Section classification failed')
                        worst = max(worst, query.Value())
                        off_boundary += int(query.Value() > .001)
            else:
                off_boundary = len(edges)
            reports.append(dict(faces=[j, i], status='checked', section_edges=len(edges), shared_edges=len(common),
                                maximum_section_distance_from_shared_edge_mm=worst, unexpected_samples_or_edges=off_boundary))
    return dict(pairs=reports, seconds=time.perf_counter()-started,
                note='Pairwise OCCT intersections checked against actual shared edges; does not certify same-face global injectivity.')


def boundary_mesh_error(stl, metadata, candidate):
    from .mesh import side_uv, chain_parameters, edges
    from .splines import evaluate
    from spline import from_occt
    vertices, tri = read_stl(stl); layout = json.loads(Path(metadata).read_text(encoding='utf-8'))
    e, counts, _ = edges(tri); outer = {tuple(x) for x in e[counts == 1]}; shape = read_step(candidate)
    # Reconstruction face order can change on sewing, so use unsewn saved pole arrays.
    directory = Path(candidate).parent; records = []
    def segment_distance(points, chain):
        a, b = chain[:-1], chain[1:]; e = b-a
        v = points[:, None]-a; t = np.clip(np.einsum('nki,ki->nk', v, e)/np.sum(e*e, axis=1), 0, 1)
        return np.linalg.norm(v-t[:, :, None]*e, axis=2).min(axis=1)
    for fi, patch in enumerate(layout['patches']):
        data = np.load(directory/f'patch_{fi}.npz')
        for side, chain in enumerate(patch['chains']):
            if tuple(sorted(chain[:2])) not in outer:
                continue
            q = np.linspace(0, 1, 1001); ct = chain_parameters(vertices, chain)
            p, _, _ = evaluate(data['poles'], data['tu'], data['tv'], side_uv(side, q))
            reference = np.stack([np.interp(q, ct, vertices[chain, k]) for k in range(3)], axis=1)
            records.append(dict(face=fi, side=side, paired_parameter_mm=stats(np.linalg.norm(p-reference, axis=1)),
                                curve_to_polyline_mm=stats(segment_distance(p, vertices[chain]))))
    return dict(outer_boundaries=records, note='Original boundary approximation, independent of zero shared-spline C0 gap; paired parameter residual upper-bounds sampled closest distance.')


def a_interfaces(run):
    run = Path(run); baseline = run/'baseline_cold'; protocol = json.loads((baseline/'protocol.json').read_text(encoding='utf-8'))
    names = ['inverse_xyz', 'mesh_uv_xyz']; variants = {name: read_step(run/'A'/name/'network.step') for name in names}
    mapping = json.loads((baseline/'aligned_inner_cubic/fit.json').read_text(encoding='utf-8'))['boundary_mapping']
    input_audit = json.loads((baseline/'input_audit.json').read_text(encoding='utf-8')); start = time.perf_counter()
    source = read_step(input_audit['source']); selected = set(input_audit['selected_face_ids'])
    # The same immutable whole-part object serves interface and integration
    # checks; avoid a second 98 MB transfer into the global STEP reader state.
    import designer_audit
    old_reader = designer_audit.read_step
    designer_audit.read_step = lambda path: source if Path(path).resolve() == Path(input_audit['source']).resolve() else old_reader(path)
    try:
        audit = original_boundaries(baseline, protocol, variants, {name: {'boundary_mapping': mapping} for name in names})
    finally:
        designer_audit.read_step = old_reader
    dump(run/'A/original_interfaces.json', audit)
    remaining = [f for i, f in enumerate(shapes(source, TopAbs_FACE)) if i not in selected]
    results = {}
    for name in names:
        shape, report = integrate(variants[name], remaining)
        destination = run/'A'/name/'whole_model_diagnostic.step'
        write_step(destination, shape); rt = read_step(destination)
        report.update(after_step_read=topology(rt))
        results[name] = report
    dump(run/'A/integration.json', dict(candidates=results, wall_seconds=time.perf_counter()-start))


def main():
    p = argparse.ArgumentParser(); p.add_argument('--candidate'); p.add_argument('--output'); p.add_argument('--a-run')
    p.add_argument('--stl'); p.add_argument('--layout'); a = p.parse_args()
    if a.a_run:
        a_interfaces(a.a_run); return
    result = pair_sections(a.candidate)
    if a.stl:
        result['outer_boundary'] = boundary_mesh_error(a.stl, a.layout, a.candidate)
        v, t = read_stl(a.stl)
        result['mesh_signed_volume_mm3'] = (float(np.einsum('ij,ij->i', v[t[:, 0]], np.cross(v[t[:, 1]], v[t[:, 2]])).sum()/6)
                                            if audit(v, t)['boundary_edges'] == 0 else None)
        shape = read_step(a.candidate)
        if shapes(shape, TopAbs_SOLID):
            prop = GProp_GProps(); BRepGProp.VolumeProperties_s(shape, prop); result['solid_volume_mm3'] = prop.Mass()
    dump(a.output, result)


if __name__ == '__main__':
    main()
