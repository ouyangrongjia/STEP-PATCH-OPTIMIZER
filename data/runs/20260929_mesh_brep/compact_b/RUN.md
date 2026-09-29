# candidate-179：紧凑 C2 网格重建

本轮按用户后续要求只研究 candidate-179。唯一用户入口为 [最终目录](../candidate179_B_final/README.md)。保留的 B 后端是 `tools.mesh_brep.compact`：给定五片离散布局，STL 经索引、网格参数化、三维联合拟合后直接生成五张 C2 样条面。原接口挂接和整件装配在之后单独执行，均不修改这五张支撑曲面的控制点。

局部 4290 个实际支撑控制点，Creo 实际往返仍为 5 面；整件 STEP 往返为有效 Solid，Creo 往返为 1 个 Solid、零非退化自由边。当前 Creo GEOM_CHECKS=0。原邻面 G1、单方向控制点预算、局部曲率峰值及 ModelCHECK 短边项尚未全部解决，保持 **DiagnosticOnly / can_apply=false**。逐项数值的唯一汇总是最终目录的 `metrics.json`、`comparison.csv` 与 `timing.csv`，此处记录方法、过程限制和复现证据。

## 数据权限与预先约定

起点为隔离分支 `experiment/mesh-brep-20260929` 的 `16b199c`。主仓库的 `experiment/stl-structural-partition` 及未提交修改没有改动。未推送、未合并。原四片、aligned_inner_cubic 五片和上一版 B 均保留。

`protocol.json` 是尝试前的原协议，未重写其失败假设：先试“浮动裁剪边界与低密度支撑解耦”，预定最多 3 个候选、90 分钟墙钟。实际考察了 3 类表示，但包含修错、两档分辨率和重复运行后，**执行次数超过原 3 次上限**：`executions.json` 记录 10 次完成的顶层重建，总计 208.901 秒，另有第一次失败、隔离复跑、最终入口复跑和外部检查。不能将 3 类表示说成仅执行了 3 次。第一次失败没有完整墙钟记录，明确为 N/A。模型选型在最终独立 CAD 评估前冻结于 `selection.json`；后续 pcurve 压缩不改变支撑曲面。

预先距离 0.03 mm、法向 15°、要求光顺的接缝 5°、原接口落面 0.001 mm、内部位置 1e-7 mm、单方向控制点目标 25、Creo 局部面数目标 5。网格后端构造公差 1e-7 mm，整件缝合仍为 0.001 mm，不累加局部公差。B 允许在此范围内逼近旧接口，旧版数学精确复制边界不再是拟合条件。

| 阶段 | 允许的数据 | 实际入口 / 证据 |
|---|---|---|
| 网格生成 | 原始 CAD 区域与声明的布局规则 | `input_refined/generation.json`；只生成离散 STL、标签、四角索引、边链 |
| B 重建 | STL、单位、公差、离散标签/角点/边链 | `compact13_refined/reconstruction.json`；无 CAD 曲线、UV、精确法向、旧控制网 |
| 接口挂接 | 已冻结的五片支撑与原区域外曲线 | `attached_final/attachment.json`；只生成 pcurve 和拓扑 |
| 整件拼接 | 原整件余面及新局部 | `sewn_final/assembly.json`；替换原 49 面 |
| 最终确认 | 独立原 CAD 几何及新 STEP | `confirmation_final.json`、`interfaces_final.json`；不反馈给拟合 |

输入生成器的全局网格图只用于落实给定五片划分，不将该 UV 或旧 XY 映射传入 B。STL 重建按完全相同的 float32 XYZ 建索引，不进行近邻焊接；本例网格为单连通盘，非流形、绕向冲突及退化三角形均为 0。接口两侧并非任意锐边法向混合，本原型只接受声明为光顺的内部边。没有恢复任意分区搜索，没有使用高度场或论文方法二。

## 保留的算法与实现修正

1. 每片使用真实邻接的 mean-value 参数化；共享链统一弧长参数。Coons 初始化的四边方向重新映射到网格边界后，再解一次内部 UV。三维网格三角形与 UV 用重心插值对应，目标点始终取离散三角面，不投回 CAD。
2. 四条外链用 FITPACK 三次平滑曲线自适应选简单节点，RMS 预算为输入接口公差的 1/10。该 RMS 条件本身不是最大误差保证，后续另查最大边界误差。径向及中央边链各保留 13 控制点，径向简单节点按平方分布集中于弯曲窄带；中央面 13×13。
3. 不同曲面节点空间通过精确 B-spline 基底转换共享一条接缝的变量。固定外链，内部三维位置、网格法向、二阶公平项联合求解。法向权重 0.01、公平权重 1e-5、接缝切平面权重 1，逐节点区间四点 Gauss 积分。共同角点切平面为硬约束，重复约束用秩揭示 QR 去冗余，并验证全部原约束残差。
4. 原接口挂接按旧曲线参数逐点投影到新外链，拟合非线性 pcurve，避免强行使用旧仿射参数。385 点建立对应，1537 点/边检查，随后 STEP 回读及独立原邻面检查。参数曲线压缩后控制点从首次 8822 降至 1308，支撑系数不变。B_local 最大原接口误差约 0.000575 mm，未扩大 0.001 mm 容差。
5. 已修正法向估计与验证中的两个实现问题：近共线密集点的无条件二次局部拟合曾偏离三角面法向 86.85°，当前使用按真实邻接、面积加权的一环法向；Creo 自由边审计最初把 54 条退化边误计为开口，最终显式区分退化边、闭合自身缝和真实自由边。初次报告保留，正式结果取 `creo_face_audit_final/audit.json`。

最终曲面网格为 78×13、49×13、99×13、91×13、13×13，4290 支撑控制点，10836 独立 XYZ 标量变量。旧 B 为 13375 点；其外部 CAD 重复节点传入曲面，存在 C0/C1 节点及 Creo 拆面。当前全部面内节点为简单三次节点，具有 C2 连续性；C2 不能保证低曲率或面间 G1，仍按实际法向和曲率测量。

实现依据：[SciPy FITPACK 平滑曲线参数与残差定义](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.splprep.html)、[OCCT B-spline 曲面接口](https://occt3d.com/dev/doc/refman/html/class_geom___b_spline_surface.html)。失败的单片实验参考 [LSCM 原论文](https://www.cs.tau.ac.il/~dcor/Graphics/papers/s2002_lscm.pdf)。本轮没有安装或升级依赖；沿用 Python 3.11、SciPy 1.17.1、NumPy 2.4.6、OCP 7.9.3.1.1、VTK 9.6.2 和 Creo 11。

## 两档分辨率和失败方案

| 方案 | 实际支撑点 | 观察与结论 |
|---|---:|---|
| 浮动外裁剪，五片 25×25 | 3125 | 首次 UV 更新翻转；全局阻尼被瘦长三角形限制。修为边界更新后重解内部图，12/25 次迭代仍约 0.002/0.0018 mm 外边残差，未满足 0.001 mm。停用。 |
| 单片 LSCM，41×41 | 1681 | 修正法向估计后网格距离最大 0.030574 mm、法向最大 50.62°，真实渲染边缘波纹明显。停用。 |
| 紧凑 C2，0.001 mm 网格 | 3848 | 网格 10730 点/20936 三角形；开发集双向最大 0.005402/0.005449 mm，内部接缝 3.167°。原接口边 3 为 0.001038 mm，超过原容差，拒绝挂接。 |
| 相同紧凑 C2，0.0002 mm 网格 | 4290 | 网格 37478 点/74207 三角形；同超参数开发集双向最大 0.003988/0.003964 mm，内部接缝 1.257°。原接口通过，选中。 |

`trim25*`、`conformal41*` 和 `compact13*` 的 JSON/日志保留失败原因与运行成本。`trim25_harmonic` 的最近点评估曾出现数值解与 OCCT 距离核对不一致，不能把该次候选的距离当作已验证结论。最终修正版本可由 `trimmed.py --controls 25 --iterations 25 --graded` 和 `conformal.py --controls 41` 复跑；原始错误迭代的日志仅用于定位，不将其伪装成最终源码的逐字节重放。

细化依据是独立的接口容差失败与网格信息量，未使用最终原 CAD 内部确认点调参。粗、细输入全由原区域重新网格化，而非采样已拟合曲面。参数图均无翻转；真实 UV 图和相同材质、相机、光照的 STEP 表面图已打开查看。曲面图使用真实 D1 法向，没有显示法向平滑；另绘所有节点线及每方向 11 条均匀参数线，并显示 Creo 实际面边。

## 独立确认、Creo 与尚未解决的问题

新旧 B 使用同一批 30000 个 CAD 确认点，seed=2910300；反向各 12000 点，seed=2910301，按相同面积规则采样。网格开发集为 12000/6000、seed=2910200；训练 seed=2910100、目标 36000 个面积样本按片取整。最终确认独立运行，没有复用距离缓存或缩减样本。误差最大值均为采样最大值，不是连续域上界。

新旧 B 的节点、法向目标、输入网格分辨率及外边约束都有改变，不能将收益单独归因于 UV。与 aligned_inner_cubic 的 4324 点规模相近，但不是相同自由度的 UV 消融。当前 CAD 双向距离约 0.00406/0.00398 mm，比上一版 0.00311/0.00341 mm 略增；内部接缝改善至 1.257°，CAD 最大法向改善至 5.686°。原邻面 G1 仍为 5.612°（旧原接口自身约 0.013°，这一差异为新增），方向控制点 99 仍超过 25。采样最大绝对平均曲率从 42.984 增至 159.422 /mm，不能声称全部光顺性改善。

原 STEP→网格、网格→原 STEP、网格↔曲面、原 CAD↔曲面的距离独立列出。检查覆盖 SameParameter、曲线落面、共享拓扑方向、UV 畸变、面内节点连续性、Jacobian 及两两曲面求交；未发现意外交线或采样奇异点，不构成单面全域自交证明。

`creo_final/result.json` 为实际批量导入/ModelCHECK，`creo_export/export.json` 为实际原生件导出，`creo_face_audit_final` 对导出的真实 B-Rep 实体计数。旧版 5 面被拆为 13 面，本版保持 5 面。新整件为 2695 面/1 Solid/零非退化自由边；原始整件及当前整件 GEOM_CHECKS 均为 0，短边数量为 1501/1487，检查阈值报告原值 0.5。仅总数下降不能证明全部剩余短边都与旧模型一一对应。

**单位复核修正：**两次 ModelCHECK 的绝对精度原值均为 0.004240749674，参数 `PTC_UNITS_LENGTH=in`；原输入与 Creo 导出 STEP 的 LENGTH_UNIT 均为 mm。报告保留原始值和单位参数，不把 0.004240749674 直接误写为 mm。没有改动 Creo 导入精度选项；这些结果不能称为 Creo 在 0.001 mm 绝对精度下的验收。[PTC 模型单位参数说明](https://support.ptc.com/help/creo/creo_pma/r13/usascii/fundamentals/fundamentals/model_units_parameters.html) 与 [ACCURACY_INFO 定义](https://support.ptc.com/help/creo/creo_pma/r12/usascii/model_analysis/modelcheck/accuracy_info.html) 仅用于核对报告语义，不代替实际本地检查。

局部为开放区域，没有任意封盖。闭合结论针对与原整件余面共同装配后的 B-Rep；不是只输入局部 STL 就恢复了整个原件。OCCT 写入前及 STEP 回读的整件有效性通过；Creo 再导出后的整件在本次只确认实体数与闭合拓扑，局部 BRepCheck 通过，未把它宣称为再次完整核验所有整件 OCCT 几何谓词。ModelCHECK 因短边未全通过，仍保留诊断状态。

## 数据隔离、耗时与测试

`isolation_final` 在 Windows 用独占文件句柄锁定 1881 个 CAD/缓存文件，覆盖本运行目录、主仓库原 STEP，以及 adaptive-layout 和 candidate179-continuity 两个工作树的运行档案。对子进程实际执行 Python 读取与原生 STEP 读取拒绝探针。完成重建时实际数据输入仅 `input/input.stl`、`input/layout.json`，意外读取为 0；与冻结五张面控制点数组的最大差均为 0 mm（`isolation_comparison.json`）。这是受审查后端的实际 OS 文件共享拒绝证据，不声称抵御恶意原生代码的安全沙箱。

| 阶段 | 本次秒数 |
|---|---:|
| 原区域 STEP 读取 / 网格及布局生成 | 0.096 / 7.832 |
| STL 索引 / 网格 UV / 边界和变量映射 | 0.120 / 0.418 / 0.054 |
| 对应关系 / 矩阵装配 / 线性求解 | 0.222 / 1.609 / 0.908 |
| 局部 B-Rep 写出 / 回读检查 | 0.030 / 0.081 |
| 纯重建首次 / 暖复跑 | 3.555 / 3.576 |
| 原接口挂接 / 整件装配读写及检查 | 0.889 / 25.259 |
| 最终独立确认 / 旧 B 同批确认 | 34.947 / 39.541 |
| 曲面对求交 / Creo 实际面审计 | 10.376 / 16.043 |
| 当前 Creo 导入检查 / 原始 Creo 导入检查 | 84.022 / 83.884 |
| 较早未压缩 pcurve 的 Creo 检查 / 当前 Creo STEP 导出 | 89.118 / 41.572 |
| 10 次顶层重建之和，含失败方案的完成运行及重复 | 208.901 |

各阶段可能嵌套，不能把本表各行直接相加为端到端耗时。OS 文件缓存没有清空，只能区分新进程首次与重复执行；并行验证和首次失败缺失的墙钟进一步限制速度结论。全部精确秒数、隔离及交付入口复跑成本见最终 `timing.csv`。没有宣称网格路线总体更快。

最终 `tests_final.log` 为 9 项网格后端测试通过，`retained_tests.log` 为 8 项既有 candidate-179 工具测试通过；新增回归验证不同接缝基底仍严格共享位置、固定外链不变，以及矛盾硬位置约束必须拒绝。`entry_replay.log` 是从交付目录 `reproduce.ps1` 实际启动的 STL→STEP 完整复跑，输出位于 `entry_replay/mesh`。

## 复现命令

从本工作树根目录使用具有已有依赖的 Python；所有输出目录必须不存在。实际 Python 为 `D:\pyProject\3d_engineering\astra_modeling_test\.venv\Scripts\python.exe`。

```powershell
$py='D:\pyProject\3d_engineering\astra_modeling_test\.venv\Scripts\python.exe'
$run='data/runs/20260929_mesh_brep'
$b="$run/compact_b"
& $py -B -m tools.mesh_brep.generate --kind cad --source "$run/baseline_cold/original_region.step" --deflection 0.0002 --output "$b/new_input"
& $py -B -m tools.mesh_brep.compact --stl "$b/new_input/input.stl" --layout "$b/new_input/layout.json" --output "$b/new_mesh"
& $py -B -m tools.mesh_brep.attach_boundary --mesh-result "$b/new_mesh" --region "$run/baseline_cold/original_region.step" --output "$b/new_interface"
& $py -B -m tools.mesh_brep.assemble_candidate --audit "$run/baseline_cold/input_audit.json" --patch "$b/new_interface/network.step" --output "$b/new_whole"
& $py -B -m tools.mesh_brep.evaluate --stl "$b/new_input/input.stl" --candidate "$b/new_interface/network.step" --source "$run/baseline_cold/original_region.step" --count 30000 --reverse-count 12000 --seed 2910300 --output "$b/new_confirmation.json"
& $py -B -m tools.mesh_brep.check_interface --reference "$run/baseline_cold" --mesh-result "$b/new_mesh" --candidate "$b/new_interface/network.step" --attachment "$b/new_interface/attachment.json" --output "$b/new_interfaces.json"
& $py -B -m tools.mesh_brep.extra_checks --candidate "$b/new_interface/network.step" --output "$b/new_intersections.json"
```

便捷纯网格复跑直接执行最终目录内 `reproduce.ps1 -Python $py -OutputDir <新目录>`；加 `-Assemble` 才启用 CAD 接口与整件组装。隔离重跑用 `tools.mesh_brep.isolate --backend compact`，按 `isolation_final/lock_manifest.json` 提供 `--deny-root` 与 `--probe`。Creo 导入使用已有 `scripts/run_creo_step_diagnostic.ps1`；原生件导出由 `tools/mesh_brep/creo_export.ps1`，随后 `tools.mesh_brep.creo_topology --before <局部STEP> --after <Creo导出STEP> --output <新目录>` 检查面数。最终目录通过 `tools.mesh_brep.report_compact --run $run --output <新目录>` 生成。

下一步保留紧凑共享接缝后端，针对外边附近切平面与曲率峰值改进约束及局部曲线压缩；当前证据不足以承诺仅靠减少 UV 畸变即可继续减少控制点并保持外接口 G1。
