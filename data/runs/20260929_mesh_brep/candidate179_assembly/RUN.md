# candidate-179：原接口覆盖、整件缝合与褶皱修正

当前交付是 [A/B 各一个局部与整件 STEP](../candidate179/README.md)。**B 已重新拟合并缝合，明显成排褶皱降低；仍未通过完整光顺和 Creo ModelCHECK 验收。** 用户截图对应先前 `creo_B/import/candidate.prt.1`，加载路径由本轮 Creo trail 确认；截图保留为 `user_rejected_wrinkles.png`。

## 做了什么

原始纯网格 B 五片直接拼回旧模型失败：29 条自由边、0 Solid，见 `direct/assembly.json`。之后按用户允许的“原区域边界采样点必须覆盖”增加独立接口适配阶段：保留完整的 26 条原 CAD 曲线及参数对应，外边控制系数固定；10010 个检查样本的最大落面误差为 `8.10e-12 mm`。这比只把边界点加大拟合权重更强。整件仍使用既有 `0.001 mm` 缝合容差。

第一版 `adapt_interface.py` 将原外边界与网格 B 内部几何组合，得到有效实体，但存在明显褶皱，面内重复节点最大法向跳变 33.631°，用户已拒绝该版。其局部 STEP、控制点、检查结果保留在 `adapted_B/`，不再作为当前交付。

采用的修正版 `fair_interface.py --corner-planes` 以 STL 三角形位置及网格法向重新联合拟合 XYZ。外边界精确固定，内接缝共享三维系数；用每个节点跨度四点 Gauss 积分完整计算二阶导数平方光顺项，并在重复节点和接缝两侧加入切平面探针。四个接缝外端点的共同切平面由相邻原边界切线确定，以四个线性等式约束求解，约束残差约 `1.94e-16`。没有使用原 CAD 内部位置、法向或高度值作为此次拟合目标。

这是 **纯网格后端之后的 CAD 辅助接口重拟合与组装**，不是完全脱离 CAD 的整件重建。改善来自新的拟合目标、积分与接口处理，不能全部归因于网格 UV。参数与两次候选上限见 `smoothness_protocol.json`，选型见 `selection.json`。旧原始 B 的不可读 CAD 隔离实验继续成立，但不能用于宣称这个附加接口阶段也不读 CAD。

## 实测前后对比

选型仅使用网格开发样本及实际曲面。冻结后，修正前后都重新使用原 CAD 同一批 30000 点，反向各 12000 点，seed `2909400/2909401`；未使用旧距离缓存。下面都是采样最大值，不是连续域误差上界。

| 项目 | 有褶皱的接口版 | 当前修正版 |
|---|---:|---:|
| 面数 / 实际支撑控制点 | 5 / 13375 | 5 / 13375 |
| 最大单方向控制点 | 152 | 152 |
| 网格→曲面 / 曲面→网格，mm | 0.007413 / 0.006549 | 0.002872 / 0.002760 |
| 原 CAD→曲面 / 曲面→原 CAD，mm | 0.008030 / 0.006656 | 0.003459 / 0.003583 |
| 相对原 CAD 法向 P95 / 最大，° | 2.731 / 26.678 | 0.982 / 8.400 |
| 内接缝法向最大差，° | 24.823 | 6.716 |
| 面内重复节点法向最大跳变，° | 33.631 | 2.801 |

当前内部位置差最大 `7.54e-12 mm`；原邻面要求 G1 的接口最大法向差 **6.875°**，仍超过原定 5°。同位置原模型的 G1 接口最大约 **0.0128°**，所以该残余差是新增问题。面内不存在零 Jacobian 采样，两两曲面求交未发现共享边之外的交线；这些检查不证明连续域无自交。

当前 10730 个网格顶点、20936 个三角形，5 张面的尺寸为 `120×25、87×25、152×25、151×25、25×25`，独立 XYZ 标量变量 **36789**。原始纯网格 B 为 3125 控制点；精确继承原接口令支撑表达增长到 13375，单方向 25 点预算仍失败。当前先解决可见形状，未宣称压缩改善。原始纯网格 B 已有两档分辨率对照；**本次新接口光顺仅复测了细网格**，未将旧两档结果冒充此次算法的分辨率复核。

证据：[run.json](run.json)、[对比 CSV](comparison.csv)、`fair_B_corners/confirmation.json`、`final_interfaces.json`、`final_intersections.json`。实际查看过 [同视角真实表面及全部节点参数线](../candidate179/wrinkle_comparison.png)；法向来自 STEP 曲面 D1，没有使用显示法向平滑滤镜。实际 [UV 图](final_uv.png) 也已保存，五片均无翻转。

## 整件与 Creo

局部 B 为开放的 5 张曲面。拼回未替换的原 CAD 面后，OCCT 与 STEP 回读均为 **2695 Face、1 个有效 Solid、零自由边、零多重边**。曲线落面最大 `1.10e-11 mm`，SameParameter / SameRange 失败数为 0。原面邻接关系保持，未加盖。

A 整件和修正版 B 整件均实际运行 Creo 11 导入及 ModelCHECK。B 模型名为 `b179_smooth`，避免与用户仍打开的旧 `candidate` 混淆；导入分数 PASS，记录体积 **690.042273 mm³**。`GEOM_CHECKS=1`、`SHORT_EDGES=1490` 仍报错，不能称 Creo 全部检查通过。此前 A 和有褶皱 B 整件也各有 GEOM_CHECKS；本轮未重新验证原始全模型的同尺度来源，故不把整件这条错误简单归咎于原模型或新面片。新版本的交互式 Creo 截图未单独采集，灰色表面的视觉检查来自真实 STEP 同条件渲染。

Creo 证据在 `creo_A/result.json`、`creo_B/result.json`、`creo_final_B/result.json` 及对应 XML。当前导入后的原生文件为 `creo_final_B/import/b179_smooth.prt.1`，保留本机。A 本轮只补齐整件检查，不宣称其起伏被此修正解决。

## 时间与失败保留

| 当前 B 阶段 | 秒 |
|---|---:|
| 原接口、网格、UV、法向探针准备 | 1.646 |
| 矩阵装配 | 4.232 |
| 约束系统求解 | 3.188 |
| 接口重拟合到局部 STEP 总计 | 9.561 |
| 原整件读取、缝合、导出、回读 | 27.573 |
| 独立网格及 CAD 确认 | 39.448 |
| Creo 导入及 ModelCHECK | 83.938 |

上述是实测墙钟；此次没有单独拆分准备内部的 UV/对应关系时间，也没有清空系统磁盘缓存或做新算法的冷暖速度比较。独立确认与其他任务并发执行，不用于同机单任务加速比。B 原始网格后端、上游网格生成另见原实验，不能从端到端成本中略去。

直接缝合失败、第一版边界适配、两次光顺候选及其整件构造的内部耗时合计 **111.262 s**，完整清单见 `run.json/executions`。未隐藏中间候选：`fair_B` 消除了主要条纹但接缝端点约 6.975°；补四个硬端点切平面得到 `fair_B_corners`，端点自身相容，但邻域仍约 6.716°。未继续无界调权重。

一次曲线投影端点无极值导致初次脚本失败，已将曲线端点纳入候选并验证参数单调；错误日志为 `fair_B_initial_error.log`。一次合并测试命令没有设置旧脚本导入路径，已改用正确的 unittest discover；旧错误留在 `tests.log`。最终 **7 项网格/数值测试、8 项保留工具回归通过**，见 `mesh_tests_final.log`、`retained_all_tests.log`。没有改 C++ 产品或将未运行的 C++ 测试计为通过。

## 复跑及文件范围

按 [工具说明](../../../../tools/mesh_brep/README.md) 依次运行纯网格 B、`fair_interface --corner-planes`、`assemble_candidate`。评价命令为：

```powershell
python -B -m tools.mesh_brep.evaluate --stl <input.stl> --candidate <network.step> --source <original_region.step> --count 30000 --reverse-count 12000 --seed 2909400 --output <new_confirmation.json>
python -B -m tools.mesh_brep.check_interface --reference <baseline_cold> --mesh-result <B_fine_25> --candidate <network.step> --output <new_interface.json>
./tools/candidate179_research/run_creo.ps1 -StepPath <sewn.step> -OutputDir <new_creo_directory> -CreoRoot <Creo安装目录> -ModelName b179_smooth
```

原输入、历史四片/五片和主工作区用户修改保留。当前交付目录只放 A/B 四个 STEP、一个表面对比图及说明；其余候选数值和必要局部证据留在本目录。整件 STEP 和原生 Creo 文件保留本机，通过入口直接可打开，复跑入口完整。

用户要求删除的无关夹具已排除出交付。自动审批在执行删除前拒绝命令，仅返回 **`blocked by policy`**；实际未删除，也没有换工具绕过。经核定的剩余 36 个目标在 `cleanup_plan.json`；[清理脚本](../../../../tools/mesh_brep/cleanup_unrelated.ps1) 默认仅列出目标，已核验语法但未执行删除。混有 candidate-179 数值的 `junction_and_wrap_diagnostics.json` 留作证据。

保留实现是共享三维接缝、四点积分光顺与独立接口适配。下一处瓶颈是原复杂边界附近的切平面/曲率兼容和表达成本；水密已经实现，仍不能把灰色实体显示当作光顺合格。
