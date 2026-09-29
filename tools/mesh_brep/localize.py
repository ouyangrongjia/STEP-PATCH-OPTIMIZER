"""Locate held-out normal failures and visualize actual surface neighborhoods."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from .evaluate import distances
from .mesh import dump
from .render import render_shape
from geometry import read_step, shapes, edge_uses, compound, TopAbs_FACE, gp_Pnt
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex


def run(root):
    root = Path(root); case = root/'B_coarse_25'; evidence = np.load(case/'confirmation_v2.npz')
    source = read_step(root/'baseline_cold/original_region.step')
    candidate = read_step(case/'network.step'); edge_list = edge_uses(shapes(candidate, TopAbs_FACE))
    outer = compound([e for e, use in edge_list if len(use) == 1]); seams = compound([e for e, use in edge_list if len(use) == 2])
    ids = np.argsort(evidence['cad_normal_angle'])[-20:][::-1]
    points, normals = evidence['cad_points'][ids], evidence['cad_normal'][ids]
    report, arrays = distances(points, candidate, normals); records = []
    for k, i in enumerate(ids):
        vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(*points[k])).Vertex()
        d_outer, d_seam = BRepExtrema_DistShapeShape(vertex, outer), BRepExtrema_DistShapeShape(vertex, seams)
        records.append(dict(sample=int(i), point=points[k].tolist(), normal_degrees=float(arrays['normal_angle'][k]),
                            distance_mm=float(arrays['distance'][k]), face=int(arrays['patch'][k]), uv=arrays['uv'][k].tolist(),
                            distance_to_outer_mm=d_outer.Value(), distance_to_internal_seam_mm=d_seam.Value()))
    dump(root/'localization.json', dict(worst_normal_samples=records, selection='Post-freeze diagnostics only; no fitting changes'))
    reference = np.load(root/'baseline_cold/reference.npz'); axes = reference['axes']; focus = points[0]
    out = root/'figures'; font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 24)
    board = Image.new('RGB', (2700, 1420), 'white'); draw = ImageDraw.Draw(board)
    for i, (title, path) in enumerate([
            ('原始区域', root/'baseline_cold/original_region.step'),
            ('B 粗网格 · 3125 点', root/'B_coarse_25/network.step'),
            ('B 细网格 · 3125 点', root/'B_fine_25/network.step')]):
        shape = read_step(path)
        for wire in range(2):
            p = out/f'local_{i}_{wire}.png'; render_shape(shape, p, focus, axes, .35, bool(wire))
            board.paste(Image.open(p), (i*900, 55+wire*670))
        draw.text((i*900+20, 14), title, font=font, fill='#20324b')
    draw.text((20, 1390), '独立 CAD 法向误差最坏点附近；同视角、同尺度，未隐藏参数线。', font=font, fill='#803724')
    board.save(out/'boundary_closeup.png')
    # Measured error positions on the common original geometry, fixed color range.
    error_board = Image.new('RGB', (2700, 740), 'white'); draw = ImageDraw.Draw(error_board)
    for i, (name, title) in enumerate([('baseline_cold', '旧五片'), ('B_coarse_25', 'B 粗网格'), ('B_fine_25', 'B 细网格')]):
        data = np.load(root/name/'confirmation_v2.npz'); p = out/f'error_{i}.png'
        render_shape(source, p, reference['center'], axes, 3.35, error=(data['cad_points'], data['cad_normal_angle']))
        error_board.paste(Image.open(p), (i*900, 55)); draw.text((i*900+15, 12), f'{title} · 最大 {data["cad_normal_angle"].max():.2f}°', font=font, fill='#20324b')
    draw.text((20, 710), '独立原 CAD 30000 点；共同色标：蓝 0° → 红 45°；散点为实测，没有补点。', font=font, fill='#803724')
    error_board.save(out/'normal_error_comparison.png')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); run(p.parse_args().run)
