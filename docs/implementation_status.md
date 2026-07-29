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

当前实现先生成并冻结显式 `RegionCurveNetwork`。内部界面使用端点固定、正则化的 C2 三次 B-spline；每个 patch 仍保存载体 `parameters` 和矩形 `surfaceParameters`，四逻辑边从确定性合法分组中选择。拟合样本以原始 `FaceId/UV` 在同面载体三角形内做重心映射；四条逻辑边使用共享节点兼容、每侧不超过 25 个控制点的三次 B-spline 支撑，四周控制点冻结后以 `SimplicialLDLT` 求解内部变量的约化 SPD 法方程。布局与曲面质量评价共享拓扑传播后的三角形方向，STEP Face 的局部绕序不再被误报为 UV 翻转。Coons 仅可作为 `BoundaryLockedDiagnostic`。

拟合仍分为双轨：`GeomagicEquivalent` 在恢复规格为 `ResearchPartial` 时明确返回 `UnsupportedFittingSpecification`；`FeaturePreserving` 保持原 STEP Edge、共享 Edge/pcurve、`SameParameter`、G0 和固定质量门不变。P0 沿唯一共享三维 Edge 的共同参数 `t` 同时评价 `Edge(t)` 与双侧 `Surface(pcurve(t))`，统一 seam 距离、法向和导数比例的取样真值。UV 拓扑、支撑曲面 Jacobian、Face/Wire 方向分别判定；拟合与 B-Rep seam 验证冲突时报告 `ValidatorInconsistent` 并阻断产品资格。缓存二进制版本保持 14，算法指纹为 `native-regional-refit-v44-shared-edge-parameter-validator-gates`。

P0 锁定诊断证据位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_212344_628__native-region-refit__03_配件_Clay__cand-0179__target\final\`。旧 `4.96138 mm` seam 距离来自 mesh-vertex 配对污染；共同参数下双侧支撑面最大分离为 `4.36e-14 mm`，Curve-on-Surface 偏差为 `4.80e-14 mm`。7 个原始三角形符号变化经拓扑方向传播后判定为 STEP Face 局部绕序，不是 UV 翻转。4 张裁剪 Face 的 38 次 `SameParameter` 检查无失败，内部自由边和意外交线为 0；固定 `0.001 mm` 诊断缝合与 STEP 回读得到 1 个 Solid、0 条自由边和 0 条多重边。Creo 导入成功，但 Face/Wire 仍有 `BadOrientationOfSubshape` 且 ModelCHECK 失败，因此结果为 `DiagnosticOnly`、`canApply=false`。

当前代码不包含 P1 的 `TrimmedConvexSupportChart` 支撑面解耦，也不包含 P2 的边界可表示性预检和定向增片；相关既有运行目录仅作为历史失败证据保留。P0 只修正验证真值和错误分类，不改善拟合曲面的距离、法向、接缝、公平性或控制网复杂度，因此不能转化为 `ProductFit`。

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
