"""Crop the same spatial neighborhood from source and reconstructed displays."""
import argparse
import json
from pathlib import Path
import numpy as np
from .mesh import dump
from .regional_quality import render


def closeup(quality, region, output, margin=.25):
    root = Path(quality); out = Path(output); out.mkdir(parents=True, exist_ok=False)
    metadata = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(metadata['partition']); display = np.load(root/'display_mesh.npz')
    v, f, labels = source['mesh_vertex_original'], source['mesh_triangle'], source['face_instance_id']
    selected = v[np.unique(f[labels == region])]
    if not len(selected):
        raise ValueError('Requested original region is absent')
    center = selected.mean(axis=0); axes = np.linalg.svd(selected-center, full_matrices=False)[2]
    local = (selected-center)@axes.T; lo, hi = local.min(axis=0), local.max(axis=0)
    padding = np.maximum(hi-lo, np.max(hi-lo)*.15)*margin; lo -= padding; hi += padding
    def crop(points, triangles, normals=None, region_labels=None):
        import vtk
        from vtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy
        from .render import polydata
        mesh = polydata(points, triangles, normals)
        if region_labels is not None:
            array = numpy_to_vtk(region_labels, deep=True); array.SetName('region'); mesh.GetCellData().AddArray(array)
        # Clip actual triangles against all six planes. Selecting centroids
        # leaves long CAD tessellation triangles protruding across the crop.
        for k in range(3):
            for bound, sign in [(lo[k], -1), (hi[k], 1)]:
                plane = vtk.vtkPlane(); plane.SetOrigin(*(center+bound*axes[k])); plane.SetNormal(*(sign*axes[k]))
                clip = vtk.vtkClipPolyData(); clip.SetInputData(mesh); clip.SetClipFunction(plane); clip.InsideOutOn(); clip.Update()
                mesh = clip.GetOutput()
        triangulate = vtk.vtkTriangleFilter(); triangulate.SetInputData(mesh); triangulate.Update(); mesh = triangulate.GetOutput()
        vertices = vtk_to_numpy(mesh.GetPoints().GetData()).copy()
        faces = vtk_to_numpy(mesh.GetPolys().GetConnectivityArray()).reshape(-1, 3).copy()
        normal = None if normals is None else vtk_to_numpy(mesh.GetPointData().GetNormals()).copy()
        regions = None if region_labels is None else vtk_to_numpy(mesh.GetCellData().GetArray('region')).copy()
        return vertices, faces, normal, regions
    original, cells, _, crop_labels = crop(v, f, region_labels=labels)
    result, result_cells, normal, _ = crop(display['points'], display['triangles'], display['normals'])
    np.savez_compressed(out/'display_source_crop.npz', mesh_vertex_original=original,
                        mesh_triangle=cells, face_instance_id=crop_labels)
    np.savez_compressed(out/'display_mesh.npz', points=result, triangles=result_cells, normals=normal)
    is_preview = metadata.get('status') == 'preview_only'
    dump(out/'run.json', dict(partition=str((out/'display_source_crop.npz').resolve()),
        source_quality=str(root.resolve()), source_partition=metadata['partition'], original_region=region,
        status='preview_only' if is_preview else 'visual_diagnostic',
        scope='Identical spatial crop for visual inspection only; not a new partition or fit. Crop cuts are artificial.'))
    render(out, title='支撑面局部预览（未回读 STEP）' if is_preview else '重建 STEP 局部裁切（原生法向）')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--quality', required=True); p.add_argument('--region', type=int, required=True)
    p.add_argument('--output', required=True); p.add_argument('--margin', type=float, default=.25)
    a = p.parse_args(); closeup(a.quality, a.region, a.output, a.margin)
