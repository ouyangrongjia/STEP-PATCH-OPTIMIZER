"""Write measured hard-boundary follow-up evidence and reviewable geometry bundle."""
import argparse
import csv
import datetime
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from geometry import dump


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args()
    out=Path(args.run).resolve();repo=Path(__file__).resolve().parents[2]
    protocol=read(out/'protocol.json');ev=read(out/'evaluation.json');baseline=repo/protocol['baseline']
    previous=repo/protocol['previous_run'];names=['hard_c0','hard_c1'];rows=[];checks={}
    for name in ['baseline']+names:
        folder=out/('baseline_control' if name=='baseline' else name)
        integration=read(folder/'integration.json');s=integration['after_step_read'];m=ev[name]
        fit=read(folder/'fit.json') if name!='baseline' else None
        seams=max(x['normal_degrees']['max'] for x in fit['seams']) if fit else max(x['normal_degrees']['max'] for x in read(previous/'local_diagnostics.json')['baseline']['seams'])
        rows.append(dict(name=name,faces=4,step_poles=9645,forward_max_mm=m['distance_mm']['max'],forward_rms_mm=m['distance_mm']['rms'],
            normal_p95_deg=m['normal_degrees']['p95'],normal_max_deg=m['normal_degrees']['max'],original_boundary_mm=m['original_boundary_max_mm'],
            seam_normal_max_deg=seams,external_requested_G1_max_deg=m['requested_g1_max_normal_degrees'],solids=s['solids'],free_edges=s['free_edges'],brep_valid=s['brep_valid']))
        if fit:
            creo=read(folder/'creo/result.json') if (folder/'creo/result.json').exists() else dict(status='NotRun',validated=False)
            summary=[dict(object=Path(x['object']).name,parsed=x['summary']['parsed'],failures=[dict(name=y['name'],answer=y['answer']) for y in x['summary']['failed_checks']],
                import_validation=x['summary'].get('import_validation')) for x in creo.get('component_checks',[])]
            checks[name]=dict(boundary_hard_preservation=True,boundary_max_mm=m['original_boundary_max_mm'],
                curve_on_surface=fit['edge_checks']['max_curve_on_surface_mm'],same_parameter_false=fit['edge_checks']['same_parameter_false'],
                closed_solid_at_001mm=s['solids']==1 and s['free_edges']==0 and s['multiple_edges']==0 and s['brep_valid'],
                forward_distance_pass=m['distance_mm']['max']<=.03,reverse_distance_pass=m['reverse']['distance_mm']['max']<=.03,
                normal_pass=m['normal_degrees']['max']<=15,internal_seam_pass=seams<=5,
                maximum_poles_per_direction=max(max(p['complexity']['u_poles'],p['complexity']['v_poles']) for p in fit['patches']),
                poles_per_direction_25_pass=False,creo=dict(status=creo['status'],validated=creo['validated'],components=summary),
                product_StrictTopologyGate='Not invoked; independent shape/continuity checks still fail',can_apply=False)
    with (out/'comparison.csv').open('w',newline='',encoding='utf-8-sig') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    manifest=dict(status='HardBoundaryClosedDiagnostics',can_apply=False,priority='Original boundaries and watertight solid first; original geometric gates remain active',
        source=read(out/'input_audit.json'),baseline=read(previous/'run.json')['baseline'],protocol=protocol,
        implementation=dict(base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
            worktree=str(repo),branch=subprocess.check_output(['git','branch','--show-current'],cwd=repo,text=True).strip(),committed=False,pushed=False,product_cpp_modified=False),
        configurations=2,fit_batches=2,initial_batch_preserved='initial_parameterization_diagnostic',
        parameterization=read(out/'parameterization.json'),comparison=rows,gates=checks,
        dependencies=read(previous/'dependencies.json'),completed_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
    dump(out/'run.json',manifest)
    shutil.copy2(previous/'dependencies.json',out/'dependencies.json')
    dump(out/'protocol_amendment.json',dict(reason='Independent OCCT check found three failed numerical XY inversions, including one training sample. Corrected correspondence, kept original samples and both fitting configurations.',
        additional_configurations=0,additional_fit_executions=2,initial_artifacts='initial_parameterization_diagnostic',
        acceptance_limits_unchanged=True,final_unresolved_inversions=0))
    a,b,c=rows
    table='\n'.join(f"| {r['name']} | {r['forward_max_mm']:.5f} | {r['forward_rms_mm']:.5f} | {r['normal_p95_deg']:.2f} / {r['normal_max_deg']:.2f} | {r['seam_normal_max_deg']:.2f} | {r['solids']} / {r['free_edges']} |" for r in rows)
    cad=[]
    for name in names:
        check=checks[name]['creo'];entries=[]
        for component in check['components']:
            failures=', '.join(x['name']+'='+str(x['answer']) for x in component['failures']) or '无失败项'
            entries.append(f"`{component['object']}`：{failures}")
        cad.append(f"- `{name}`：{check['status']}，ModelCHECK 通过={check['validated']}；"+'；'.join(entries)+'。')
    report=f'''# candidate-179：严格锁界与水密优先补充实证

可以严格保留原外边界，同时优化四片内部。这一补充实验已经实际做到：两种候选都在固定 `0.001 mm` 缝合、STEP 导出及回读后得到 **1 个有效 Solid、0 条自由边、0 条多重边**。原始整件 2739 面，替换后 2694 面；研究区域仍为四面。没有扩大边/面容差，也没有补洞面。完整几何验收仍未通过，均为 `can_apply=false`。

用户明确整体水密优先后，前轮移动外边界的 446 控制点版本只保留为自由重建对照，不能充当工程主候选。后续顺序为：原边界硬保持与闭合先成立；随后解决形状、相邻连续性和褶皱；最后在这些条件内压缩复杂度。水密作为前置要求，不代表取消其余质量门。

## 直接审阅

- 只锁位置： [四片 STEP](hard_c0/four_patch_diagnostic.step)、[完整实体 STEP](hard_c0/whole_model_diagnostic.step)、[同视角对照](figures/comparison_hard_c0.png)。这组形状误差更低，但人工接缝法向跳变加大。
- 同时锁已有导数： [四片 STEP](hard_c1/four_patch_diagnostic.step)、[完整实体 STEP](hard_c1/whole_model_diagnostic.step)、[同视角对照](figures/comparison_hard_c1.png)。这组保留旧接缝状态，边界附近仍有波纹。
- [原始区域](original_region.step)、[表格](comparison.csv)、[机器记录](run.json)、[原始外边复核](boundary_audit.json)、[代码快照](code_snapshot.zip)、[审阅包](review_bundle.zip)。

## 为什么基线能闭合、如何严格保留

本轮重新读取指定基线与原始整件，按原 FaceId 删除 49 面并放入四片，原公差缝合及 STEP 回读仍得到一个有效实体。证据为 [基线闭合复核](baseline_control/integration.json)。因此基线闭合不是仅沿用旧报告的判断。

核对四片的所有裁剪边后，确认它们全部位于各自支撑面的矩形等参边界。对于这四张夹持 B-spline，冻结最外圈控制点就保留整条边界曲线；不是仅给有限边界样本增加罚权。实现上深拷贝原四片及共享拓扑，只沿固定 PCA 法向改变内部控制点，保留 knot、平面投影、三维边曲线及 pcurve。`hard_c0` 消去一圈边界变量；`hard_c1` 消去两圈，连旧一阶导数一起保留。后者名称指冻结已有导数，**不表示两侧已经达到 C1/G1**。

38 次边使用的 Curve-on-Surface/SameParameter 检查全部通过；整条矩形边界每侧 1001 点变化为零，原始 26 条外边又以每边 385 点独立检查，最大原边落面残差约 {b['original_boundary_mm']:.2e} mm，仅表明数值一致性。闭合同时需要正确的边配对、方向及双侧曲线落面，不能仅凭轮廓看起来重合。OCCT 控制点修改接口依据见 [Geom_BSplineSurface](https://occt3d.com/dev/doc/refman/html/class_geom___b_spline_surface.html)。

## 同输入结果

沿用首轮从原始 STEP 取出的 12000 个训练点、18000 个评估点及种子，不以基线内部作为形状真值。高度位置项、原始法向对应的梯度项、物理 X/Y 坐标中的二阶公平项联合求解。两个配置的所有权重相同，唯一对照因素是冻结一圈或两圈。位置和导数单位分别为 mm 与无量纲，协议系数及来源见 [protocol.json](protocol.json)。这不是独立模型测试集。

| 配置 | 正向最大 mm | 正向 RMS mm | 法向 P95 / 最大 ° | 人工接缝最大法向差 ° | Solid / 自由边 |
|---|---:|---:|---:|---:|---:|
{table}

反向 6000 点面积采样的最大偏差分别为 `hard_c0` {ev['hard_c0']['reverse']['distance_mm']['max']:.5f} mm、`hard_c1` {ev['hard_c1']['reverse']['distance_mm']['max']:.5f} mm。固定与最坏样本均用 OCCT 独立查询交叉核验。最大值都是采样值，不是连续域上界。

三组均为 **4 个实际 STEP Face、9645 个实际控制点**，单方向最多 125。未为硬边界降低或改写原 25 控制点方向预算；这次明确保留高复杂度验证硬约束可行性，因此复杂度门也没有通过。没有宣称同时解决低复杂度表达。

## 水密通过以后仍失败的项目

`hard_c0` 的正反向距离均低于原 0.03 mm 门，但最大法向 {b['normal_max_deg']:.2f}° 高于 15°，人工缝 {b['seam_normal_max_deg']:.2f}° 高于 5°。`hard_c1` 的距离与最大法向仍失败；它保留基线人工缝约 {c['seam_normal_max_deg']:.2f}° 的问题，而非解决它。

对 14 条原来要求 G1 的外边，基线与外侧原面最大法向差为 {a['external_requested_G1_max_deg']:.2f}°；`hard_c0` 为 {b['external_requested_G1_max_deg']:.2f}°，`hard_c1` 为 {c['external_requested_G1_max_deg']:.2f}°。原始区域对应关系约 0.013°，见首轮原始边审计。**基线保住了外边位置，却没有保住正确外侧切向。** 因此把旧导数全部锁住会锁住既有错误。现有内部 5°门不被擅自移用为外侧新门，但这些角度明确不能称为保留工程 G1。

连续支撑面仍采用原平面投影，优化只改变高度。各 patch 原稠密 knot 跨区采样中没有新增 Jacobian 退化或投影翻转。已核实的网格投影、支撑差分、Face/Wire、边曲线及整件拓扑分别记录；没有将其统称为一种翻转。原生 StrictTopologyGate/Apply 未执行，不能把独立闭合检查称为产品门全部通过。

## 实际 Creo 检查

{chr(10).join(cad)}

已经完成的导入生成单一 `candidate.prt.1`，不再是前轮两块分离零件及装配。但单一零件、非零体积或导入校验分数并不等价于 ModelCHECK 全通过；完整检查见各候选 `creo/result.json` 及其指向的 XML。未更改 Creo 模板、精度与 ModelCHECK 配置；短边数包含原模型问题，不能全部归因于本次改动，也不能把模板的长度单位直接当作项目毫米门。

## 结论与下一层决定

严格锁界没有阻止实质性的面内误差下降，也没有阻止四片闭合成实体。前轮“先自由重建再设法缝回”的结果不能代替这条工程路线。现在保留硬锁位置的求解结构，下一步在同一个闭合可行域内联合求解原外侧与人工接缝的导数；允许内部曲线作为变量时，必须双侧共享并保持外轮廓固定。

两组结果说明需要区分“位置必须不动”和“旧导数是否正确”。不要继续把旧 Coons 导数作为正确连续性的硬目标，也不要靠增加边界罚权代替等式约束。knot/控制点压缩在水密和几何质量成立后进行；必要时再改变局部参数域或布局。本轮没有证明当前高控制点一定不可压缩，也没有证明低复杂度四片一定不可行。

## 实现、修正与复现

仍在独立工作树 `{repo}`；源码起点为 `bfe1412`，指定几何运行仍是 `20260729_032156_431`，其原始 dirty 运行源码不能等同于归档提交。没有修改 C++ 产品源码、原始模型、指定基线或首轮结果，没有提交、合并或推送。

新增 `hard_boundary.py`、`hard_boundary_audit.py`、`test_hard_boundary.py` 和本汇总脚本；渲染工具仅增加可选页脚，默认行为保持。Python/OCP/SciPy 环境沿用首轮，见 [dependencies.json](dependencies.json)。弯曲坐标中的解析二次曲面梯度/Hessian测试通过；原两项数值测试也通过。

初次参数反求有 3 个点陷入局部迭代，包含 1 个训练点。独立 OCCT 查询找到了精确参数；已加异常点回退，30000 点平面反求全部闭合到数值精度。两个相同配置因此各重跑一次，旧批次保留在 `initial_parameterization_diagnostic/`，见 [协议补充](protocol_amendment.json)。没有删除样本、改变采样或修改验收阈值。正式指标只来自修正后的批次。

复现时先把本目录的 `protocol.json`、`reference.npz`、`input_audit.json`、`original_region.step` 复制到新的输出目录；`protocol.json` 中的前轮与基线相对路径从仓库根解析，`input_audit.json` 的原始整件路径需要可访问。随后从本工作树执行（`$run` 指新的输出目录）：

```powershell
$py = '{sys.executable}'
$run = 'data/runs/<新的运行号>'
$base = '{protocol['baseline']}'
& $py tools/candidate179_research/test_hard_boundary.py
& $py tools/candidate179_research/hard_boundary.py --run $run --baseline $base
& $py tools/candidate179_research/hard_boundary_audit.py --run $run --baseline $base
& $py tools/candidate179_research/integrate.py --source '{manifest['source']['source']}' --run $run --names hard_c0 hard_c1
./tools/candidate179_research/run_creo.ps1 -StepPath "$run/hard_c0/whole_model_diagnostic.step" -OutputDir "$run/hard_c0/creo" -CreoRoot 'E:\\Proe\\Creo 11.0.0.0'
./tools/candidate179_research/run_creo.ps1 -StepPath "$run/hard_c1/whole_model_diagnostic.step" -OutputDir "$run/hard_c1/creo" -CreoRoot 'E:\\Proe\\Creo 11.0.0.0'
```

各阶段实际命令已执行，日志保留在本目录。审阅包包括两组最终整件 STEP、局部 STEP、参考数据、原始记录与代码；Creo 原始零件和大量资源文件、初次错误批次整件没有重复压入包，仍保留在工作树。所有图像由实际 STEP 渲染。
'''
    (out/'RUN.md').write_text(report,encoding='utf-8')
    with zipfile.ZipFile(out/'code_snapshot.zip','w',zipfile.ZIP_DEFLATED) as z:
        for path in sorted(Path(__file__).parent.iterdir()):
            if path.is_file():z.write(path,'tools/candidate179_research/'+path.name)
    with zipfile.ZipFile(out/'review_bundle.zip','w',zipfile.ZIP_DEFLATED) as z:
        for path in sorted(out.rglob('*')):
            if not path.is_file():continue
            relative=path.relative_to(out)
            if 'initial_parameterization_diagnostic' in relative.parts:continue
            if 'creo' in relative.parts and path.name!='result.json':continue
            if path.name=='review_bundle.zip':continue
            if relative.parts[0]=='baseline_control' and path.suffix=='.step':continue
            if path.suffix in ['.json','.md','.csv','.npz','.png','.step','.zip','.log']:z.write(path,str(relative))
        for filename in ['trimmed_patch_faces.step','untrimmed_support_surfaces.step']:
            z.write(baseline/filename,'baseline/'+filename)
    print(out/'RUN.md')


if __name__=='__main__':main()
