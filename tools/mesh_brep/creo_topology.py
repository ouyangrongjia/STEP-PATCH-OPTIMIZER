"""Localize reconstructed faces in a real Creo STEP round trip."""
import argparse
import time
import numpy as np
from pathlib import Path
from .mesh import dump
from .evaluate import sample_cad
from geometry import read_step, shapes, compound, edge_uses, write_step, TopAbs_FACE, TopAbs_SOLID, BRep_Tool
from OCP.BRepBndLib import BRepBndLib
from OCP.Bnd import Bnd_Box
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.BRepTools import BRepTools
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf
from OCP.gp import gp_Pnt


def bounds(shape):
    b = Bnd_Box(); BRepBndLib.Add_s(shape, b, False)
    q = np.array(b.Get()); return q[:3], q[3:]


def run(before, after, out):
    start = time.perf_counter(); out = Path(out); out.mkdir(parents=True, exist_ok=False)
    original = read_step(before); whole = read_step(after)
    sources = shapes(original, TopAbs_FACE); faces = shapes(whole, TopAbs_FACE)
    lo, hi = bounds(original); found = []; records = []; counts = [0]*len(sources); screened = 0
    for i, face in enumerate(faces):
        a, b = bounds(face)
        if np.any(b < lo-.001) or np.any(a > hi+.001):
            continue
        screened += 1; points, _ = sample_cad(face, 9, 2910400+i); options = []
        for fi, source in enumerate(sources):
            surface = BRep_Tool.Surface_s(source); limits = BRepTools.UVBounds_s(source); distances = []
            for point in points:
                query = GeomAPI_ProjectPointOnSurf(gp_Pnt(*point), surface, *limits, 1e-10)
                distances.append(query.LowerDistance() if query.NbPoints() else float('inf'))
            options.append((max(distances), fi))
        distance, fi = min(options)
        if distance <= 3e-5:
            counts[fi] += 1; found.append(face)
            records.append(dict(creo_face=i, source_face=fi, maximum_support_distance_mm=distance))
    if not found or any(n == 0 for n in counts):
        raise ValueError('Could not identify every reconstructed support in Creo export')
    local = compound(found); write_step(out/'creo_local.step', local)
    uses = edge_uses(faces)
    incidences = [sum(2 if BRep_Tool.IsClosed_s(edge, faces[i]) else 1 for i in owners)
                  for edge, owners in uses if not BRep_Tool.Degenerated_s(edge)]
    report = dict(before_local_faces=len(sources), after_local_faces=len(found), per_source_face=counts,
                  after_whole_faces=len(faces), after_whole_solids=len(shapes(whole, TopAbs_SOLID)),
                  after_whole_free_edges=sum(n == 1 for n in incidences), after_whole_multiple_edges=sum(n > 2 for n in incidences),
                  after_whole_degenerate_edges=sum(BRep_Tool.Degenerated_s(edge) for edge, _ in uses),
                  local_brep_valid=BRepCheck_Analyzer(local, True).IsValid(), screened_faces=screened,
                  matching_samples_per_face=9, matching_tolerance_mm=3e-5, matches=records,
                  wall_seconds=time.perf_counter()-start,
                  scope='Actual Creo import/export B-Rep entities. Full-model OCCT validity is not re-certified here.')
    dump(out/'audit.json', report); print(report, flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    for name in ['before', 'after', 'output']:
        p.add_argument('--'+name, required=True)
    a = p.parse_args(); run(a.before, a.after, a.output)
