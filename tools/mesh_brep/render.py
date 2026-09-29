"""Same-camera geometry, full knot isoparametrics, UV and measured error views."""
import argparse
import json
import sys
from pathlib import Path
import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray
from PIL import Image, ImageDraw, ImageFont
from .mesh import read_stl, dump
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'candidate179_research'))
from geometry import read_step, tessellate, shapes, TopAbs_FACE, TopAbs_EDGE, BRep_Tool, TopAbs_REVERSED, evaluate, edge_samples


def polydata(points, triangles, normals=None):
    p = vtk.vtkPoints(); p.SetData(numpy_to_vtk(points, deep=True))
    cells = vtk.vtkCellArray(); cells.SetCells(len(triangles), numpy_to_vtkIdTypeArray(np.c_[np.full(len(triangles), 3), triangles].astype(np.int64).ravel(), deep=True))
    poly = vtk.vtkPolyData(); poly.SetPoints(p); poly.SetPolys(cells)
    if normals is not None:
        poly.GetPointData().SetNormals(numpy_to_vtk(normals, deep=True))
    return poly


def lines(renderer, points_list, color, width=1.):
    vp = vtk.vtkPoints(); cells = vtk.vtkCellArray()
    for points in points_list:
        line = vtk.vtkPolyLine(); line.GetPointIds().SetNumberOfIds(len(points))
        for i, p in enumerate(points):
            line.GetPointIds().SetId(i, vp.InsertNextPoint(*p))
        cells.InsertNextCell(line)
    poly = vtk.vtkPolyData(); poly.SetPoints(vp); poly.SetLines(cells)
    mapper = vtk.vtkPolyDataMapper(); mapper.SetInputData(poly)
    actor = vtk.vtkActor(); actor.SetMapper(mapper); actor.GetProperty().SetColor(*color)
    actor.GetProperty().SetLineWidth(width); actor.GetProperty().LightingOff(); renderer.AddActor(actor)


def render_shape(shape, path, center, axes, scale, wire=False, error=None):
    points, tri, uv, owners = tessellate(shape, .0005)
    normals = np.zeros_like(points)
    for i, face in enumerate(shapes(shape, TopAbs_FACE)):
        ids = np.unique(tri[owners == i]); _, normal = evaluate(BRep_Tool.Surface_s(face), uv[ids])
        normals[ids] = normal*(-1 if face.Orientation() == TopAbs_REVERSED else 1)
    mapper = vtk.vtkPolyDataMapper(); mapper.SetInputData(polydata(points, tri, normals))
    actor = vtk.vtkActor(); actor.SetMapper(mapper)
    actor.GetProperty().SetColor(.72, .77, .84); actor.GetProperty().SetSpecular(.45)
    actor.GetProperty().SetSpecularPower(35); actor.GetProperty().SetAmbient(.18); actor.GetProperty().SetDiffuse(.72)
    renderer = vtk.vtkRenderer(); renderer.SetBackground(.97, .98, .99); renderer.AddActor(actor)
    borders = [edge_samples(e, 385)[1] for e in shapes(shape, TopAbs_EDGE)]
    lines(renderer, borders, (.12, .17, .23), 1.7)
    if wire:
        curves = []; t = np.linspace(0, 1, 257)
        for face in shapes(shape, TopAbs_FACE):
            s = BRep_Tool.Surface_s(face)
            for axis in range(2):
                get, count = (s.UKnot, s.NbUKnots()) if axis == 0 else (s.VKnot, s.NbVKnots())
                for k in sorted(set([get(i) for i in range(1, count+1)]+np.linspace(0, 1, 11).tolist())):
                    q = np.c_[np.full(len(t), k), t] if axis == 0 else np.c_[t, np.full(len(t), k)]
                    p, _ = evaluate(s, q); curves.append(p)
        lines(renderer, curves, (.16, .35, .48), .65)
    if error is not None:
        p, values = error; cloud = vtk.vtkPolyData(); vp = vtk.vtkPoints(); vp.SetData(numpy_to_vtk(p, deep=True)); cloud.SetPoints(vp)
        cells = vtk.vtkCellArray()
        for i in range(len(p)):
            cells.InsertNextCell(1); cells.InsertCellPoint(i)
        cloud.SetVerts(cells); cloud.GetPointData().SetScalars(numpy_to_vtk(values, deep=True))
        lut = vtk.vtkLookupTable(); lut.SetHueRange(.67, 0.); lut.SetRange(0, 45); lut.Build()
        m = vtk.vtkPolyDataMapper(); m.SetInputData(cloud); m.SetLookupTable(lut); m.SetScalarRange(0, 45)
        a = vtk.vtkActor(); a.SetMapper(m); a.GetProperty().SetPointSize(3); a.GetProperty().LightingOff(); renderer.AddActor(a)
    camera = renderer.GetActiveCamera(); camera.SetFocalPoint(*center)
    direction = axes[2]+.25*axes[1]-.16*axes[0]
    camera.SetPosition(*(center+20*direction)); camera.SetViewUp(*axes[1]); camera.ParallelProjectionOn(); camera.SetParallelScale(scale)
    window = vtk.vtkRenderWindow(); window.SetOffScreenRendering(True); window.SetSize(900, 650)
    window.AddRenderer(renderer); window.SetMultiSamples(4); window.Render()
    capture = vtk.vtkWindowToImageFilter(); capture.SetInput(window); capture.ReadFrontBufferOff(); capture.Update()
    writer = vtk.vtkPNGWriter(); writer.SetFileName(str(path)); writer.SetInputConnection(capture.GetOutputPort()); writer.Write(); window.Finalize()


def compose(run):
    run = Path(run); out = run/'figures'; out.mkdir(exist_ok=True)
    source = run/'baseline_cold/original_region.step'
    ref = np.load(run/'baseline_cold/reference.npz'); center, axes = ref['center'], ref['axes']
    cases = [('原始区域 · 49 面', source),
             ('旧五片 · 4324 控制点', run/'baseline_cold/aligned_inner_cubic/patch_faces.step'),
             ('A 反求 UV + XYZ · 4324 点', run/'A/inverse_xyz/network.step'),
             ('A 网格 UV + XYZ · 4324 点', run/'A/mesh_uv_xyz/network.step'),
             ('B 网格 · 4205 点', run/'B_coarse_29_global/network.step'),
             ('B 网格 · 3125 点', run/'B_coarse_25/network.step')]
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 25)
    canvas = Image.new('RGB', (2700, 2800), 'white'); draw = ImageDraw.Draw(canvas)
    for i, (title, path) in enumerate(cases):
        shape = read_step(path); col, row = i % 3, i//3
        for mode in range(2):
            image_path = out/f'geometry_{i}_{mode}.png'
            render_shape(shape, image_path, center, axes, 3.35, bool(mode))
            canvas.paste(Image.open(image_path), (col*900, row*1390+50+mode*660))
        draw.text((col*900+15, row*1390+12), title, font=font, fill='#20324b')
        draw.text((col*900+15, row*1390+710), '全部节点参数线 + 每方向 11 条等参线', font=font, fill='#20324b')
    draw.text((20, 2760), '真实 STEP 表面及导数法向；全部视图使用同一相机、光照、材质与尺度。未通过项见报告。', font=font, fill='#803724')
    canvas.save(out/'candidate_comparison.png')
    for case, title in [('B_closed64_13', '闭合自由曲面夹具 · 6 面 / 1014 点'), ('B_wrapped64_17', '240° 包裹区域 · 1 面 / 289 点')]:
        shape = read_step(run/case/'network.step'); p, _, _, _ = tessellate(shape)
        cen = (p.min(axis=0)+p.max(axis=0))/2
        ax = np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0.]])
        board = Image.new('RGB', (1800, 730), 'white'); d = ImageDraw.Draw(board)
        for mode in range(2):
            f = out/f'{case}_{mode}.png'; render_shape(shape, f, cen, ax, 2.7, bool(mode)); board.paste(Image.open(f), (mode*900, 55))
        d.text((20, 12), title+'；同相机真实曲面 / 全节点参数线', font=font, fill='#20324b')
        board.save(out/f'{case}.png')
    # True triangle UV charts; no synthetic regularized UV illustration.
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(3, 5, figsize=(16, 9))
    inverse = np.load(run/'A/inverse_mapping/parameterization.npz')
    amesh = np.load(run/'A/mesh_correspondence.npz')
    bp, bt = read_stl(run/'inputs/candidate_coarse/input.stl')
    for i in range(5):
        q = inverse['uv'][inverse['patch'] == i]; ax[0, i].scatter(q[:, 0], q[:, 1], s=.08)
        tri = amesh['triangles'][amesh['labels'] == i]; uv = amesh['vertex_uv'][i]
        ax[1, i].triplot(uv[:, 0], uv[:, 1], tri, linewidth=.18)
        b = np.load(run/f'B_coarse_25/patch_{i}.npz'); uv = b['vertex_uv']; tri = bt[b['triangle_ids']]
        ax[2, i].triplot(uv[:, 0], uv[:, 1], tri, linewidth=.18)
        for j in range(3):
            ax[j, i].set_aspect('equal'); ax[j, i].set_xlim(-.02, 1.02); ax[j, i].set_ylim(-.02, 1.02)
            ax[j, i].set_title(f'{["A inverse samples", "A mesh triangles", "B mesh triangles"][j]}: patch {i}')
    fig.tight_layout(); fig.savefig(out/'uv_comparison.png', dpi=170); plt.close(fig)
    dump(out/'views.json', dict(candidate_center=center.tolist(), candidate_axes=axes.tolist(), parallel_scale=3.35,
                                tessellation_deflection_mm=.0005, normals='Exact surface D1; no smoothing filter',
                                wire='Every native knot and 11 uniform parameters per direction; nothing hidden'))


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); compose(p.parse_args().run)
