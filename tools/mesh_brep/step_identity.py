"""Verify generated STEP support identity before selecting mesh-only repairs."""
import json
from pathlib import Path
import numpy as np
from scipy.spatial import cKDTree
from OCP.BRep import BRep_Tool
from .partition_fit import surface_poles


def match_supports(faces, failed_faces, assembly, target_fit, tolerance=1e-8):
    """Use complete poles and knot vectors, never STEP face ordering alone.

    Ambiguous or changed supports remain unmatched for spatial diagnosis.
    Output geometry identifies source triangles; it is not a fitting target.
    """
    assembly, target_fit = Path(assembly), Path(target_fit)
    meta = json.loads((assembly/'run.json').read_text(encoding='utf-8'))
    target = json.loads((target_fit/'run.json').read_text(encoding='utf-8'))
    for field in ('stl', 'partition'):
        if Path(meta[field]).resolve() != Path(target[field]).resolve():
            raise ValueError('Generated support identity belongs to another source mesh or partition')
    records = [dict(np.load(p)) for p in sorted((Path(meta['source_fit'])/'charts').glob('*.npz'))]
    if meta.get('boundary_smoothing_budget_mm', 0):
        from .joint_graph_boundary import extend_support
        for rec in records:
            extend_support(rec, meta['boundary_smoothing_budget_mm']*1.1)
    expected = [surface_poles(rec) for rec in records]
    def descriptor(poles):
        return np.r_[poles[0, 0], poles[-1, -1], poles.mean(axis=(0, 1))]
    tree = cKDTree(np.array([descriptor(p) for p in expected]))
    assignment = np.load(target_fit/'chart_index.npz')['face_chart']
    selected, missing, verified = set(), [], []
    for fi in failed_faces:
        surface = BRep_Tool.Surface_s(faces[fi]); matches = []
        if (hasattr(surface, 'NbUPoles') and surface.UDegree() == 3 and surface.VDegree() == 3
                and not surface.IsURational() and not surface.IsVRational()):
            actual = np.array([[surface.Pole(i, j).Coord() for j in range(1, surface.NbVPoles()+1)]
                               for i in range(1, surface.NbUPoles()+1)])
            ku = np.repeat([surface.UKnot(i) for i in range(1, surface.NbUKnots()+1)],
                           [surface.UMultiplicity(i) for i in range(1, surface.NbUKnots()+1)])
            kv = np.repeat([surface.VKnot(i) for i in range(1, surface.NbVKnots()+1)],
                           [surface.VMultiplicity(i) for i in range(1, surface.NbVKnots()+1)])
            for chart in tree.query_ball_point(descriptor(actual), 3*tolerance):
                poles, knots = expected[chart], records[chart]['knots']
                if (poles.shape == actual.shape and ku.shape == knots.shape and kv.shape == knots.shape
                        and np.allclose(actual, poles, rtol=0, atol=tolerance)
                        and np.allclose(ku, knots, rtol=0, atol=1e-12)
                        and np.allclose(kv, knots, rtol=0, atol=1e-12)):
                    matches.append(chart)
        if len(matches) != 1:
            missing.append(int(fi)); continue
        chart = matches[0]
        current = list(map(int, np.unique(assignment[records[chart]['triangle_ids']])))
        selected.update(current)
        verified.append(dict(face=int(fi), source_chart=int(chart), current_charts=current))
    report = dict(method='verified_generated_supports_with_mesh_fallback',
        assembly=str(assembly.resolve()), support_coordinate_tolerance_mm=tolerance,
        verified=verified, fallback_faces=missing, all_supports_verified=not missing)
    return selected, missing, report
