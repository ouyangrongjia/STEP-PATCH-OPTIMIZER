"""Preserve disjoint bodies and nested cavities of an oriented closed mesh."""
import numpy as np
from scipy import sparse


def winding(point, v, f):
    angle = 0.
    for start in range(0, len(f), 50000):
        p = v[f[start:start+50000]]-point
        lengths = np.linalg.norm(p, axis=2)
        denominator = np.prod(lengths, axis=1)
        denominator += np.einsum('ij,ij->i', p[:, 0], p[:, 1])*lengths[:, 2]
        denominator += np.einsum('ij,ij->i', p[:, 1], p[:, 2])*lengths[:, 0]
        denominator += np.einsum('ij,ij->i', p[:, 2], p[:, 0])*lengths[:, 1]
        numerator = np.einsum('ij,ij->i', p[:, 0], np.cross(p[:, 1], p[:, 2]))
        angle += np.sum(2*np.arctan2(numerator, denominator))
    return float(angle/(4*np.pi))


def layout(v, f, edge_owners):
    graph = sparse.coo_matrix((np.ones(edge_owners.size), (edge_owners.ravel(), edge_owners[:, ::-1].ravel())),
                              shape=(len(f), len(f))).tocsr()
    count, component = sparse.csgraph.connected_components(graph, directed=False)
    records = []; origin = v.mean(axis=0)
    for i in range(count):
        ids = np.flatnonzero(component == i); used = np.unique(f[ids]); p = v[f[ids]]-origin
        volume = float(np.einsum('ij,ij->i', p[:, 0], np.cross(p[:, 1], p[:, 2])).sum()/6)
        if abs(volume) < 1e-15:
            raise ValueError('A closed input shell has effectively zero signed volume')
        records.append(dict(component=i, triangles=ids, vertices=used, signed_volume_mm3=volume,
                            lo=v[used].min(axis=0), hi=v[used].max(axis=0), parent=-1))
    for i, inner in enumerate(records):
        containers = []
        for j, outer in enumerate(records):
            if i == j or np.any(outer['lo'] > inner['lo']) or np.any(outer['hi'] < inner['hi']):
                continue
            probes = inner['vertices'][np.unique([0, len(inner['vertices'])//2, len(inner['vertices'])-1])]
            inside = [abs(winding(v[k], v, f[outer['triangles']])) > .5 for k in probes]
            if any(inside) and not all(inside):
                raise ValueError('Input shells intersect or have ambiguous containment')
            if all(inside):
                containers.append(j)
        if containers:
            inner['parent'] = min(containers, key=lambda j: abs(records[j]['signed_volume_mm3']))
    for i, record in enumerate(records):
        seen = {i}; parent = record['parent']; depth = 0
        while parent >= 0:
            if parent in seen:
                raise ValueError('Cyclic source-shell containment')
            seen.add(parent); depth += 1; parent = records[parent]['parent']
        record['depth'] = depth
        record['reverse'] = bool((record['signed_volume_mm3'] > 0) != (depth % 2 == 0))
    return component, records


def assemble(shells, records):
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeSolid
    from OCP.BRepLib import BRepLib
    from OCP.TopoDS import TopoDS, TopoDS_Compound
    oriented = [TopoDS.Shell_s(s.Reversed()) if r['reverse'] else s for s, r in zip(shells, records)]
    solids = []
    for i, rec in enumerate(records):
        if rec['depth'] % 2:
            continue
        maker = BRepBuilderAPI_MakeSolid(oriented[i])
        for j, child in enumerate(records):
            if child['parent'] == i:
                maker.Add(oriented[j])
        solid = maker.Solid()
        if not BRepLib.OrientClosedSolid_s(solid):
            raise ValueError('Cannot orient a body with its declared cavities')
        solids.append(solid)
    if len(solids) == 1:
        return solids[0], 1
    compound = TopoDS_Compound(); builder = BRep_Builder(); builder.MakeCompound(compound)
    for solid in solids:
        builder.Add(compound, solid)
    return compound, len(solids)
