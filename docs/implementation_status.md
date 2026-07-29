# 实现状态

本文是当前实现状态的唯一事实来源，只记录当前能力、限制和最近验证基线。里程碑见 completed_features.md，未完成任务见 TODO.md，精确参数与指标见 data/runs。

## 分支

当前开发分支为 native-regional-refit。

当前产品主线是原生 Surface Reduction。Geomagic AutoSurface、原生单曲面重拟合和原生局部 AutoSurface 均不是默认路线。

## 产品入口

GUI 主流程：

1. 导入 STEP。
2. 检测或锁定特征边。
3. 生成并选择候选区域。
4. 选择 STEP-only 或 STEP 加扫描 STL 软参考。
5. 运行原生重拟合预览。
6. 检查面数、偏差、拒绝原因和质量门。
7. 对通过候选执行 Apply。
8. 导出并执行往返或外部 CAD 验证。

主工具栏提供当前原生流程；旧候选、patch 与 Geomagic 入口保留在高级菜单中，只能显式启用。

## Surface Reduction 能力

### 候选与搜索

已实现：

- 从当前选择和拓扑图建立受保护边约束。
- 按原始 FaceId 集合表示搜索状态。
- 生成组+组、组+单面和单面+单面提案。
- 对候选做确定性去重、排序和预算控制。
- 以有限档案保留可继续扩展或可终验状态。
- 对硬接缝不可行状态尽早拒绝。
- 支持 TargetRatio 和 MinimizeFaceCount 两种产品模式。

TargetRatio 用于兼容锁定比例回归。MinimizeFaceCount 在拟合、终验和墙钟预算内寻找当前最好结果，并分别报告：

- OptimalProven：有效结果达到受保护边图给出的可证明下界。
- BestFoundWithinBudget：存在通过结果，但未证明最优。
- NoValidatedResultFound：预算内没有通过全部门禁的结果。

搜索中出现的低面数状态若未通过最终接缝、拓扑或往返门禁，不计为有效结果。

### 拟合

已实现：

- RestrictedBSplinePatchFitter 受约束 B-spline 拟合。
- STEP B-rep 采样作为默认输入。
- 原始扫描 STL 可作为显式软参考。
- 混合参数化、边界约束、留出验证和重裁剪诊断。
- 支撑曲面包络和参数域守卫。
- 连续参数投影与稠密 SameParameter 检查。

新增或扩大的候选分组始终从原始 STEP 面重新拟合，不从父候选拟合面继续合并。

### 缓存与执行策略

已实现：

- 以原始面集合、拟合配置和算法指纹为键的内容寻址缓存。
- 正结果和带原因的负结果缓存。
- 运行内与持久缓存统计。
- 固定数量工作线程的批量拟合。
- FitSet、Replay、Full 和 RefreshCache 分层执行。
- 分阶段计时、拟合计数和活动分组进度回调。

Replay 只允许重放相同算法指纹和配置的拟合事实。任何缓存模式都不会跳过最终候选装配、StrictTopologyGate 或 STEP 往返门。

### 替换与质量门

最终候选依次接受：

- 边界与接缝检查。
- 距离、法向和保留光顺接缝检查。
- 参数化和支撑曲面包络检查。
- 边界约束重裁剪。
- PatchReplacementRepair 受控修复。
- StrictTopologyGate。
- 全局 BRepCheck。
- STEP 写出和重新读入。
- 适用时的 CommercialCadQualityGate 与外部 CAD 检查。

任一门禁失败时 Apply 不提交，当前 ShapeDocument 与 CommandHistory 保持不变。

## GUI 与后台执行

当前 GUI 已支持：

- STEP 导入、特征边检测、候选生成、预览和 Apply 后台化。
- 长阶段心跳和 Process Status 同步。
- 显示当前面数、目标面数、拟合进度和活动分组。
- 区分 STEP-only 与扫描 STL 软参考；加载 STL 不会隐式改变请求。
- 预览、应用、一键预览并应用。
- 撤销、重做和 STEP 导出。
- 失败报告中保留阶段化拒绝原因。

自动 Apply 只在预览仍对应当前候选且全部质量门通过时执行。选择、模型或参数变化会使旧预览失效。

窗口级自动化覆盖仍不完整；日常交互需按 run_gui.md 的冒烟步骤人工确认。

## 最近锁定验证基线

### 12 区域回归

证据目录：

data/runs/20260715_surface_reduction_benchmark_final12b/

该基线覆盖 12 个动态发现区域，结果为 3 个 TargetAchieved、9 个 NoAcceptableReduction。成功区域均完成参数化、BRepCheck、STEP 回读和商业 CAD 类边界门。它用于证明重构和性能改动没有改变既有接受/拒绝合同。

### candidate-179 比例回归

证据目录：

data/runs/20260715_surface_reduction_candidate_0179_hybrid/

对象为 49 面选区。锁定配置使用 TargetRatio、扫描 STL 软参考、目标比例 0.95 和最多二面提案，最终得到 49→46。三个接受组通过几何、接缝、支撑曲面包络、稠密 SameParameter、B-rep、STEP 往返和 Creo 人工复核。

这是跨内核纠偏后的回归事实，不是最少面数证明，也不是 GUI 默认配置回放。

### candidate-179 最少面预算实验

证据目录：

data/runs/20260721_055601_195__surface-reduction-minimize__03-clay__cand-0179__windows-msvc-debug/

同一 49 面选区在 MinimizeFaceCount、512 次拟合预算和 64 次最终验证预算下返回 49→47、BestFoundWithinBudget。理论图下界为 1，但有效结果未达到该下界，因此不能声称最优。

搜索期间更低面数的未通过状态不计入最终结果。

### 分层缓存与性能基线

candidate-179 已完成 FitSet、Replay、Full 与 RefreshCache 的真实链路验证。缓存运行与无缓存 Full 路径保持相同的已接受分组和 49→46 结果。精确耗时、缓存命中和逐组 JSON 只在对应 data/runs 目录维护，不在本文复制。

## 路线状态

### Surface Reduction

状态：当前产品主线。

SR-0～SR-13 的产品化能力已进入主线，涵盖区域发现、受限拟合、质量门、确定性搜索、缓存、GUI 后台执行和最少面预算模式。阶段摘要见 completed_features.md。

### Geomagic AutoSurface

状态：显式后备。

保留 fitting STL 生成、Geomagic 调用、patch 导入、原始边界约束 Apply、诊断和外部 CAD 路由。默认 GUI 不自动进入该路线。合同见 geomagic_patch_workflow.md。

### 原生单曲面重拟合

状态：冻结实验。

SurfaceRefitter 与 native_surface_fit_probe 保留用于诊断和历史复现，但真实候选未形成可作为当前产品主线的稳定接受链。重新启用条件见 native_surface_refitting_guide.md。

### 原生局部 AutoSurface

状态：冻结实验。

拓扑分析、四边布局和多图参数化实验提供了失败边界，但没有建立稳定产品能力。结论与重启条件见 native_autosurface_guide.md。

### 原生区域级多曲面重拟合

状态：独立实验分支，命令行纵向链已改为曲线网络先行，尚未产品验收。

当前实现先生成并冻结显式 `RegionCurveNetwork`。内部界面使用端点固定、正则化的 C2 三次 B-spline；投影载体先删除局部反向段，三次曲线仍不能同时通过投影、转角和交叉门时才退回穿过原投影点的一级诊断曲线。每个 patch 同时保存载体 `parameters` 和矩形 `surfaceParameters`，四逻辑边从确定性合法分组中按实际 `surfaceParameters` 的翻转、面积比、99.5% 畸变和稳定 hash 选择。拟合样本以原始 `FaceId/UV` 在同面载体三角形内做重心映射，并记录同面回退和跨面失败。四条逻辑边使用相对边与共享边节点兼容、每侧不超过 25 个控制点的三次 B-spline 支撑；四周控制点冻结后，只用 `SimplicialLDLT` 求解内部变量的约化 SPD 法方程。Coons 仅可作为 `BoundaryLockedDiagnostic`。最终曲面直接报告逐面 poles/knots、公平能量、法向变化、曲率振荡、Jacobian 和条件数分位数。builder 复用唯一 Vertex/Edge，四张 Face 组成单一 `TopoDS_Shell`，并逐 Shell/Face/Wire/Edge 记录 `BRepCheck` 状态。缓存二进制版本为 11，算法指纹为 `native-regional-refit-v40-fair-boundary-spd`，旧布局和拟合缓存拒绝。

合成测试覆盖同面 `FaceId/UV` 重心映射与跨面拒绝、K=4 四逻辑边选择、共享边兼容节点、内部变量消元、重复约束、Coons 诊断降级、实际 poles/knots、带空格 STEP 实体统计、单 Shell quilt、质量门失败后的固定容差缝合和最终目录互斥合同。当前 candidate-179 全链诊断基线为 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_032156_431__native-region-refit__03_配件_Clay__cand-0179__target\final\`。K=4 曲线网络仍为 30 个节点、33 条曲线，5 条内部曲线均为 `RegularizedCubic`，最大投影误差 `0.0298705 mm`，无自交或非节点交叉。2572 个 fitting samples 全部通过同面重心映射，无回退或跨面失败。实际 `surfaceParameters` 暴露 116 个翻转或退化三角形，面积比为 0，99.5% 和最大畸变均为非有限值，布局产品门失败；每侧 25 控制点内不能同时满足冻结曲线误差，因此正式约化 SPD 路径未启动，保留高复杂度 Coons 诊断。独立距离为 `0.324422 mm`、法向 `21.7735°`、接缝法向 `18.3652°`，均未过拟合门。四张面实际为 `125×14`、`81×74`、`118×11`、`9×67` poles，总计 9645，最大单边 125，复杂度门失败；公平能量为 `7.44722e-4`，曲率振荡为 `0.919857`，最小归一化 Jacobian 为 `0.238712`，条件数 P50/P95/P99 为 `1.62490/4.57683/10.6860`，质量硬失败。

四张裁剪 Face 的 38 次 `SameParameter` 检查仍为零失败，最大残差 `4.36e-14 mm`，Curve-on-Surface 最大偏差 `4.78e-14 mm`，内部自由边和意外交线均为 0。单 Shell 中两张 Face 报 `BadOrientationOfSubshape`，所以内存态 BRep 产品门仍失败；固定 `0.001 mm` 诊断缝合和 STEP 回读保持 1 个 Solid、0 条自由边、0 条多重边与有效 B-Rep。STEP 实体统计已恢复为非零：基线/候选 surface 为 2739/2694、edge curve 为 5478/5400。Creo 导入状态与评分为 `OK/PASS`，PRT 生成成功，ModelCHECK 未通过。最终仍为 `DiagnosticOnly`、`canApply=false`；水密实体不代表布局、拟合、复杂度或产品门通过。K=5/6/8 布局筛选均未在 900000 ms 预算和 30 秒硬退出宽限内完成，因此没有进入完整拟合，K=4 是唯一形成完整可比较证据的候选。

## 当前限制

- MinimizeFaceCount 是预算搜索；除非达到可证明下界，否则不证明全局最优。
- GUI 默认参数与锁定的 candidate-179 49→46 比例回归不同。
- 单一 Open CASCADE 结果不足以证明跨 CAD 可交付。
- 原始扫描 STL 是软参考；缺失时仍可使用 STEP-only，但两种运行不可混为同一基线。
- 商业 CAD 自动检查依赖本机工具与许可，默认单元测试不会启动外部应用。
- 大型真实候选仍可能受拟合和终验预算限制。
- 项目序列化和完整窗口级自动化不是当前已完成能力。

## 验证入口

默认单元测试：

~~~powershell
./scripts/test.ps1
~~~

完整仓库验证：

~~~powershell
./scripts/verify_spo.ps1
~~~

底层构建与测试：

~~~powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
~~~

正式回归应复用对应脚本和独立 run-id。不得覆盖锁定证据目录，也不得用一次新运行的摘要替换原始 run.json。

## 状态维护规则

- 本文只维护当前快照，不追加日期流水账。
- 精确实验参数和指标写入 data/runs。
- 已完成阶段的短结论写入 completed_features.md。
- 未完成任务与验收条件写入 TODO.md。
- 架构不变量写入 module_design.md。
- 详细开发过程写入 Obsidian，历史差异由 Git 保存。
