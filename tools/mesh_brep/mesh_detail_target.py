"""Recover STL detail on a shared, already parameterized subdivision mesh.

Interior samples project only to their supplied region. Interface samples
project to the original interface of the same two regions, once globally.
Junction samples retain their original mesh vertex. Local backtracking guards
orientation and sampled intersections without changing connectivity or labels.
"""
import numpy as np
from .mesh import Locator, stats
from .partition_fit import incidence


def closest_segments(points, vertices, segments):
    import vtk
    from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray
    xyz=vtk.vtkPoints();xyz.SetData(numpy_to_vtk(np.ascontiguousarray(vertices),deep=True))
    lines=vtk.vtkCellArray()
    lines.SetCells(len(segments),numpy_to_vtkIdTypeArray(np.c_[np.full(len(segments),2),segments].astype(np.int64).ravel(),deep=True))
    poly=vtk.vtkPolyData();poly.SetPoints(xyz);poly.SetLines(lines)
    tree=vtk.vtkStaticCellLocator();tree.SetDataSet(poly);tree.BuildLocator()
    result=np.empty_like(points);cell=vtk.reference(0);sub=vtk.reference(0);d=vtk.reference(0.)
    for i,p in enumerate(points):tree.FindClosestPoint(p,result[i],cell,sub,d)
    return result


def project_detail(points, faces, parents, coarse, regions, source_v, source_f, source_regions, source_ids, diagnostic=None):
    from .intersections import self_intersections
    fine_regions=regions[parents]
    wedges=np.unique(np.c_[faces.ravel(),np.repeat(fine_regions,3)],axis=0)
    count=np.bincount(wedges[:,0],minlength=len(points));target=points.copy()
    for region in np.unique(regions):
        ids=wedges[(wedges[:,1]==region)&(count[wedges[:,0]]==1),0]
        if len(ids):target[ids]=Locator(source_v,source_f[source_regions==region]).closest(points[ids])[0]
    edge,owners=incidence(faces);pairs=np.sort(fine_regions[owners],axis=1)
    source_edges,source_owners=incidence(source_f);source_pairs=np.sort(source_regions[source_owners],axis=1)
    for pair in np.unique(pairs[pairs[:,0]!=pairs[:,1]],axis=0):
        ids=np.unique(edge[np.all(pairs==pair,axis=1)])
        ids=ids[count[ids]==2]
        if len(ids):
            segments=source_edges[np.all(source_pairs==pair,axis=1)]
            if not len(segments):raise ValueError('A fixed regional interface has no source segments')
            target[ids]=closest_segments(points[ids],source_v,segments)
    junction=np.flatnonzero(count>2)
    if np.any(junction>=len(source_ids)):raise ValueError('Subdivision created an unexpected regional junction')
    target[junction]=source_v[source_ids[junction]]
    base_cross=np.cross(points[faces[:,1]]-points[faces[:,0]],points[faces[:,2]]-points[faces[:,0]])
    base_norm=np.linalg.norm(base_cross,axis=1);base_cross/=np.maximum(base_norm[:,None],1e-30)
    weight=np.ones(len(points));delta=target-points;history=[]
    for attempt in range(80):
        candidate=points+weight[:,None]*delta
        cross=np.cross(candidate[faces[:,1]]-candidate[faces[:,0]],candidate[faces[:,2]]-candidate[faces[:,0]])
        size=np.linalg.norm(cross,axis=1)
        bad=np.flatnonzero((np.sum(cross*base_cross,axis=1)<.03*size)|(size<base_norm*1e-5))
        folded=len(bad);collisions=[]
        if not len(bad):
            collisions=self_intersections(candidate,faces);bad=np.asarray(collisions,dtype=int)
        history.append(dict(attempt=attempt,folded_faces=folded,intersecting_faces=len(collisions)))
        if diagnostic is not None:
            from .mesh import dump
            dump(diagnostic,history)
        if not len(bad):break
        ids=np.unique(faces[bad])
        # Include the one-ring: isolated reductions merely move a fold to the
        # next triangle and create an unnecessarily jagged displacement field.
        ids=np.unique(faces[np.isin(faces,ids).any(axis=1)])
        new=np.where(weight[ids]>.002,weight[ids]*.25,0.)
        if np.array_equal(new,weight[ids]):raise ValueError('The original detail target is not regular')
        weight[ids]=new
    else:raise ValueError('Detail projection exceeded its local regularity budget')
    return candidate,dict(displacement_mm=stats(np.linalg.norm(candidate-points,axis=1)),
                          rejected_displacement_mm=stats(np.linalg.norm(candidate-target,axis=1)),
                          fully_projected_vertices=int(np.sum(weight==1)),vertices=len(points),history=history,
                          region_restricted=True,shared_boundary_projection=True)
