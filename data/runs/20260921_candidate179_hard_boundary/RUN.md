# candidate-179：严格锁界与水密优先补充实证

2026-09-21 归档整理：本目录保留当轮协议、指标及失败记录；旧候选几何、重复图片、日志、Creo 临时资源和 ZIP 已按用户要求清理。以下结论描述当轮实测，当前几何入口见 [最新策略](../20260921_candidate179_designer_layout/RUN.md)。源码及精简结果随独立研究分支保存，发布状态以 Git 历史为准。

可以严格保留原外边界，同时优化四片内部。这一补充实验已经实际做到：两种候选都在固定 `0.001 mm` 缝合、STEP 导出及回读后得到 **1 个有效 Solid、0 条自由边、0 条多重边**。原始整件 2739 面，替换后 2694 面；研究区域仍为四面。没有扩大边/面容差，也没有补洞面。完整几何验收仍未通过，均为 `can_apply=false`。

用户明确整体水密优先后，前轮移动外边界的 446 控制点版本只保留为自由重建对照，不能充当工程主候选。后续顺序为：原边界硬保持与闭合先成立；随后解决形状、相邻连续性和褶皱；最后在这些条件内压缩复杂度。水密作为前置要求，不代表取消其余质量门。

## 指标与当前产物

- [当轮指标表](comparison.csv)、[机器记录](run.json)、[外边审计](boundary_audit.json)。
- [当前策略、原始区域及基线对照](../20260921_candidate179_designer_layout/RUN.md)。

## 为什么基线能闭合、如何严格保留

本轮重新读取指定基线与原始整件，按原 FaceId 删除 49 面并放入四片，原公差缝合及 STEP 回读仍得到一个有效实体。证据为 [基线闭合复核](baseline_control/integration.json)。因此基线闭合不是仅沿用旧报告的判断。

核对四片的所有裁剪边后，确认它们全部位于各自支撑面的矩形等参边界。对于这四张夹持 B-spline，冻结最外圈控制点就保留整条边界曲线；不是仅给有限边界样本增加罚权。实现上深拷贝原四片及共享拓扑，只沿固定 PCA 法向改变内部控制点，保留 knot、平面投影、三维边曲线及 pcurve。`hard_c0` 消去一圈边界变量；`hard_c1` 消去两圈，连旧一阶导数一起保留。后者名称指冻结已有导数，**不表示两侧已经达到 C1/G1**。

38 次边使用的 Curve-on-Surface/SameParameter 检查全部通过；整条矩形边界每侧 1001 点变化为零，原始 26 条外边又以每边 385 点独立检查，最大原边落面残差约 6.41e-12 mm，仅表明数值一致性。闭合同时需要正确的边配对、方向及双侧曲线落面，不能仅凭轮廓看起来重合。OCCT 控制点修改接口依据见 [Geom_BSplineSurface](https://occt3d.com/dev/doc/refman/html/class_geom___b_spline_surface.html)。

## 同输入结果

沿用首轮从原始 STEP 取出的 12000 个训练点、18000 个评估点及种子，不以基线内部作为形状真值。高度位置项、原始法向对应的梯度项、物理 X/Y 坐标中的二阶公平项联合求解。两个配置的所有权重相同，唯一对照因素是冻结一圈或两圈。位置和导数单位分别为 mm 与无量纲，协议系数及来源见 [protocol.json](protocol.json)。这不是独立模型测试集。

| 配置 | 正向最大 mm | 正向 RMS mm | 法向 P95 / 最大 ° | 人工接缝最大法向差 ° | Solid / 自由边 |
|---|---:|---:|---:|---:|---:|
| baseline | 0.07679 | 0.02159 | 13.58 / 65.30 | 18.37 | 1 / 0 |
| hard_c0 | 0.02260 | 0.00206 | 5.65 / 38.13 | 65.27 | 1 / 0 |
| hard_c1 | 0.04469 | 0.00550 | 10.81 / 65.25 | 18.37 | 1 / 0 |

反向 6000 点面积采样的最大偏差分别为 `hard_c0` 0.02392 mm、`hard_c1` 0.04403 mm。固定与最坏样本均用 OCCT 独立查询交叉核验。最大值都是采样值，不是连续域上界。

三组均为 **4 个实际 STEP Face、9645 个实际控制点**，单方向最多 125。未为硬边界降低或改写原 25 控制点方向预算；这次明确保留高复杂度验证硬约束可行性，因此复杂度门也没有通过。没有宣称同时解决低复杂度表达。

## 水密通过以后仍失败的项目

`hard_c0` 的正反向距离均低于原 0.03 mm 门，但最大法向 38.13° 高于 15°，人工缝 65.27° 高于 5°。`hard_c1` 的距离与最大法向仍失败；它保留基线人工缝约 18.37° 的问题，而非解决它。

对 14 条原来要求 G1 的外边，基线与外侧原面最大法向差为 67.63°；`hard_c0` 为 59.87°，`hard_c1` 为 67.63°。原始区域对应关系约 0.013°，见首轮原始边审计。**基线保住了外边位置，却没有保住正确外侧切向。** 因此把旧导数全部锁住会锁住既有错误。现有内部 5°门不被擅自移用为外侧新门，但这些角度明确不能称为保留工程 G1。

连续支撑面仍采用原平面投影，优化只改变高度。各 patch 原稠密 knot 跨区采样中没有新增 Jacobian 退化或投影翻转。已核实的网格投影、支撑差分、Face/Wire、边曲线及整件拓扑分别记录；没有将其统称为一种翻转。原生 StrictTopologyGate/Apply 未执行，不能把独立闭合检查称为产品门全部通过。

## 实际 Creo 检查

- `hard_c0`：ReportReady，ModelCHECK 通过=False；`candidate.prt.1`：GEOM_CHECKS=1, SHORT_EDGES=1504。
- `hard_c1`：ReportReady，ModelCHECK 通过=False；`candidate.prt.1`：GEOM_CHECKS=1, SHORT_EDGES=1502。

已经完成的导入生成单一 `candidate.prt.1`，不再是前轮两块分离零件及装配。但单一零件、非零体积或导入校验分数并不等价于 ModelCHECK 全通过；完整检查见各候选 `creo/result.json` 及其指向的 XML。未更改 Creo 模板、精度与 ModelCHECK 配置；短边数包含原模型问题，不能全部归因于本次改动，也不能把模板的长度单位直接当作项目毫米门。

## 结论与下一层决定

严格锁界没有阻止实质性的面内误差下降，也没有阻止四片闭合成实体。前轮“先自由重建再设法缝回”的结果不能代替这条工程路线。现在保留硬锁位置的求解结构，下一步在同一个闭合可行域内联合求解原外侧与人工接缝的导数；允许内部曲线作为变量时，必须双侧共享并保持外轮廓固定。

两组结果说明需要区分“位置必须不动”和“旧导数是否正确”。不要继续把旧 Coons 导数作为正确连续性的硬目标，也不要靠增加边界罚权代替等式约束。knot/控制点压缩在水密和几何质量成立后进行；必要时再改变局部参数域或布局。本轮没有证明当前高控制点一定不可压缩，也没有证明低复杂度四片一定不可行。

## 实现、修正与复现

仍在独立工作树 `D:\pyProject\step-patch-optimizer-worktrees\candidate179-evidence-20260921`；源码起点为 `bfe1412`，指定几何运行仍是 `20260729_032156_431`，其原始 dirty 运行源码不能等同于归档提交。没有修改 C++ 产品源码、原始模型、指定基线或首轮结果，实验执行时尚未提交；本次精简结果随研究分支归档，未合并主线。

新增 `hard_boundary.py`、`hard_boundary_audit.py`、`test_hard_boundary.py` 和本汇总脚本；渲染工具仅增加可选页脚，默认行为保持。Python/OCP/SciPy 环境沿用首轮，见 [dependencies.json](dependencies.json)。弯曲坐标中的解析二次曲面梯度/Hessian测试通过；原两项数值测试也通过。

初次参数反求有 3 个点陷入局部迭代，包含 1 个训练点。独立 OCCT 查询找到了精确参数；已加异常点回退，30000 点平面反求全部闭合到数值精度。两个相同配置因此各重跑一次，旧批次的 JSON 指标保留在 `initial_parameterization_diagnostic/`，见 [协议补充](protocol_amendment.json)。没有删除样本、改变采样或修改验收阈值。正式指标只来自修正后的批次。

复现时将本目录的 `protocol.json`、`input_audit.json`，以及最新运行目录 `../20260921_candidate179_designer_layout/` 中的 `reference.npz`、`original_region.step` 复制到新的输出目录；`protocol.json` 中的前轮与基线相对路径从仓库根解析，`input_audit.json` 的原始整件路径需要可访问。随后从本工作树执行（`$run` 指新的输出目录）：

```powershell
$py = 'D:\pyProject\3d_engineering\astra_modeling_test\.venv\Scripts\python.exe'
$run = 'data/runs/<新的运行号>'
$base = 'data\runs\20260729_candidate_0179_geomagic_reverse_optimization_visualization\final\evidence\previous_full_diagnostic_20260729_032156'
& $py tools/candidate179_research/test_hard_boundary.py
& $py tools/candidate179_research/hard_boundary.py --run $run --baseline $base
& $py tools/candidate179_research/hard_boundary_audit.py --run $run --baseline $base
& $py tools/candidate179_research/integrate.py --source 'D:\pyProject\step-patch-optimizer\data\stp\03_配件_Clay.stp' --run $run --names hard_c0 hard_c1
./tools/candidate179_research/run_creo.ps1 -StepPath "$run/hard_c0/whole_model_diagnostic.step" -OutputDir "$run/hard_c0/creo" -CreoRoot 'E:\Proe\Creo 11.0.0.0'
./tools/candidate179_research/run_creo.ps1 -StepPath "$run/hard_c1/whole_model_diagnostic.step" -OutputDir "$run/hard_c1/creo" -CreoRoot 'E:\Proe\Creo 11.0.0.0'
```

以上为当轮实际执行的命令。整理后保留协议和指标，原始区域与共享样本统一放在最新运行目录；当轮几何、日志、ZIP 和 Creo 临时文件已清理。历史图像由实际 STEP 渲染，当前只保留最新策略与原始、基线的对比图。复现必须使用新的输出目录。
