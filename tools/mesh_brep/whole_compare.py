"""Same-camera STL / previous STEP / new STEP evidence, including all edges."""
import argparse
import json
from pathlib import Path
import numpy as np
import vtk
from vtk.util.numpy_support import vtk_to_numpy
from PIL import Image, ImageDraw, ImageFont
from OCP.BRep import BRep_Tool
from OCP.TopAbs import TopAbs_EDGE
from . import cad
from .mesh import dump
from .source_normals import crease_fan_normals
from .partition_fit import incidence
from .step_quality import tessellate
from .render import polydata, lines


def frame(mesh, edges, center, direction, up, scale, width=900, height=600):
    renderer = vtk.vtkRenderer(); renderer.SetBackground(.965, .975, .985)
    mapper = vtk.vtkPolyDataMapper(); mapper.SetInputData(polydata(mesh['points'], mesh['triangles'], mesh['normals']))
    actor = vtk.vtkActor(); actor.SetMapper(mapper)
    prop = actor.GetProperty(); prop.SetColor(.72, .76, .80); prop.SetAmbient(.2)
    prop.SetDiffuse(.72); prop.SetSpecular(.4); prop.SetSpecularPower(35)
    renderer.AddActor(actor)
    if edges:
        lines(renderer, edges, (.12, .16, .20), 1.)
    camera = renderer.GetActiveCamera(); camera.SetFocalPoint(*center)
    camera.SetPosition(*(center+100*direction)); camera.SetViewUp(*up)
    camera.ParallelProjectionOn(); camera.SetParallelScale(float(scale))
    renderer.ResetCameraClippingRange()
    window = vtk.vtkRenderWindow(); window.SetOffScreenRendering(True)
    window.SetSize(width, height); window.SetMultiSamples(4); window.AddRenderer(renderer); window.Render()
    capture = vtk.vtkWindowToImageFilter(); capture.SetInput(window); capture.ReadFrontBufferOff(); capture.Update()
    pixels = vtk_to_numpy(capture.GetOutput().GetPointData().GetScalars()).reshape(height, width, -1)
    result = Image.fromarray(np.flipud(pixels).copy()).convert('RGB'); window.Finalize()
    return result


def compare(partition, before, after, output, focus=None, focus_scale=5., focus_only=False, focuses=None):
    if focus_only and focus is None and not focuses:
        raise ValueError('Focus-only comparison requires a focus position')
    out = Path(output); out.mkdir(parents=True, exist_ok=False)
    data = np.load(partition); v, f = data['mesh_vertex_original'], data['mesh_triangle']
    n = crease_fan_normals(v, f)
    meshes = [dict(points=v[f].reshape(-1, 3), triangles=np.arange(f.size).reshape(-1, 3), normals=n.reshape(-1, 3))]
    source_edges, owners = incidence(f); labels = data['face_instance_id']
    source_edges = source_edges[labels[owners[:, 0]] != labels[owners[:, 1]]]
    borders = [list(v[source_edges])]; face_counts = [None]
    for source in [before, after]:
        shape = cad.read_generated(source); display, surfaces, _ = tessellate(shape, deflection=.001)
        meshes.append(display); face_counts.append(len(surfaces)); edge_points = []
        for edge in cad.shapes(shape, TopAbs_EDGE):
            if BRep_Tool.Degenerated_s(edge): continue
            first, last = BRep_Tool.Range_s(edge); curve = BRep_Tool.Curve_s(edge, first, last)
            # Whole-model exchange edges are short exact subcurves. Include
            # every internal knot as well, for unsplit external STEP inputs.
            cuts = [first, last]
            if hasattr(curve, 'NbKnots'):
                cuts += [curve.Knot(i) for i in range(1, curve.NbKnots()+1) if first < curve.Knot(i) < last]
            cuts = np.unique(cuts)
            q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 9)).ravel())
            edge_points.append(np.array([curve.Value(float(t)).Coord() for t in q]))
        borders.append(edge_points)
    center = (v.min(axis=0)+v.max(axis=0))/2
    axes = np.linalg.svd(v-v.mean(axis=0), full_matrices=False)[2]
    directions = [axes[2]+.25*axes[1]-.16*axes[0], -axes[2]+.25*axes[1]+.16*axes[0],
                  axes[0]+.3*axes[2], -axes[0]+.3*axes[2], axes[1]+.2*axes[2], -axes[1]+.2*axes[2]]
    labels = ['原始 STL', f'上一版 STEP：{face_counts[1]} 面', f'本版 STEP：{face_counts[2]} 面']
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 24); views = []
    for show_edges, filename in ([] if focus_only else [(False, 'whole_shaded.png'), (True, 'whole_boundaries.png')]):
        board = Image.new('RGB', (2700, 6*630), 'white'); draw = ImageDraw.Draw(board)
        for row, d in enumerate(directions):
            d = d/np.linalg.norm(d); up = axes[0] if row >= 4 else axes[1]
            right = np.cross(d, up); right /= np.linalg.norm(right); up = np.cross(right, d)
            scale = max(np.ptp((v-center)@up), np.ptp((v-center)@right)*600/900)*.54
            if not show_edges: views.append(dict(center=center.tolist(), direction=d.tolist(), up=up.tolist(), scale=float(scale)))
            for col, mesh in enumerate(meshes):
                board.paste(frame(mesh, borders[col] if show_edges else [], center, d, up, scale), (900*col, 630*row+30))
                draw.text((900*col+14, 630*row+2), f'{row+1}. {labels[col]}', font=font, fill='#20324b')
        board.save(out/filename)
    focus_views = list(focuses or [])
    if focus is not None:
        focus_views.insert(0, dict(center=np.asarray(focus, dtype=float).tolist(), scale=focus_scale, view=0))
    for index, detail in enumerate(focus_views):
        point = np.asarray(detail['center'], dtype=float); view = int(detail.get('view', 0)); scale = float(detail['scale'])
        if point.shape != (3,) or not np.isfinite(point).all() or not 0 <= view < 6 or not np.isfinite(scale) or scale <= 0:
            raise ValueError('Invalid focus camera')
        d = directions[view]/np.linalg.norm(directions[view]); up = axes[0] if view >= 4 else axes[1]
        board = Image.new('RGB', (2700, 1260), 'white'); draw = ImageDraw.Draw(board)
        for row, show_edges in enumerate([False, True]):
            for col, mesh in enumerate(meshes):
                board.paste(frame(mesh, borders[col] if show_edges else [], point, d, up, scale), (900*col, 630*row+30))
                draw.text((900*col+14, 630*row+2), labels[col], font=font, fill='#20324b')
        board.save(out/('focus_comparison.png' if len(focus_views) == 1 else f'focus_{index+1:02d}.png'))
    dump(out/'run.json', dict(before=str(Path(before).resolve()), after=str(Path(after).resolve()),
        views=views, focus=None if focus is None else np.asarray(focus).tolist(), focus_scale=focus_scale, focuses=focus_views,
        face_counts=face_counts, source_normals='Original STL crease-separated corner normals, 30 degrees',
        step_normals='Actual native D1, no display smoothing', deflection_mm=.001,
        boundaries='All nondegenerate CAD edges; no edge filtering', identical_material_and_lighting=True))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['partition', 'before', 'after', 'output']: p.add_argument('--'+name, required=True)
    p.add_argument('--focus', type=float, nargs=3); p.add_argument('--focus-scale', type=float, default=5.)
    p.add_argument('--focus-only', action='store_true')
    p.add_argument('--focuses', help='JSON list of center, scale and view (0..5) camera records')
    a = p.parse_args(); compare(a.partition, a.before, a.after, a.output, a.focus, a.focus_scale, a.focus_only,
                               None if a.focuses is None else json.loads(Path(a.focuses).read_text(encoding='utf-8')))
