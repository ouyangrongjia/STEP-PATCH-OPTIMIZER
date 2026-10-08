"""Same-camera source / previous / new regional STEP views, with real edges."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from . import cad
from .mesh import dump, vertex_normals
from .render import render_shape, polydata, lines
from .partition_fit import boundary_edges


def source_view(v, f, path, center, axes, scale):
    import vtk
    mapper = vtk.vtkPolyDataMapper(); mapper.SetInputData(polydata(v, f, vertex_normals(v, f)))
    actor = vtk.vtkActor(); actor.SetMapper(mapper); prop = actor.GetProperty()
    prop.SetColor(.72, .77, .84); prop.SetSpecular(.45); prop.SetSpecularPower(35)
    prop.SetAmbient(.18); prop.SetDiffuse(.72)
    renderer = vtk.vtkRenderer(); renderer.SetBackground(.97, .98, .99); renderer.AddActor(actor)
    # A supplied region can pinch at a source vertex while its individual CAD
    # charts remain regular. Draw every source boundary segment in that case.
    lines(renderer, [v[e] for e in boundary_edges(f)], (.12, .17, .23), 1.7)
    camera = renderer.GetActiveCamera(); camera.SetFocalPoint(*center)
    camera.SetPosition(*(center+20*(axes[2]+.25*axes[1]-.16*axes[0]))); camera.SetViewUp(*axes[1])
    camera.ParallelProjectionOn(); camera.SetParallelScale(scale)
    window = vtk.vtkRenderWindow(); window.SetOffScreenRendering(True); window.SetSize(900, 650)
    window.AddRenderer(renderer); window.SetMultiSamples(4); window.Render()
    capture = vtk.vtkWindowToImageFilter(); capture.SetInput(window); capture.ReadFrontBufferOff(); capture.Update()
    writer = vtk.vtkPNGWriter(); writer.SetFileName(str(path)); writer.SetInputConnection(capture.GetOutputPort()); writer.Write(); window.Finalize()


def compare(partition, region, before, after, output):
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    data = np.load(partition); v, f, labels = data['mesh_vertex_original'], data['mesh_triangle'], data['face_instance_id']
    used = np.unique(f[labels == region]); tri = np.searchsorted(used, f[labels == region]); v = v[used]
    center = v.mean(axis=0); axes = np.linalg.svd(v-center, full_matrices=False)[2]
    normal = np.cross(v[tri[:, 1]]-v[tri[:, 0]], v[tri[:, 2]]-v[tri[:, 0]]).sum(axis=0)
    if axes[2]@normal < 0:
        axes[2] *= -1
    axes[1] = np.cross(axes[2], axes[0]); size = np.ptp((v-center)@axes.T, axis=0)
    scale = max(size[1], size[0]*650/900)*.58
    shapes = [cad.read_generated(before), cad.read_generated(after)]
    counts = [len(cad.shapes(s, cad.TopAbs_FACE)) for s in shapes]
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 25)
    board = Image.new('RGB', (2700, 1420), 'white'); draw = ImageDraw.Draw(board)
    for row in range(2):
        view = axes.copy()
        if row:
            angle = np.radians(35); view[2] = np.cos(angle)*axes[2]+np.sin(angle)*axes[1]
            view[1] = np.cross(view[2], view[0])
        for col in range(3):
            path = out/f'view_{row}_{col}.png'
            if col == 0:
                source_view(v, tri, path, center, view, scale)
            else:
                render_shape(shapes[col-1], path, center, view, scale)
            board.paste(Image.open(path), (col*900, row*695+45))
            title = ['原始 STL（顶点法向显示）', f'上一版 STEP：{counts[0]} 面', f'本版 STEP：{counts[1]} 面'][col]
            draw.text((col*900+15, row*695+10), title, font=font, fill='#20324b')
    draw.text((15, 1390), f'固定分区 {region}；真实 STEP 边界和曲面导数法向；相同相机、尺度、材质和光照。', font=font, fill='#20324b')
    board.save(out/'comparison.png')
    dump(out/'run.json', dict(partition=str(Path(partition).resolve()), region=region,
        before=str(Path(before).resolve()), after=str(Path(after).resolve()), faces_before=counts[0], faces_after=counts[1],
        native_surface_normals=True, show_all_topological_edges=True, source_display_normals='area-weighted mesh vertex normals'))


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--partition', required=True); p.add_argument('--region', type=int, required=True)
    p.add_argument('--before', required=True); p.add_argument('--after', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); compare(a.partition, a.region, a.before, a.after, a.output)
