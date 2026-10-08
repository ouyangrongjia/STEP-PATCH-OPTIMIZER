"""Independent distance, native differential, and display evidence for fits."""
import argparse
import json
from pathlib import Path
import time

import numpy as np

from .mesh import Locator, dump, sample, stats, vertex_normals
from .regional_reconstruct import evaluate, tensor


def display_mesh(poles, corners, resolution=17):
    grid = np.linspace(0., 1., resolution)
    uv = np.stack(np.meshgrid(grid, grid, indexing='ij'), axis=-1).reshape(-1, 2)
    cells = []
    for i in range(resolution-1):
        for j in range(resolution-1):
            a = i*resolution+j; b = a+resolution
            cells.extend([[a, b, b+1], [a, b+1, a+1]])
    cells = np.array(cells); points = []; normals = []; tri = []; owners = []; params = []
    for pi, cp in enumerate(poles):
        xyz, normal, _ = evaluate(cp, uv)
        if corners[pi] == 3:
            cap = uv[:, 1] == 1
            _, normal[cap], _ = evaluate(cp, np.c_[uv[cap, 0], np.full(cap.sum(), 1-1e-7)])
        p = xyz[cells]
        good = np.linalg.norm(np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]), axis=1) > 1e-16
        points.append(xyz); normals.append(normal); tri.append(cells[good]+pi*len(uv))
        owners.extend([pi]*int(good.sum())); params.append(uv)
    return np.concatenate(points), np.concatenate(tri), np.concatenate(normals), np.array(owners), np.concatenate(params)


def weighted_stats(values, weights):
    order = np.argsort(values); x, w = np.asarray(values)[order], np.asarray(weights)[order]
    total = w.sum(); c = np.cumsum(w)/total
    return dict(rms=float(np.sqrt(np.sum(x*x*w)/total)),
                p50=float(x[np.searchsorted(c, .5)]), p95=float(x[np.searchsorted(c, .95)]),
                max=float(x[-1]), min=float(x[0]))


def audit(run, source_count=30000, reverse_per_patch=96):
    root = Path(run); started = time.perf_counter()
    meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(meta['partition']); v, f = source['mesh_vertex_original'], source['mesh_triangle']
    data = np.load(root/'surfaces.npz'); poles, corners = data['poles'], data['corners']
    points, triangles, normals, owners, uv = display_mesh(poles, corners)
    np.savez_compressed(root/'display_mesh.npz', points=points, triangles=triangles, normals=normals)
    mesh = Locator(v, f); display = Locator(points, triangles)
    x, _, _, _, _ = sample(v, f, source_count, 930601)
    _, _, ids, bary = display.closest(x)
    patch = owners[ids]; q = np.einsum('ij,ijk->ik', bary, uv[triangles[ids]])
    reconstructed = np.empty_like(x)
    for pi in np.unique(patch):
        selected = np.flatnonzero(patch == pi); param = q[selected].copy()
        cp = poles[pi]; degree = len(cp)-1; p = cp.reshape(-1, 3)
        for _ in range(12):
            a = tensor(degree, param)@p
            du = tensor(degree, param, 1, 0)@p; dv = tensor(degree, param, 0, 1)@p
            residual = a-x[selected]
            aa = np.sum(du*du, axis=1)+1e-18; ab = np.sum(du*dv, axis=1); bb = np.sum(dv*dv, axis=1)+1e-18
            ra = np.sum(residual*du, axis=1); rb = np.sum(residual*dv, axis=1)
            det = np.maximum(aa*bb-ab*ab, 1e-28)
            step = np.c_[(bb*ra-ab*rb)/det, (aa*rb-ab*ra)/det]
            param = np.clip(param-np.clip(step, -.1, .1), 0, 1)
        reconstructed[selected] = tensor(degree, param)@p
    forward = np.linalg.norm(x-reconstructed, axis=1)
    rng = np.random.default_rng(930602)
    reverse = []; angles = []; weights = []; singular = 0; inverted = 0
    sn = vertex_normals(v, f)
    for pi, cp in enumerate(poles):
        q = rng.uniform(1e-5, 1-1e-5, (reverse_per_patch, 2))
        xyz, n, jac = evaluate(cp, q)
        _, d, ids, bary = mesh.closest(xyz)
        target_n = np.einsum('ij,ijk->ik', bary, sn[f[ids]])
        target_n /= np.maximum(np.linalg.norm(target_n, axis=1)[:, None], 1e-30)
        dot = np.sum(n*target_n, axis=1)
        reverse.extend(d); angles.extend(np.degrees(np.arccos(np.clip(dot, -1, 1)))); weights.extend(jac)
        singular += int(np.sum(jac < 1e-14)); inverted += int(np.sum(dot < 0))
    reverse, angles, weights = map(np.asarray, (reverse, angles, weights))
    result = dict(forward_sampled_distance_upper_bound_mm=stats(forward),
                  reverse_stratified_distance_mm=stats(reverse),
                  reverse_area_weighted_distance_mm=weighted_stats(reverse, weights),
                  source_normal_difference_degrees=weighted_stats(angles, weights),
                  singular_samples=singular, opposed_source_normal_samples=inverted,
                  opposed_source_normal_area_fraction=float(weights[angles>90].sum()/weights.sum()),
                  estimated_surface_area_mm2=float(weights.sum()/reverse_per_patch),
                  source_count=source_count, reverse_samples=len(reverse),
                  tessellation_triangles=len(triangles), seconds=time.perf_counter()-started,
                  distance_scope='Independent area samples; nearest-point iteration on actual polynomial supports. Sampled upper bounds, not a certified Hausdorff bound.')
    np.savez_compressed(root/'quality_samples.npz', forward=forward, reverse=reverse, normal_degrees=angles, area_weights=weights)
    dump(root/'quality.json', result)
    print(json.dumps(result), flush=True)
    return result


def render(run, camera=None, title='重建曲面（真实导数法向）'):
    import vtk
    from PIL import Image, ImageDraw, ImageFont
    from .render import polydata
    root = Path(run); meta = json.loads((root/'run.json').read_text(encoding='utf-8'))
    source = np.load(meta['partition']); fit = np.load(root/'display_mesh.npz')
    v = source['mesh_vertex_original']; center = (v.max(axis=0)+v.min(axis=0))/2
    axes = np.load(camera)['axes'] if camera else np.linalg.svd(v-v.mean(axis=0), full_matrices=False)[2]
    directions = [axes[2]+.25*axes[1]-.16*axes[0], -axes[2]+.25*axes[1]+.16*axes[0],
                  axes[0]+.3*axes[2], -axes[0]+.3*axes[2], axes[1]+.2*axes[2], -axes[1]+.2*axes[2]]
    board = Image.new('RGB', (1800, 2460), 'white'); draw = ImageDraw.Draw(board)
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 22)
    for column, (title, points, triangles, normals) in enumerate([
            ('原始 STL', v, source['mesh_triangle'], None),
            (title, fit['points'], fit['triangles'], fit['normals'])]):
        mapper = vtk.vtkPolyDataMapper(); mapper.SetInputData(polydata(points, triangles, normals))
        for row, direction in enumerate(directions):
            renderer = vtk.vtkRenderer(); renderer.SetBackground(.965, .975, .985)
            actor = vtk.vtkActor(); actor.SetMapper(mapper)
            prop = actor.GetProperty(); prop.SetColor(.72, .76, .80); prop.SetAmbient(.2)
            prop.SetDiffuse(.72); prop.SetSpecular(.35); prop.SetSpecularPower(35); renderer.AddActor(actor)
            cam = renderer.GetActiveCamera(); cam.SetFocalPoint(*center); cam.SetPosition(*(center+100*direction))
            up=axes[0] if row>=4 else axes[1]
            direction=direction/np.linalg.norm(direction);right=np.cross(direction,up);right/=np.linalg.norm(right)
            vertical=np.cross(right,direction);projected=v-center
            cam.SetViewUp(*vertical); cam.ParallelProjectionOn()
            cam.SetParallelScale(max(np.ptp(projected@vertical),np.ptp(projected@right)*380/900)*.55)
            window = vtk.vtkRenderWindow(); window.SetOffScreenRendering(True); window.SetSize(900, 380)
            window.AddRenderer(renderer); window.SetMultiSamples(4); window.Render()
            capture = vtk.vtkWindowToImageFilter(); capture.SetInput(window); capture.ReadFrontBufferOff(); capture.Update()
            path = root/f'view_{row}_{column}.png'; writer = vtk.vtkPNGWriter(); writer.SetFileName(str(path))
            writer.SetInputConnection(capture.GetOutputPort()); writer.Write(); window.Finalize()
            board.paste(Image.open(path), (column*900, row*410+30))
            draw.text((column*900+20, row*410+3), f'{row+1}. {title}', font=font, fill='#20324b')
    board.save(root/'multiview_comparison.png')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--run', required=True)
    parser.add_argument('--camera'); parser.add_argument('--render', action='store_true')
    args = parser.parse_args(); audit(args.run)
    if args.render:
        render(args.run, args.camera)
