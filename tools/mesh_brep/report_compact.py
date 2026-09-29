"""Package the measured candidate-179 compact-B result in one review folder."""
import argparse
import csv
import json
import shutil
from datetime import datetime
from pathlib import Path
import numpy as np
from .mesh import read_stl, dump


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def metrics(report):
    return dict(support_controls=report['expression']['actual_support_control_points'],
                maximum_direction=report['expression']['maximum_direction_poles'],
                curve_controls=report['expression']['unique_3d_curve_controls'],
                pcurve_controls=report['expression']['pcurve_controls'],
                cad_forward_mm=report['step_to_surface']['distance_mm']['max'],
                cad_reverse_mm=report['surface_to_step']['distance_mm']['max'],
                cad_normal_p95_deg=report['step_to_surface']['normal_degrees']['p95'],
                cad_normal_max_deg=report['step_to_surface']['normal_degrees']['max'],
                mesh_forward_mm=report['mesh_to_surface']['distance_mm']['max'],
                mesh_reverse_mm=report['surface_to_mesh']['distance_mm']['max'],
                internal_normal_max_deg=max(x['normal_degrees']['max'] for x in report['internal_seams']),
                internal_gap_mm=max(x['separation_mm'] for x in report['internal_seams']),
                within_face_normal_jump_deg=report['interior_continuity']['maximum_sampled_normal_jump_degrees'],
                mean_curvature_abs_max_per_mm=max(x['absolute_mean_curvature_per_mm']['max'] for x in report['regularity']['faces']),
                sampled_singularities=sum(x['singular_samples'] for x in report['regularity']['faces']),
                local_step_bytes=report['step_bytes'])


def creo_summary(path):
    result = load(path); s = result['component_checks'][0]['summary']
    parameters = next(x['items'] for x in s['key_checks'] if x['name'] == 'PARAM_INFO')
    unit = next(x['info2'] for x in parameters if x['info1'] == 'PTC_UNITS_LENGTH')
    return dict(validated=result['validated'], checks={x['name']:dict(status=x['status'], value=x['answer']) for x in s['key_checks']},
                reported_model_length_unit=unit,
                accuracy_unit_note='ModelCHECK value is retained in reported model units; it is not the 0.001 mm OCCT sewing tolerance.',
                import_validation=s['import_validation'], started=result['started_at'], ended=result['ended_at'])


def write_tables(run, output):
    run, output = Path(run), Path(output); r = run/'compact_b'
    previous = metrics(load(r/'previous_confirmation.json')); current = metrics(load(r/'confirmation_final.json'))
    reconstruction = load(r/'compact13_refined/reconstruction.json')
    attachment = load(r/'attached_final/attachment.json'); assembly = load(r/'sewn_final/assembly.json')
    for stage in ['before_write', 'after_step_read']:
        valid = assembly[stage].pop('face_valid')
        assembly[stage]['invalid_face_count'] = sum(not x for x in valid)
    topology = load(r/'creo_face_audit_final/audit.json'); interface = load(r/'interfaces_final.summary.json')['candidate']
    current.update(interface_max_mm=interface['original_boundary_max_mm'], interface_g1_max_deg=interface['requested_g1_max_normal_degrees'],
                   independent_scalar_variables=reconstruction['solver']['independent_scalar_variables'],
                   mesh_vertices=load(r/'input_refined/generation.json')['topology']['vertices'],
                   mesh_triangles=load(r/'input_refined/generation.json')['topology']['triangles'],
                   actual_creo_local_faces=topology['after_local_faces'])
    old_fit = load(run/'candidate179_assembly/fair_B_corners/fairing.json')
    old_interface = load(run/'candidate179_assembly/final_interfaces.summary.json')['candidate']
    previous.update(interface_max_mm=old_interface['original_boundary_max_mm'], interface_g1_max_deg=old_interface['requested_g1_max_normal_degrees'],
                    independent_scalar_variables=old_fit['solver']['independent_scalar_variables'], actual_creo_local_faces=13)
    summary = dict(status='DiagnosticOnly', can_apply=False, selected_backend='tools.mesh_brep.compact',
                   previous=previous, current=current,
                   initial_protocol=load(r/'protocol.json'), selection=load(r/'selection.json'),
                   executions=load(r/'executions.json'),
                   reconstruction=reconstruction, attachment={k:attachment[k] for k in ['stage','tolerance_mm','boundary_error_mm','topology','wall_seconds']},
                   assembly=assembly, creo_topology=topology,
                   creo=creo_summary(r/'creo_final/result.json'), original_creo=creo_summary(r/'creo_original/result.json'),
                   isolated_replay=load(r/'isolation_final/result/isolation_evidence.json'),
                   mesh_generation=load(r/'input_refined/generation.json'),
                   step_to_mesh=load(r/'confirmation_final.json')['step_to_mesh']['distance_mm'],
                   mesh_to_step=load(r/'confirmation_final.json')['mesh_to_step']['distance_mm'],
                   intersections=load(r/'intersections.json'),
                   tolerances=dict(shape_mm=.03, normal_deg=15., smooth_seam_deg=5., interface_mm=.001,
                                   construction_mm=1e-7, maximum_direction_controls=25),
                   comparison_limits=['New input deflection is 0.0002 mm; previous fit used 0.001 mm.',
                                      'Boundary is now approximated within 0.001 mm rather than inherited exactly.',
                                      'Fairness, seam constraints, knot spaces and boundary parameterization all changed.',
                                      'Curvature peak and outer-neighbor G1 remain limitations; C2 is not a curvature bound.',
                                      'Creo batch import/check/export was run; figures render real STEP, not a live Creo screenshot.'])
    summary['isolated_replay']['pole_comparison'] = load(r/'isolation_comparison.json')
    dump(output/'metrics.json', summary)
    rows = []
    for key in current:
        rows.append(dict(metric=key, previous=previous.get(key, 'N/A'), current=current[key]))
    with (output/'comparison.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['metric','previous','current']); writer.writeheader(); writer.writerows(rows)
    timing = reconstruction['timing_seconds']
    with (output/'timing.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.writer(f); writer.writerow(['stage','seconds'])
        writer.writerow(['STEP region read', summary['mesh_generation']['step_read_seconds']])
        writer.writerow(['mesh and declared layout generation', summary['mesh_generation']['mesh_and_layout_seconds']])
        writer.writerows(timing.items()); writer.writerow(['pure reconstruction total', reconstruction['wall_seconds']])
        writer.writerow(['boundary attachment total', attachment['wall_seconds']]); writer.writerow(['whole CAD assembly total', assembly['wall_seconds']])
        writer.writerow(['independent confirmation total', load(r/'confirmation_final.json')['wall_seconds']])
        writer.writerow(['previous B same-sample confirmation', load(r/'previous_confirmation.json')['wall_seconds']])
        writer.writerow(['surface pair intersections', summary['intersections']['seconds']])
        for name, filename in [('selected Creo import/check', 'creo_final/result.json'),
                               ('original Creo import/check', 'creo_original/result.json'),
                               ('earlier uncompressed-pcurve Creo check', 'creo/result.json'),
                               ('selected Creo STEP export', 'creo_export/export.json')]:
            data = load(r/filename)
            writer.writerow([name, (datetime.fromisoformat(data['ended_at'])-datetime.fromisoformat(data['started_at'])).total_seconds()])
        writer.writerow(['actual Creo face audit', topology['wall_seconds']])
        writer.writerow(['warm reconstruction repeat', load(r/'warm_replay/reconstruction.json')['wall_seconds']])
        writer.writerow(['OS-locked replay including probes', summary['isolated_replay']['reconstruction_seconds']])
        writer.writerow(['packaged entry replay', load(r/'entry_replay/mesh/reconstruction.json')['wall_seconds']])
        writer.writerow(['all top-level completed reconstruction attempts including repeats', summary['executions']['sum_completed_reconstruction_seconds']])
        writer.writerow(['first failed trim25 complete wall time', 'N/A'])
    return summary


def generate(run, output):
    run, output = Path(run), Path(output); r = run/'compact_b'
    output.mkdir(parents=True, exist_ok=False)
    summary = write_tables(run, output)
    previous, current = summary['previous'], summary['current']
    reconstruction, attachment, assembly = summary['reconstruction'], summary['attachment'], summary['assembly']
    for src, name in [(r/'compact13_refined/network.step','B_mesh_only.step'),(r/'attached_final/network.step','B_local.step'),
                      (r/'sewn_final/sewn.step','B_sewn.step'),(r/'creo_final/import/b179_c2.prt.1','b179_c2.prt.1')]:
        shutil.copy2(src, output/name)
    (output/'input').mkdir()
    for name in ['input.stl','layout.json']:
        shutil.copy2(r/'input_refined'/name, output/'input'/name)
    shutil.copy2(Path(__file__).with_name('reproduce_compact.ps1'), output/'reproduce.ps1')
    figures(run, output)
    write_readme(summary, output)


def write_readme(summary, output):
    output = Path(output)
    previous, current = summary['previous'], summary['current']
    reconstruction, attachment, assembly = summary['reconstruction'], summary['attachment'], summary['assembly']
    p, n = previous, current
    text = f'''# candidate-179：当前 B 结果

优先打开 **B_sewn.step** 查看缝合整件，或打开已经由 Creo 11 导入的 **b179_c2.prt.1**。局部为 **5 张面、{n['support_controls']} 个支撑控制点**；Creo 往返仍为 5 张面。整件经 OCCT/STEP 往返为有效 Solid，Creo 往返为 1 个 Solid、零非退化自由边；Creo GEOM_CHECKS=0，导入评分 PASS。

| 文件 | 内容 |
|---|---|
| B_mesh_only.step | 仅由目录内 STL 与离散标签重建的五片曲面 |
| B_local.step | 上述曲面接上原 CAD 的 26 条接口边，支撑控制点不变 |
| B_sewn.step | 局部替换原 49 面后，与原模型其余部分缝合的整件 |
| b179_c2.prt.1 | 已实测导入的 Creo 原生整件 |
| comparison.png / uv_comparison.png | 同相机真实表面、全部节点参数线、Creo 实际面边，以及两档网格的真实 UV |
| metrics.json / comparison.csv / timing.csv | 精简事实、对比指标及分阶段耗时 |
| input/ / reproduce.ps1 | 纯网格输入与复跑入口 |

采用 **网格 MVC 参数化 + 自适应 C2 边界曲线 + 不同大小的共享接缝基底 + XYZ 联合光顺拟合**。复杂外链按给定公差近似，内部接缝只用 13 个控制点表示，再通过精确基底转换共享；中央面保持 13×13。面内只有简单节点，因此为 C2；相邻面的共同三维接缝在缝合前已一致。此法保留给定五片布局，没有进行拓扑搜索。

此前 Creo 中的密集线条包含真实拆面：旧版的内部重复节点只有 C0/C1 连续性，5 面导入后被拆为 13 面。本版消除了这类重复节点，实测仍为 5 面；参数线数量与真实面数分别展示。

原 CAD/旧控制网/缓存锁定不可读时已重跑：实际数据输入只有 input.stl、layout.json。结构标签、四角点和边链仍需明确提供。B_local/B_sewn 的后续接口挂接与整件组装会读取原 CAD；这不是整件无标签 STL 自动重建。

| 指标 | 上一版 B | 当前 B |
|---|---:|---:|
| 输入 STEP 面 / Creo 往返局部面 | 5 / 13 | 5 / 5 |
| 支撑控制点 | {p['support_controls']} | {n['support_controls']} |
| 最大单方向控制点 | {p['maximum_direction']} | {n['maximum_direction']} |
| 独立 XYZ 标量变量 | {p['independent_scalar_variables']} | {n['independent_scalar_variables']} |
| 3D 曲线 / pcurve 控制点 | {p['curve_controls']} / {p['pcurve_controls']} | {n['curve_controls']} / {n['pcurve_controls']} |
| CAD→曲面 / 曲面→CAD 最大距离，mm | {p['cad_forward_mm']:.6f} / {p['cad_reverse_mm']:.6f} | {n['cad_forward_mm']:.6f} / {n['cad_reverse_mm']:.6f} |
| CAD 法向 P95 / 最大，° | {p['cad_normal_p95_deg']:.3f} / {p['cad_normal_max_deg']:.3f} | {n['cad_normal_p95_deg']:.3f} / {n['cad_normal_max_deg']:.3f} |
| 内接缝最大法向差，° | {p['internal_normal_max_deg']:.3f} | {n['internal_normal_max_deg']:.3f} |
| 原邻面 G1 接口最大法向差，° | {p['interface_g1_max_deg']:.3f} | {n['interface_g1_max_deg']:.3f} |
| 原接口最大落面差，mm | {p['interface_max_mm']:.3g} | {n['interface_max_mm']:.6f} |
| 面内重复节点法向跳变，° | {p['within_face_normal_jump_deg']:.3f} | 0（简单节点 C2） |
| 采样最大绝对平均曲率，1/mm | {p['mean_curvature_abs_max_per_mm']:.3f} | {n['mean_curvature_abs_max_per_mm']:.3f} |

控制点比上一版减少 {(1-n['support_controls']/p['support_controls'])*100:.1f}%；与原先 aligned_inner_cubic 的 4324 点规模接近。原 CAD 对照使用同一批 30000 个确认点，反向各 12000 点，seed=2910300/2910301。新旧训练网格密度、边界约束和拟合目标有变化，收益不能全记在 UV 上。表中最大值是采样结果，不是连续域误差上界。

当前 STL 有 {n['mesh_vertices']} 点、{n['mesh_triangles']} 三角形。STEP→网格 / 网格→STEP 最大误差为 {summary['step_to_mesh']['max']:.6f} / {summary['mesh_to_step']['max']:.6f} mm；网格→重建 / 重建→网格为 {n['mesh_forward_mm']:.6f} / {n['mesh_reverse_mm']:.6f} mm。0.001 mm 网格版本为 3848 点，但原接口超过既有 0.001 mm 容差；细化到 0.0002 mm 后，本方案不改超参数即可缝合。采样加密不能代替输入网格精度。

本版按 **0.001 mm 容差覆盖原边界**，没有提高缝合容差；不再要求旧版约 1e-11 mm 的数学精确落面。内部接缝位置差为 {n['internal_gap_mm']:.3g} mm，曲线落面与 SameParameter 已独立检查。更少控制点的代价包括小幅距离增大、pcurve 数增加及局部曲率峰值变大。

**保留未通过项：**最大单方向 99 点仍超过 25 点目标；原邻面最大 G1 差 {n['interface_g1_max_deg']:.2f}° 仍超过 5°；曲率峰值尚需优化。Creo 原始整件/当前整件 GEOM_CHECKS 均为 0，短边分别为 1501/1487（相同检查阈值，报告原值 0.5），所以 ModelCHECK 未全通过。两者绝对精度报告值均为 0.004240749674，单位参数 PTC_UNITS_LENGTH 为 in；不能把这个数值写成 mm，也不能把 Creo 检查称为按 0.001 mm 精度通过。没有改动 Creo 导入精度选项；输出 STEP 长度单位为 mm。局部开放面没有封盖，水密结论针对拼回原余面的整件。仍标记 DiagnosticOnly / can_apply=false。

真实表面图使用 STEP 的 D1 法向，未做显示法向平滑；参数线对比绘出全部节点及每方向 11 条均匀参数线。无 UV 翻转、无采样 Jacobian 奇异点，面间求交未发现共享边以外的交线；这些检查不构成全域自交证明。

纯网格重建耗时 {reconstruction['wall_seconds']:.3f} s，接口挂接 {attachment['wall_seconds']:.3f} s，整件缝合/读写/回读 {assembly['wall_seconds']:.3f} s。网格生成、失败尝试、重复验证与 Creo 成本另列在运行记录；没有清空系统文件缓存，部分验证并行执行，不宣称严格端到端加速比。

保留 compact.py 作为下一轮 B 后端；下一处瓶颈是边界附近切平面/曲率的稳定性和曲线节点压缩。失败的浮动裁剪与单片展开方案、完整命令和原始证据见 [运行记录](../compact_b/RUN.md)。复跑：`./reproduce.ps1 -Python <Python路径>`；加 `-Assemble` 使用原接口与原余面组装整件。
'''
    (output/'README.md').write_text(text, encoding='utf-8')


def figures(run, output):
    from .render import render_shape, read_step, Image, ImageDraw, ImageFont
    ref = np.load(run/'baseline_cold/reference.npz'); font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 25)
    board = Image.new('RGB', (1800, 2160), 'white'); draw = ImageDraw.Draw(board)
    old = run/'candidate179_assembly/fair_B_corners/network.step'; new = run/'compact_b/attached_final/network.step'
    scratch = run/'compact_b/figures'; scratch.mkdir(exist_ok=True)
    for col, (path, title) in enumerate([(old,'上一版 B：13375 控制点'), (new,'当前 B：4290 控制点')]):
        shape = read_step(path)
        for row in range(2):
            name = scratch/f'surface_{col}_{row}.png'; render_shape(shape, name, ref['center'], ref['axes'], 3.35, bool(row))
            board.paste(Image.open(name), (col*900, 50+row*705))
        draw.text((col*900+15, 12), title, font=font, fill='#20324b')
        draw.text((col*900+15, 715), '全部节点参数线；相同相机、材质、光照', font=font, fill='#20324b')
        native = run/('candidate179_assembly/creo_face_audit/creo_local.step' if col == 0 else 'compact_b/creo_face_audit_final/creo_local.step')
        name = scratch/f'creo_{col}.png'; render_shape(read_step(native), name, ref['center'], ref['axes'], 3.35)
        board.paste(Image.open(name), (col*900, 1460))
        draw.text((col*900+15, 1420), f'Creo 实际往返面：{13 if col == 0 else 5} 张', font=font, fill='#20324b')
    draw.text((15, 2117), '真实 STEP 表面及 D1 法向；外接口 G1、曲率峰值和控制点方向预算仍见 README。', font=font, fill='#803724')
    board.save(output/'comparison.png')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams['font.sans-serif'] = ['Microsoft YaHei']
    fig, ax = plt.subplots(2, 5, figsize=(16, 6.7))
    for row, (mesh, fit, label) in enumerate([('inputs/candidate_fine', 'compact_b/compact13_frozen', '0.001 mm 网格'),
                                            ('compact_b/input_refined', 'compact_b/compact13_refined', '0.0002 mm 网格')]):
        _, triangles = read_stl(run/mesh/'input.stl')
        for fi in range(5):
            d = np.load(run/fit/f'patch_{fi}.npz'); uv = d['vertex_uv']; tri = triangles[d['triangle_ids']]
            ax[row, fi].triplot(uv[:, 0], uv[:, 1], tri, lw=.12, color='#365870')
            ax[row, fi].set_aspect('equal'); ax[row, fi].set_title(f'{label} · 面 {fi+1}', fontsize=10)
            ax[row, fi].set_xlim(-.02, 1.02); ax[row, fi].set_ylim(-.02, 1.02)
    fig.suptitle('实际网格连接与求解 UV；两档网格、十张图均无翻转', fontsize=13)
    fig.tight_layout(); fig.savefig(output/'uv_comparison.png', dpi=160); plt.close(fig)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--run', required=True); p.add_argument('--output', required=True)
    a = p.parse_args(); generate(a.run, a.output)
