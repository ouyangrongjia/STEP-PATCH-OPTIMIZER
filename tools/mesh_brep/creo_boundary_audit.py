"""Localize free boundaries in a diagnostic export from actual Creo import.

The exported geometry is diagnostic evidence only, never reconstruction input.
"""
import argparse
from collections import Counter
from pathlib import Path
import time
import numpy as np
from OCP.BRep import BRep_Tool
from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.TopoDS import TopoDS
from . import cad
from .mesh import Locator, dump
from .partition_cad import topology


def audit(step, partition, output, chart_index=None):
    out = Path(output); out.mkdir(parents=True, exist_ok=False); started = time.perf_counter()
    shape = cad.read_generated(step); topo = topology(shape)
    source = np.load(partition); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    labels = source['face_instance_id']; locator = Locator(v, f)
    assigned = np.load(chart_index)['face_chart'] if chart_index else None
    indexed = TopTools_IndexedMapOfShape(); owners = []
    for fi, face in enumerate(cad.shapes(shape, TopAbs_FACE)):
        for edge in cad.shapes(face, TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge):
                continue
            index = indexed.Add(edge)-1
            if index == len(owners):
                owners.append([])
            owners[index].extend([fi]*(2 if BRep_Tool.IsClosed_s(edge, face) else 1))
    records, points = [], []
    for index, uses in enumerate(owners):
        if len(uses) == 2:
            continue
        edge = TopoDS.Edge_s(indexed.FindKey(index+1)); a, b = BRep_Tool.Range_s(edge)
        curve = BRep_Tool.Curve_s(edge, a, b)
        if curve is None:
            continue
        p = np.array([curve.Value(float(t)).Coord() for t in np.linspace(a, b, 9)])
        _, distances, ids, _ = locator.closest(p)
        record = dict(edge=index, owners=uses, center=p[4].tolist(),
            length_mm=float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum()),
            tolerance_mm=BRep_Tool.Tolerance_s(edge), source_distance_max_mm=float(distances.max()),
            nearby_region_votes=dict(Counter(map(int, labels[ids]))))
        if assigned is not None:
            record['nearby_chart_votes'] = dict(Counter(map(int, assigned[ids])))
        records.append(record); points.extend(p)
    report = dict(topology=topo, abnormal_edges=records, seconds=time.perf_counter()-started,
        scope='Actual Creo import/export topology. Nearest source votes are spatial diagnostics, not CAD ID correspondence. Export is not a reconstruction input.')
    dump(out/'run.json', report); np.save(out/'boundary_points.npy', np.asarray(points))
    print(topo, flush=True)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--step', required=True); p.add_argument('--partition', required=True)
    p.add_argument('--output', required=True); p.add_argument('--chart-index')
    a = p.parse_args(); audit(a.step, a.partition, a.output, a.chart_index)
