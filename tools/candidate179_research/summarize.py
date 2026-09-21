"""Assemble measured evidence and a review bundle, without file digests."""
import argparse
import csv
import datetime as dt
import importlib.metadata as metadata
import json
import platform
import re
import subprocess
import sys
import zipfile
from pathlib import Path
from geometry import dump


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True);args=parser.parse_args()
    out=Path(args.run).resolve();baseline=Path(args.baseline).resolve();repo=Path(__file__).resolve().parents[2]
    audit=read(out/'input_audit.json');ev=read(out/'evaluation.json');local=read(out/'local_diagnostics.json');boundary=read(out/'boundary_audit.json')
    crop=read(out/'g17_outer_cropped/crop.json');integration=read(out/'g17_outer_cropped/integration.json');construction=read(out/'baseline_construction.json')
    historical=read(baseline/'validation_summary.json')
    names=['g17_data','g17_fair','g17_outer','g17_frozen_seams','g25_outer','g25_strict_outer','g17_svd','g17_exterior_fair']
    labels=['只拟合原始数据','全域公平项','全域公平 + 外边界罚项','再加旧人工接缝罚项','增至 25×25','外边界罚权升至 100000','同目标换为 SVD','仅在裁剪外域加公平项']
    rows=[]
    for name,label in zip(names,labels):
        fit=read(out/name/'fit.json');measurement=ev[name];brep=read(out/name/'brep.json')
        rows.append(dict(name=name,change=label,faces=4,design_poles=fit['unique_support_poles'],step_poles=brep['step_total_support_poles'],
                         original_to_support_max_mm=measurement['support_projection_distance_mm']['max'],
                         original_to_support_rms_mm=measurement['support_projection_distance_mm']['rms'],
                         normal_p95_deg=measurement['support_projection_normal_degrees']['p95'],
                         normal_max_deg=measurement['support_projection_normal_degrees']['max'],
                         original_boundary_max_mm_193=fit['boundary_graph_distance_mm']['max'],seconds=fit['seconds']))
    with (out/'comparison.csv').open('w',newline='',encoding='utf-8-sig') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    dependencies={name:metadata.version(name) for name in ['cadquery-ocp','cadquery-ocp-proxy','numpy','scipy','vtk','matplotlib','pillow','shapely']}
    dump(out/'dependencies.json',dependencies)
    creo_file=out/'g17_outer_cropped/creo/result.json';creo=read(creo_file) if creo_file.exists() else dict(status='NotRun',validated=False)
    stats_baseline=ev['baseline'];best=local['g17_outer']['trim_correction'];shape_best=local['g17_data']['trim_correction']
    g1=max(e['candidates']['g17_outer']['adjacent_normal_gap_degrees']['max'] for e in boundary['edges'] if e['requested_continuity']=='G1')
    g0=max(e['candidates']['g17_outer']['original_curve_to_support_mm']['max'] for e in boundary['edges'])
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
    branch=subprocess.check_output(['git','branch','--show-current'],cwd=repo,text=True).strip()
    sampling_file=out/'sampling_reproduction.json'
    sampling=read(sampling_file) if sampling_file.exists() else None
    sampling_note=(f"独立重跑原始 STEP 提取与采样，{len(sampling)} 个数组逐项数值一致，见 [采样复现](sampling_reproduction.json)。"
                   if sampling and all(item['matches_at_1e_12'] for item in sampling.values())
                   else '本目录未记录独立的采样重跑一致性验证。')
    gates=dict(original_shape_maximum_distance=dict(limit_mm=.03,observed_mm=best['distance_mm']['max'],passed=best['distance_mm']['max']<=.03),
               original_shape_maximum_normal=dict(limit_degrees=15,observed_degrees=best['normal_degrees']['max'],passed=best['normal_degrees']['max']<=15),
               original_boundary_curve_on_surface=dict(limit_mm=.001,observed_mm=g0,passed=g0<=.001),
               external_requested_G1=dict(observed_degrees=g1,engineering_continuity_preserved=False,
                   product_gate='Not independently invoked; original code treats adjacent normals as soft constraints. The 5 degree internal seam gate is not silently extended here.'),
               internal_artificial_seams=dict(limit_degrees=5,observed_degrees=max(e['normal_degrees']['max'] for e in crop['seams']),passed=True),
               moved_curve_same_parameter=crop['edge_checks'],trimmed_step_readback=crop['after_read'],
               fixed_tolerance_sewing=dict(tolerance_mm=.001,free_edges=integration['after_step_read']['free_edges'],solids=integration['after_step_read']['solids'],passed=False),
               commercial_cad=dict(status=creo['status'],passed=creo.get('validated',False)),
               product_StrictTopologyGate='Not invoked; independent boundary and open-shell checks already fail',can_apply=False)
    manifest=dict(schema_version=1,status='ResearchCompleted_ProductRejected',candidate_id=179,
                  source=audit,baseline=dict(archive_commit='bfe1412574a623225657543cf1af140b0ae3b294',
                  geometry_run='20260729_032156_431',original_runtime_commit='90282b948bc073c3b6f7e8804c25526780626807',original_runtime_dirty=True),
                  implementation=dict(base_commit=commit,branch=branch,worktree=str(repo),product_source_modified=False,committed=False,pushed=False),
                  environment=dict(python=sys.version,executable=sys.executable,platform=platform.platform(),dependencies=dependencies),
                  experiments=rows,preferred_diagnostic='g17_outer_cropped/four_patch_diagnostic.step',
                  shape_priority_diagnostic='g17_data_cropped/four_patch_diagnostic.step',gates=gates,
                  verification=dict(sampling_reproduction=sampling,
                    execution_scope='Individual stages executed; combined PowerShell runner syntax checked, not rerun end-to-end'),
                  execution=dict(initial_fits=6,evidence_triggered_solver_controls=2,additional_refits_after_controls=0,
                    recorded_fit_stage_seconds=sum(r['seconds'] for r in rows),peak_memory='Not measured',
                    timing_scope='Fit stage timers include support export and immediate statistics; not directly comparable to historical end-to-end time'),
                  completed_at=dt.datetime.now(dt.timezone.utc).isoformat())
    dump(out/'run.json',manifest)
    table='\n'.join(f"| {r['name']} | {r['step_poles']} | {r['original_to_support_max_mm']:.5f} | {r['original_to_support_rms_mm']:.5f} | {r['normal_p95_deg']:.2f} / {r['normal_max_deg']:.2f} | {r['original_boundary_max_mm_193']:.5f} |" for r in rows)
    coef=read(out/'g17_svd/fit.json')['maximum_pole_change_mm']
    component_lines=[]
    for component in creo.get('component_checks',[]):
        s=component['summary'];failed=', '.join(f"{x['name']}={x['answer']}" for x in s['failed_checks']) or '无 ERROR'
        component_lines.append(f"- `{Path(component['object']).name}`：报告解析 {s['parsed']}，{failed}。")
    coons_error=max(p['styles']['Coons']['max_difference_mm'] for p in construction['boundary_only_reconstruction'])
    report=f'''# candidate-179：八组拟合与边界机制实证

本轮取得面内形状、平顺性与复杂度的阶段性改善，但没有得到可局部替换的候选。推荐保留四片、446 个实际 STEP 控制点的平衡诊断，作为下一轮边界适配的对照；同时保留形状误差更低、但未裁剪支撑域异常的版本。两者均为 `DiagnosticOnly`、`can_apply=false`。

## 直接审阅

- [平衡诊断：四张裁剪面](g17_outer_cropped/four_patch_diagnostic.step)、[四张支撑面](g17_outer_cropped/support.step)、[整件开壳诊断](g17_outer_cropped/whole_model_diagnostic.step)。
- [形状优先诊断：四张裁剪面](g17_data_cropped/four_patch_diagnostic.step)、[异常支撑面](g17_data_cropped/support.step)。该版本不能仅凭裁剪后的外观选为正式结果。
- [原始 49 面区域](original_region.step)、[相同视角：平衡诊断](figures/comparison_g17_outer_cropped.png)、[相同视角：形状优先](figures/comparison_g17_data_cropped.png)。
- [同输入误差图](figures/error_maps.png)、[局部边界剖面](figures/boundary_profile.png)，均附同名矢量 PDF；三维图由真实 STEP 三角化后用 VTK 渲染。
- [机器记录](run.json)、[八组表格](comparison.csv)、[代码快照](code_snapshot.zip)、[轻量审阅包](review_bundle.zip)。完整整件和 Creo 原始报告保留在本目录，未压入轻量包。

## 来源与比较合同

研究工作树：`{repo}`，分支 `{branch}`，基于归档提交 `{commit}`。主工作区与原 `native-regional-refit` 工作树的既有修改未改动；没有提交、合并或推送。

指定基线仍是 `20260729_032156_431` 的四片 STEP。其运行记录是 `90282b9`、`dirty=true`；归档提交 `bfe1412` 是本轮源码起点，不是该次脏工作树的精确源码快照。没有把后续单片或 49→46 产品回归替换为本轮基线。

原始输入为 `{audit['source']}`，{audit['source_bytes']:,} 字节、{audit['input_faces']} 面；FaceId 按项目 `TopologyGraph.cpp` 的 `TopExp.MapShapes` 从零编号，选择 {audit['selected_face_ids'][0]}–{audit['selected_face_ids'][-1]} 共 49 面。STEP 声明毫米。当前选区有 26 条真实外边；旧四片布局把其中两条细分后形成 28 段外边与 5 条人工内缝。

原始 STEP 三角化只用于按面积抽样和定位，挠度 0.003 mm、角度 0.1 rad，共 {audit['tessellation']['triangles']:,} 个三角形。拟合 12,000 点、评估 18,000 点，种子分别为 179、180；点与法向重新在原始 Face 的 UV 上精确求值。反向检查从实际候选表面抽取 6,000 点、种子 181，并用原始 CAD 支撑面求距；最坏点及固定样本由 OCCT 全原始区域距离查询交叉核验。所有对比使用同一参考数据，四片结果没有作为形状真值。

该区域用于方法选择，不能称为独立模型确认集。随机样本最大值不是连续域误差上界。反向查询先用 24 个邻近三角形中心选原始面，再求 CAD 距离；OCCT 全局抽查一致不构成全域证明。采样与归档旧 holdout 不同，旧 `0.324422 mm` 不与本轮数值混表。本轮基线正向最大/RMS 为 {stats_baseline['distance_mm']['max']:.5f}/{stats_baseline['distance_mm']['rms']:.5f} mm。

## 主要发现及证据等级

**实测：指定基线是边界驱动的 Coons 填充形式。** 用每张面自身的四条边重新做 OCCT Coons 填充，控制网尺寸全部一致，961 点/面的最大几何差仅 {coons_error:.2g} mm（数值一致性，不是测量精度）。Stretch 和 Curved 填充不匹配。归档拟合记录的迭代数为零，约 342 ms 后因 25 控制点上限内不能表示冻结逻辑边而退出。见 [构造核验](baseline_construction.json)。这比“控制点太多导致拟合过拟合”的泛化解释更具体：所选面内形状没有经过成功的原始区域联合拟合。

**实测：网格翻转、曲面折叠和 Face 方向不是同一问题。** 归档记录有 116 个参数网格翻转/退化及装配前的 Face 方向错误；本轮读取交付的基线 STEP 后四个 Face 均有效。在跨 knot 的稠密支撑采样中未见 Jacobian 零点或投影方向反转，但其曲率与面内波纹仍明显。不能把旧 UV 统计直接称为连续 CAD 曲面自折叠。见 [统一评估](evaluation.json)。

**实测：原区域具有可用的简单参数域。** 原始网格投影无翻转、无退化，参数条件数最大 {boundary['graph_mesh_parameter_condition_ratio']['max']:.3f}；18,000 点的法向与 PCA 法向最小点积约 0.532。故本轮直接验证投影平面上的三次高度 B-spline，保留旧四片在平面上的分区，允许人工内缝高度变化。构造后的高度图在整个支撑域可证明单射、Jacobian 不为零；裁剪环的采样多边形无有效内部重叠，见 [分区检查](partition_audit.json)。此证明不适用于旧基线，也不代表外边界合格。

**实测：原型的异常外推不是正规方程求解器失算。** SVD 对同一带微小 ridge 的目标给出几乎相同的结果，控制点最大变化 {coef:.3g} mm，外域仍到约 −30 mm。只对裁剪外域加公平项消除了巨大外推，但牺牲边界附近的形状与法向。简单限制每片支撑参数范围仍不能救回未正则版本。该结论只涉及本轮独立求解器，未排除原生 C++ 实现里的其他约束或初始化问题。

**合理推断：下一轮的主要瓶颈在真实外边界的窄过渡带。** 形状优先版本在离边界大于 0.2 mm 的样本上法向最大偏差约 2.79°，而 FaceId 1706 的边界附近达到约 50°，沿曲面进入约 0.03 mm 后才接近 15°门。原始区域的 85 条内缝法向最大约 2.42°；14 条要求 G1 的外边上，原始相邻面法向差最大仅约 0.013°。这些是必须保留的局部几何关系，不能当作噪声平滑掉。尚待验证的是边界贴合参数域、局部非均匀节点或边界带布局能否以低复杂度保住它们。

## 对照结果

初始六组只改变明确的一项：数据项→公平项→外边界罚项→旧内缝罚项；17→25 控制点；外边界罚权 100→100000。随后因为发现外域异常，只追加 SVD 和外域公平两个控制，总计八次拟合，没有继续搜索参数。初始协议和修改理由分别见 [protocol.json](protocol.json)、[protocol_amendment.json](protocol_amendment.json)。

下表正向距离是共同数值投影到有界支撑面的结果；部分投影可能落在裁剪域外，主要候选已另外用真实裁剪 Face 的 OCCT 距离纠正。外边界列是统一的 193 点/原边位置残差；主要候选又做了 385 点/边的独立复核，数值一致。`g17_frozen_seams` 的名字沿用实验标识，实际是罚项，**不是硬冻结成功**。

| 实验 | STEP 中支撑控制点总数 | 正向最大 mm | 正向 RMS mm | 法向 P95 / 最大 ° | 原边界残差 mm |
|---|---:|---:|---:|---:|---:|
{table}

强制靠近旧人工缝后，原始形状误差增大；更高控制点数没有自动解决外边界；巨大边界罚权把边界残差压低但引入严重面内误差，反向最大偏差约 0.373 mm。因此这些配置均不保留为主候选。所有失败曲面仍在各自目录。

原始图形母曲面只有 289 或 625 个设计控制点，但 STEP 写出四个支撑面记录，实际是 1156 或 2500 个控制点；没有按内存共享指针少算 STEP 复杂度。最终用精确 knot 插入和参数域限制将每片支撑裁小，平衡候选实际为 9×13、10×12、10×11、9×11，共 **446 个控制点、4 个 Face、4 张支撑面**，单方向最多 13。裁剪内几何变化低于 3e−14 mm；该操作没有重新拟合，也没有解决外边界误差。

## 推荐保留与验收

| 指标 | 指定基线 | 形状优先诊断 | 平衡诊断（本轮主交付） |
|---|---:|---:|---:|
| 实际 STEP Face / 控制点 | 4 / 9645 | 4 / 446 | 4 / 446 |
| 裁剪 Face 正向最大偏差 mm | {stats_baseline['distance_mm']['max']:.5f} | {shape_best['distance_mm']['max']:.5f} | {best['distance_mm']['max']:.5f} |
| 裁剪 Face 正向 RMS mm | {stats_baseline['distance_mm']['rms']:.5f} | {shape_best['distance_mm']['rms']:.5f} | {best['distance_mm']['rms']:.5f} |
| 反向采样最大偏差 mm | {local['baseline']['reverse']['distance_mm']['max']:.5f} | {local['g17_data']['reverse']['distance_mm']['max']:.5f} | {local['g17_outer']['reverse']['distance_mm']['max']:.5f} |
| 法向 P95 / 最大 ° | {stats_baseline['normal_degrees']['p95']:.2f} / {stats_baseline['normal_degrees']['max']:.2f} | {shape_best['normal_degrees']['p95']:.2f} / {shape_best['normal_degrees']['max']:.2f} | {best['normal_degrees']['p95']:.2f} / {best['normal_degrees']['max']:.2f} |
| 原边界位置残差 mm | <1e−10（归档及边一致性检查） | 0.02260 | {g0:.5f} |
| 人工内缝最大法向差 ° | {max(e['normal_degrees']['max'] for e in local['baseline']['seams']):.2f} | <0.00001 | <0.00001 |
| 未裁剪支撑外域 | 保留旧诊断限制 | 最大约 30 mm 异常外推 | 高度范围约 −0.277～0.378 mm |
| 固定 0.001 mm 整件缝合 | 归档 1 Solid / 0 自由边 | 0 Solid / 54 自由边 | 0 Solid / 54 自由边 |

平衡诊断在有界支撑、控制点和面内波纹之间更适合继续研究，但其正向最大 {best['distance_mm']['max']:.5f} mm 仍超过 0.03 mm，最坏法向 {best['normal_degrees']['max']:.2f}° 超过 15°，外边界 {g0:.5f} mm 超过 0.001 mm。原外侧 G1 关系的最大法向差变为 {g1:.2f}°，相较原始约 0.013° 已明显丢失切向关系；现有代码把相邻法向作为软约束，本轮不把内部接缝的 5°门擅自扩成新的外侧产品门。

候选四片 STEP 回读和 Face/Wire 有效性通过，38 次 SameParameter/Curve-on-Surface 复核通过，5 条人工内缝位置与法向连续。但这些检查针对**重新生成、移动后的曲线**，不能抵消原始曲线落面失败。真正保留原边界的局部替换没有完成；没有调用产品 Apply 或声称 `StrictTopologyGate` 通过。见 [裁剪与支撑核验](g17_outer_cropped/crop.json)、[原始外边界与相邻面](boundary_audit.json)、[固定容差缝合](g17_outer_cropped/integration.json)。

Creo 实际导入为两个分离零件和装配，已对两个零件及根装配分别运行 ModelCHECK。根装配没有 ERROR 不代表几何闭合；两个零件仍失败：

{chr(10).join(component_lines) if component_lines else '本次复现未运行 Creo；不能声称外部验收。'}

未修改 ModelCHECK 配置。XML 的单位元数据和精度属于 Creo 当前模板，不能直接视为项目毫米公差；其 `SHORT_EDGES` 也不等于新增缺陷数，未改动的大块原模型已有大量短边。本轮不据此宣称跨 CAD 尺寸精度通过。[完整 Creo 结果](g17_outer_cropped/creo/result.json)保留每个对象的报告入口。首次仅检查第一个导入零件的记录保留在形状候选 `creo/`，补全结果在 `creo_all_components/`，两者没有混称整件通过。

## 下一步决定

保留原生原始 FaceId 采样、原曲线、精确边/双侧 pcurve、固定容差与拒绝 Apply 的门禁。保留本轮平面投影和低阶公平拟合作为面内初值及对照。

优先替换“把整个外轮廓和人工缝都固化成矩形等参边后直接 Coons 填充”的初始化与边界适配层。下一轮应把原始外边及外侧 G1 当硬要求，单独处理约 0.03～0.05 mm 的窄边界过渡带，验证边界贴合参数域/局部非均匀节点或有限边界带布局。人工内缝继续允许移动；不要再靠全局罚权暴涨消化局部约束。支撑域正则必须同时考虑有数据与无数据区域，不能只验裁剪外观。

本轮没有证明四片表示不可能，也没有验证新的边界适配方案。收益预期是保留当前面内改善同时找回闭合与外侧切向；代价可能是局部增加 knot、重新布局或少量增片，须以真实 STEP 复杂度和原边保持重新比较。

ScaleUntrim 只做了论文及源码筛选，未声称复现。其论文是一条全模型三角化、QuadriFlow 布局、Floater 参数化、逐片 B-spline 拟合的流程；论文约 1e−3 的误差是相对包围盒量，不能当 0.001 mm 绝对边界保证。[论文第 2、6、8 节](https://arxiv.org/html/2410.14318v1)。本地源码提交 `39fabf73242879185b5e2c6115c2640d2be3bf06` 的 `merge_fit.cpp`、`multi_patch_fit.cpp`、`GenerateShell.cpp` 确认它覆盖拟合和 STEP 输出，非单独参数化器；但未见与本项目原边/相邻面约束等价的局部替换合同。[源码](https://github.com/weixd07/ScaleUntrim/tree/39fabf73242879185b5e2c6115c2640d2be3bf06)。本轮已有无翻转、低畸变参数域，故没有把时间花在接入整套全模型布局依赖。

## 复现与验证边界

工具源码位于 `tools/candidate179_research/`，不改变 `src/reduction/region_refit/` 或产品 CMake。运行环境为 Python {platform.python_version()}、OCP {dependencies['cadquery-ocp']}、NumPy {dependencies['numpy']}、SciPy {dependencies['scipy']}；精确依赖见 [dependencies.json](dependencies.json) 与源码 `requirements.txt`。本轮复用现有环境，没有安装新的产品依赖。

在本实验工作树执行，选择新的输出目录：

```powershell
./tools/candidate179_research/run.ps1 `
  -PythonExe '{sys.executable}' `
  -SourceStep '{audit['source']}' `
  -BaselineDirectory '{baseline}' `
  -OutputDirectory 'data/runs/<新的运行号>' `
  -CreoRoot 'E:\\Proe\\Creo 11.0.0.0'
```

省略 CreoRoot 可运行全部 OCCT 部分，并将外部 CAD 标为未运行。各 Python 阶段均可独立调用 `--help`；本轮实际执行过逐阶段命令和 Creo 工具。汇总 PowerShell 入口已做语法检查，未再次整套重跑。汇总脚本不生成文件校验摘要。

两项独立解析几何测试通过：已知抛物面法向偏移的最近点，以及随机 B-spline 的值/法向与 OCCT 对照。真实基线投影在固定与最坏样本上和 OCCT 独立查询一致，最大差在浮点舍入量级；反向误差也做了全原始 B-Rep 抽查。未修改产品代码，未重跑无关的 C++ 全套回归，49→46 与 12 区域回归没有被重新宣称通过。

{sampling_note}

八次拟合的阶段计时合计 {sum(r['seconds'] for r in rows):.2f} 秒；拟合阶段不包括全部预处理、审计和 CAD 检查，不能拿它与旧运行约 794 秒的端到端时间宣称加速。独立整件缝合、导出、回读约 14 秒/候选；各阶段实际计时、日志及 Creo 起止时间均在 JSON 中。峰值内存未测量，不填零，也不声称满足原 C++ 产品性能验收。

本轮修正了三个实证工具问题并保留证据：用 Shell 保存共享边，避免 loose-face compound 写出后断开；最近点迭代按实际 UV 位移收敛；显式 UTF-8 读取含中文路径的 JSON。首次导出、原始求值记录与图在 `initial_export_diagnostic/`，不作为最终指标。`g17_data_cropped` 证明仅裁小支撑域不足以修复外推，失败支撑面保留。
'''
    (out/'RUN.md').write_text(report,encoding='utf-8')
    # Snapshot only these research tools; preserve original project state elsewhere.
    with zipfile.ZipFile(out/'code_snapshot.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((repo/'tools/candidate179_research').iterdir()):
            if file.is_file() and file.suffix in ['.py','.ps1','.txt','.md']:
                archive.write(file,'tools/candidate179_research/'+file.name)
    with zipfile.ZipFile(out/'review_bundle.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(out.rglob('*')):
            if not file.is_file():continue
            relative=file.relative_to(out)
            if any(part in ('inputs','verification_prepare','initial_export_diagnostic') for part in relative.parts):continue
            if any(part in ('creo','creo_all_components') for part in relative.parts) and file.name!='result.json':continue
            if file.name in ('review_bundle.zip','whole_model_diagnostic.step'):continue
            if file.suffix in ('.json','.md','.csv','.npz','.png','.pdf','.step','.zip','.log'):
                archive.write(file,str(relative))
        for name in ['trimmed_patch_faces.step','untrimmed_support_surfaces.step']:
            archive.write(baseline/name,'baseline/'+name)
    print(f'Research completed; product rejected. {out / "RUN.md"}')


if __name__=='__main__':main()
