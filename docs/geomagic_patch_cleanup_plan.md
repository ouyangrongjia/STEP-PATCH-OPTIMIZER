# 无损旧无用代码清理计划

> 文件：`docs/geomagic_patch_cleanup_plan.md`  
> 更新日期：2026-06-16  
> 适用目标：在不破坏当前 STEP / Geomagic Patch / Apply / undo-redo 功能的前提下，清理历史 analytic region merge 残留。  
> 当前结论：旧清理计划只保留为历史背景，不能再作为执行计划直接使用。

---

## 1. 旧计划是否还要用

不用作执行计划。

原因很简单：旧计划仍停留在早期 “STP sampling -> Geomagic STP -> patch apply” 设计阶段，里面把一批当时尚未落地的模块写成“建议新增”，也把部分 legacy 路径写成可以直接删除。当前仓库已经进入更晚阶段：

1. `GeomagicFittingInputMode::StpSampledCandidateSurface` 已是 GUI 默认输入模式。
2. legacy STL crop、conservative boundary-band crop、Global Cut Chain 仍是诊断 / 兜底能力，不能当废代码删。
3. `PatchReplacementCommand`、`PatchReplacementRepair`、`StrictTopologyGate` 已经接入真实 Apply。
4. redo 语义已经明确：只能复用缓存 afterDocument，不能重新运行 Geomagic、crop、import、repair。
5. Plane / Sphere 旧真实合并的 GUI 入口已在 Cleanup-2 下线；AppController API、Command 和 command 测试已在 Cleanup-3 删除；后端 merger 和后端测试已在 Cleanup-4 删除；`RegionMergeResult` / `RegionMergeOptions` 已在 Cleanup-5 删除；旧 analytic candidate type 已在 Cleanup-6 收口。

所以本文替代旧计划，作为后续瘦身的唯一执行依据。

---

## 2. 清理边界

### 2.1 必须保持不变的功能

以下功能不能因为清理旧代码而退化：

1. STEP / STP 读取、显示、选择、导出。
2. 特征边检测、用户锁边、候选区域预览。
3. `FeatureBoundedRefit` 候选生成、选择、accept / reject / hide / restore。
4. Geomagic Patch preview。
5. 默认 `StpSampledCandidateSurface` fitting input mode。
6. legacy STL crop 和 conservative boundary-band STL crop 作为可选对照路线。
7. `StlCutChainCutter` / Global Cut Chain 作为独立 STL 裁剪工具路线。
8. Patch import、patch overlay、Patch Apply。
9. `PatchReplacementRepair`。
10. `StrictTopologyGate`。
11. undo / redo，尤其是 redo 不重新运行 Geomagic / fitting STL / crop / import / repair。
12. 当前默认测试集能构建并通过。

### 2.2 绝对禁止的清理方式

1. 不能删除 `PatchReplacementCommand`、`PatchReplacementRepair`、`StrictTopologyGate` 或绕过它们。
2. 不能把 STL crop boundary、STP sampled mesh boundary、Global Cut Chain output boundary、Geomagic patch outer boundary 当最终 CAD boundary。
3. 不能删除 `StlRegionExtractor`，除非 GUI 和 AppController 已经完全移除 legacy / conservative crop 模式，并有替代诊断路线。
4. 不能删除 `StlCutChainCutter`，它不是当前 one-click patch preview 默认模式，但仍是明确保留的可选工具路线。
5. Cleanup-5 后不能继续把 Plane / Sphere 后端或 `RegionMergeResult` / `RegionMergeOptions` 当作保留实现。
6. Cleanup-6 后不能继续把 PlaneLike / SphereLike / CylinderLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 当作当前候选类型；当前候选预览主类型只保留 `FeatureBoundedRefit`，`SameDomain` 和 `Unknown` 仅作为兼容 / 统计状态存在。
7. 不能把真实样例路径或 candidate ordinal 写死进测试或工具。

---

## 3. 当前代码盘点

本次盘点使用以下搜索范围：

```powershell
rg --files src tests tools scripts CMakeLists.txt
rg -n "PlaneRegion|SphereRegion|CylinderRegion|ConeRegion|TorusRegion|RegionMergeStub|RegionMergeResult|RegionMergeOptions|mergePlane|mergeSphere|PlaneLike|SphereLike|CylinderLike|ConeLike|TorusLike|FeatureBoundedRefit" src tests CMakeLists.txt
```

### 3.1 旧 Plane / Sphere 真实合并链路已分阶段下线

Cleanup-2 前，以下 GUI 入口仍存在，因此不能直接删后端：

```text
src/app/MainWindow.h
    mergeCurrentPlaneCandidate()
    mergeAllMergeablePlaneCandidates()
    mergeAllApproximatePlaneCandidates()
    mergeCurrentSphereCandidate()
    mergeAllMergeableSphereCandidates()
    mergePlaneCandidateAction_
    mergeSphereCandidateAction_

src/app/MainWindow.cpp
    菜单 action 创建、菜单插入、connect
    MainWindow::mergeCurrentPlaneCandidate()
    MainWindow::mergePlaneCandidateBatch()
    MainWindow::mergeCurrentSphereCandidate()
    MainWindow::mergeSphereCandidateBatch()

src/app/AppController.h/.cpp
    mergePlaneCandidate()
    mergePlaneCandidates()
    mergeSphereCandidate()
    mergeSphereCandidates()

src/command/
    PlaneRegionMergeCommand
    PlaneRegionBatchMergeCommand
    SphereRegionMergeCommand
    SphereRegionBatchMergeCommand

src/merge/
    PlaneRegionMerger
    SphereRegionMerger
```

Cleanup-2 后，`src/app/MainWindow.*` 中的旧 Plane / Sphere 真实合并 action、菜单和 handler 已删除。Cleanup-3 后，`AppController` API、Command 类和对应 command 测试已删除。Cleanup-4 后，`PlaneRegionMerger` / `SphereRegionMerger` 后端和对应后端测试也已删除。

Cleanup-5 后，`RegionBoundaryAnalyzer` 已改用独立 `BoundaryAnalysisFailureReason`，`RegionMergeResult` / `RegionMergeOptions` 文件已删除。当前剩余清理点只剩旧 analytic candidate type 依赖。

### 3.2 stub-only 合并器是第一批候选

以下文件只通过 stub 测试和 CMake 保持存在，未发现 GUI / AppController 活入口：

```text
src/merge/CylinderRegionMerger.h
src/merge/CylinderRegionMerger.cpp
src/merge/ConeRegionMerger.h
src/merge/ConeRegionMerger.cpp
src/merge/TorusRegionMerger.h
src/merge/TorusRegionMerger.cpp
src/merge/RegionMergeStub.h
src/merge/RegionMergeStub.cpp
tests/test_region_merge_stubs.cpp
```

这批可以作为 Cleanup-1 的优先瘦身对象，但前提是：

```text
rg "CylinderRegionMerger|ConeRegionMerger|TorusRegionMerger|RegionMergeStub" src tests CMakeLists.txt
```

确认命中只剩上述文件、CMake 和对应 stub 测试。

### 3.3 旧候选类型已收口

Cleanup-6 后，`PlaneLike`、`SphereLike`、`CylinderLike`、`ConeLike`、`TorusLike`、`FreeformG1`、`FreeformG2` 不再是源码中的候选类型。

当前候选类型边界：

```text
MergeCandidateType::FeatureBoundedRefit
MergeCandidateType::SameDomain
MergeCandidateType::Unknown
```

当前生成路径：

```text
MergePlanner
→ FeatureBoundedRegionBuilder
→ FeatureBoundedRefit candidates
```

`MergeRegionGrower` 和 `tests/test_analytic_candidate_detection.cpp` 已删除；`CandidateFilters`、MainWindow、Viewer、ModelTree、Face Inspect 和候选类型测试已同步到 FeatureBoundedRefit / Unknown 的当前主线。

### 3.4 `RegionMergeResult` / `RegionMergeOptions` 已删除

Cleanup-5 已将 `RegionBoundaryAnalyzer` 及其测试从 `RegionMergeFailureReason` 切换到更准确的 `BoundaryAnalysisFailureReason`，并删除 `RegionMergeResult` / `RegionMergeOptions`。旧 RegionMerge result/options 概念不再是当前源码依赖。

---

## 4. 执行阶段

### Cleanup-0：基线锁定

目的：确认瘦身前仓库状态，不带着未知失败删代码。

执行前必须记录：

```powershell
git status --short
rg -n "PlaneRegion|SphereRegion|CylinderRegion|ConeRegion|TorusRegion|RegionMergeStub" src tests CMakeLists.txt
rg -n "GeomagicFittingInputMode|StpSampledCandidateSurface|StlRegionExtractor|StlCutChainCutter|StrictTopologyGate|PatchReplacementRepair|redo" src docs
```

最低验证：

```powershell
.\scripts\build_debug.ps1
.\scripts\test.ps1
git diff --check
```

如果本机因 PDB 占用、Qt runtime、Geomagic 外部程序缺失导致非源码失败，必须记录错误原文，不能把失败伪装成通过。

### Cleanup-1：删除 stub-only analytic merger

目标：删除未接入 GUI / AppController 的 Cylinder / Cone / Torus stub 合并器。

删除候选：

```text
src/merge/CylinderRegionMerger.h
src/merge/CylinderRegionMerger.cpp
src/merge/ConeRegionMerger.h
src/merge/ConeRegionMerger.cpp
src/merge/TorusRegionMerger.h
src/merge/TorusRegionMerger.cpp
src/merge/RegionMergeStub.h
src/merge/RegionMergeStub.cpp
tests/test_region_merge_stubs.cpp
```

同步修改：

```text
CMakeLists.txt
    删除 ConeRegionMerger.cpp
    删除 CylinderRegionMerger.cpp
    删除 TorusRegionMerger.cpp
    删除 RegionMergeStub.cpp
    删除 tests/test_region_merge_stubs.cpp
```

Cleanup-1 当时临时保留，Cleanup-6 后已删除：

```text
MergeCandidateType::CylinderLike
MergeCandidateType::ConeLike
MergeCandidateType::TorusLike
MergeRegionGrower 中的检测逻辑
MergePlanner 中的候选生成逻辑
候选统计、颜色、GUI 显示
```

验收命令：

```powershell
rg -n "CylinderRegionMerger|ConeRegionMerger|TorusRegionMerger|RegionMergeStub" src tests CMakeLists.txt
.\scripts\build_debug.ps1
.\scripts\test.ps1
git diff --check
```

预期：`rg` 对上述 class / stub 名称无源码和测试命中，构建和测试通过。

### Cleanup-2：隐藏旧 Plane / Sphere GUI 真实合并入口

目标：先从用户操作面下线旧真实合并，不删除后端。

处理对象：

```text
src/app/MainWindow.h
src/app/MainWindow.cpp
```

移除或隐藏：

```text
mergePlaneCandidateAction_
mergeAllPlaneCandidatesAction_
mergeAllApproximatePlaneCandidatesAction_
mergeSphereCandidateAction_
mergeAllSphereCandidatesAction_

MainWindow::mergeCurrentPlaneCandidate()
MainWindow::mergeAllMergeablePlaneCandidates()
MainWindow::mergeAllApproximatePlaneCandidates()
MainWindow::mergePlaneCandidateBatch()
MainWindow::mergeCurrentSphereCandidate()
MainWindow::mergeAllMergeableSphereCandidates()
MainWindow::mergeSphereCandidateBatch()
```

保留：

```text
showMergeCandidatesByType()
candidateTypeText()
displayCandidateTypes()
candidate preview report
FeatureBoundedRefit patch actions
undo / redo
STEP import / export
```

注意：本阶段不动 `AppController::mergePlaneCandidate()` / `mergeSphereCandidate()`，也不删 Command。这样风险小，失败时可快速回退该阶段改动。

验收：

```powershell
rg -n "mergePlaneCandidateAction_|mergeSphereCandidateAction_|mergeCurrentPlaneCandidate|mergeCurrentSphereCandidate|mergeAllApproximatePlaneCandidates" src/app
.\scripts\build_debug.ps1
.\scripts\test.ps1
```

GUI 手动验收：

```text
1. 启动 GUI。
2. 打开 STP。
3. Preview FeatureBoundedRefit candidates。
4. accept / reject / hide / restore 正常。
5. Generate Geomagic Patch 仍可触发。
6. Apply Patch 仍必须经过 PatchReplacementRepair + StrictTopologyGate。
7. 菜单和工具栏不再出现旧 Plane / Sphere 真实合并入口。
```

### Cleanup-3：删除 Plane / Sphere AppController API 和 Command

前置条件：

```text
Cleanup-2 已验证通过；
src/app/MainWindow.* 不再调用 mergePlaneCandidate / mergeSphereCandidate；
```

删除对象：

```text
src/command/PlaneRegionMergeCommand.h
src/command/PlaneRegionMergeCommand.cpp
src/command/PlaneRegionBatchMergeCommand.h
src/command/PlaneRegionBatchMergeCommand.cpp
src/command/SphereRegionMergeCommand.h
src/command/SphereRegionMergeCommand.cpp
src/command/SphereRegionBatchMergeCommand.h
src/command/SphereRegionBatchMergeCommand.cpp
```

同步修改：

```text
src/app/AppController.h
src/app/AppController.cpp
CMakeLists.txt
tests/test_plane_region_merge_command.cpp
tests/test_sphere_region_merge_command.cpp
```

删除 API：

```cpp
RegionMergeResult mergePlaneCandidate(...);
RegionMergeResult mergePlaneCandidates(...);
RegionMergeResult mergeSphereCandidate(...);
RegionMergeResult mergeSphereCandidates(...);
```

验收：

```powershell
rg -n "PlaneRegionMergeCommand|SphereRegionMergeCommand|mergePlaneCandidate\\(|mergeSphereCandidate\\(" src tests CMakeLists.txt
.\scripts\build_debug.ps1
.\scripts\test.ps1
```

### Cleanup-4：删除 Plane / Sphere Merger 后端

前置条件：

```text
Cleanup-3 已验证通过；
AppController、MainWindow、Command 已无 Plane / Sphere 真实合并入口；
```

删除对象：

```text
src/merge/PlaneRegionMerger.h
src/merge/PlaneRegionMerger.cpp
src/merge/SphereRegionMerger.h
src/merge/SphereRegionMerger.cpp
tests/test_plane_region_merger.cpp
tests/test_sphere_region_merger.cpp
```

需要同步处理的测试引用：

```text
tests/test_analytic_candidate_detection.cpp
tests/test_candidate_type_statistics.cpp
tests/test_region_boundary_analyzer.cpp
```

处理原则：

1. 如果测试目的是保护候选检测、候选统计、boundary analyzer，改为测试对应模块本身。
2. 如果测试只是在证明 Plane / Sphere merger 拒绝非法输入，随 merger 一起删除。
3. 不允许把旧测试简单改名为新测试。

验收：

```powershell
rg -n "PlaneRegionMerger|SphereRegionMerger" src tests CMakeLists.txt
.\scripts\build_debug.ps1
.\scripts\test.ps1
```

### Cleanup-5：清理 RegionMergeResult / RegionMergeOptions

状态：已完成。

前置条件：

```text
Cleanup-4 已验证通过；
rg "RegionMergeResult|RegionMergeOptions|RegionMergeFailureReason" src tests
```

若命中只剩 `RegionBoundaryAnalyzer`，有两种选择：

1. 保留 `RegionMergeFailureReason` 作为 boundary analysis 失败枚举，删除 `RegionMergeResult` / `RegionMergeOptions`。
2. 新建更准确的 `BoundaryAnalysisFailureReason`，再删除全部 RegionMerge 概念。

推荐选择 2，但必须单独做，不能混在后端删除阶段。

执行结果：

```text
新增 `BoundaryAnalysisFailureReason`，只承载 RegionBoundaryAnalyzer 的边界分析失败原因。
RegionBoundaryAnalyzer 和 tests/test_region_boundary_analyzer.cpp 已切换到该枚举。
src/merge/RegionMergeResult.h 和 src/merge/RegionMergeOptions.h 已删除。
rg -n "RegionMergeResult|RegionMergeOptions|RegionMergeFailureReason|regionMergeFailureReasonToString" src tests CMakeLists.txt 无命中。
```

### Cleanup-6：旧候选类型收口

状态：已完成。

完成内容：

```text
FeatureBoundedRefit 已经是 patch workflow 唯一候选主类型；
MergePlanner 不再生成 PlaneLike / SphereLike / CylinderLike / ConeLike / TorusLike；
GUI / Viewer / ModelTree / Inspect 不再依赖旧类型显示；
tests/test_analytic_candidate_detection.cpp 已替换为 FeatureBoundedRefit 或其他当前主线测试；
```

已删除 / 收口：

```text
MergeCandidateType::PlaneLike
MergeCandidateType::CylinderLike
MergeCandidateType::ConeLike
MergeCandidateType::SphereLike
MergeCandidateType::TorusLike
MergeCandidateType::FreeformG1
MergeCandidateType::FreeformG2
```

同步点：

```text
src/merge/MergeCandidate.h/.cpp
src/merge/CandidateFilters.cpp
src/merge/MergePlanner.cpp
src/merge/MergeRegionGrower.cpp
src/app/MainWindow.cpp
src/gui/OccViewWidget.cpp
src/gui/ModelTreePanel.cpp
src/merge/FaceInspector.cpp
tests/test_candidate_type_statistics.cpp
tests/test_analytic_candidate_detection.cpp
```

保留功能：

```text
FeatureBoundedRefit preview / filtering / status management
candidate id highlight
candidate accept / reject / hide / restore
Face Inspect 当前候选归属诊断
Geomagic patch preview / Apply
PatchReplacementRepair + StrictTopologyGate
undo / redo 缓存语义
```

---

## 5. 每阶段通用停止条件

出现以下任一情况，立即停止该阶段，不继续扩大改动：

1. `.\scripts\build_debug.ps1` 失败，且不是明确的外部环境问题。
2. `.\scripts\test.ps1` 失败，且不能用当前阶段改动解释。
3. `FeatureBoundedRefit` preview 失效。
4. Geomagic Patch preview 入口失效。
5. Apply 不再经过 `PatchReplacementRepair` 或 `StrictTopologyGate`。
6. redo 重新运行了 Geomagic / STL generation / crop / import / repair。
7. STEP 导入、导出、undo、redo、候选状态管理任一基础功能退化。

---

## 6. 推荐提交拆分

```text
commit 1: docs: replace legacy cleanup draft with lossless cleanup plan
commit 2: merge: remove unused cylinder cone torus region merger stubs
commit 3: gui: hide legacy plane and sphere real merge actions
commit 4: app: remove legacy plane and sphere merge controller APIs
commit 5: command: delete legacy plane and sphere merge commands
commit 6: merge: delete legacy plane and sphere region merger backends
commit 7: merge: replace remaining RegionMerge result types where appropriate
commit 8: gui/merge: remove legacy analytic candidate types after FeatureBoundedRefit fully owns preview
```

commit 1 已完成。commit 2 已执行第一批源码瘦身：删除 Cylinder / Cone / Torus stub-only RegionMerger 与 `RegionMergeStub`，并保留旧候选类型、候选检测和显示通道。commit 3 已执行：MainWindow 旧 Plane / Sphere 真实合并 GUI 入口已下线。commit 4/5 已作为 Cleanup-3 执行：Plane / Sphere AppController API、Command 类和 command 测试已删除。
commit 6 已作为 Cleanup-4 执行：Plane / Sphere 后端 merger、后端测试以及附带测试依赖已删除或改写为各模块自身断言。
commit 7 已作为 Cleanup-5 执行：RegionBoundaryAnalyzer 切换到 `BoundaryAnalysisFailureReason`，旧 `RegionMergeResult` / `RegionMergeOptions` 删除。
commit 8 已作为 Cleanup-6 执行：旧 analytic candidate enum、`MergeRegionGrower`、旧 analytic candidate detection 测试，以及 GUI / Viewer / ModelTree / Inspect 中的旧类型显示入口已删除。

---

## 7. 当前执行清单

```text
[x] 重读当前 Workspace / 项目 / 仓库 / Geomagic 主线文档。
[x] 盘点 legacy analytic region merge 代码引用。
[x] 明确旧计划不可直接执行。
[x] 生成新的无损清理计划。
[x] 执行 Cleanup-0 基线验证。
[x] 执行 Cleanup-1 stub-only merger 删除。
[x] 执行 Cleanup-2 Plane / Sphere GUI 入口下线。
[x] 执行 Cleanup-3 AppController API / Command / command 测试删除。
[x] 执行 Cleanup-4 Plane / Sphere merger 后端与后端测试删除。
[x] 执行 Cleanup-5 RegionMergeResult / RegionMergeOptions 清理。
[x] 执行 Cleanup-6 旧候选类型收口。
```

Cleanup-0 / Cleanup-1 / Cleanup-2 / Cleanup-3 / Cleanup-4 / Cleanup-5 / Cleanup-6 执行记录：

```text
2026-06-16:
- baseline build passed.
- `spo_tests.exe` direct run passed in about 146 seconds; old CTest TIMEOUT=120 was too tight.
- `scripts/test.ps1` now propagates native command failures and runs CTest with a 300 second timeout.
- Removed stub-only CylinderRegionMerger / ConeRegionMerger / TorusRegionMerger / RegionMergeStub and `tests/test_region_merge_stubs.cpp`.
- CMake no longer builds those deleted sources or stub test.
- `rg -n "CylinderRegionMerger|ConeRegionMerger|TorusRegionMerger|RegionMergeStub" src tests CMakeLists.txt` has no active hit.
- Removed MainWindow legacy Plane / Sphere real merge actions, menus, connects and handlers.
- Kept candidate preview/type filtering, same-domain `applyMerge()`, Patch preview / Apply, AppController APIs, Commands and Plane / Sphere merger backends.
- `rg -n "mergePlaneCandidateAction_|mergeSphereCandidateAction_|mergeCurrentPlaneCandidate|mergeCurrentSphereCandidate|mergeAllApproximatePlaneCandidates" src/app` has no active hit.
- `scripts/build_debug.ps1` passed after Cleanup-2.
- `scripts/test.ps1` passed after Cleanup-2, 1/1 `spo_tests`, about 144 seconds.
- Removed Plane / Sphere AppController merge APIs, Plane / Sphere Command classes and corresponding command tests.
- CMake no longer builds PlaneRegionMergeCommand / SphereRegionMergeCommand sources or command tests.
- `rg -n "PlaneRegionMergeCommand|SphereRegionMergeCommand|mergePlaneCandidate\\(|mergeSphereCandidate\\(" src tests CMakeLists.txt` has no active hit.
- `scripts/build_debug.ps1` passed after Cleanup-3.
- `scripts/test.ps1` passed after Cleanup-3, 1/1 `spo_tests`, about 91 seconds.
- Removed PlaneRegionMerger / SphereRegionMerger backends and corresponding backend tests.
- Reworked analytic candidate detection, candidate type statistics and RegionBoundaryAnalyzer tests so they assert their own modules instead of instantiating PlaneRegionMerger.
- CMake no longer builds PlaneRegionMerger / SphereRegionMerger sources or backend tests.
- `rg -n "PlaneRegionMerger|SphereRegionMerger" src tests CMakeLists.txt` has no active hit.
- `scripts/build_debug.ps1` passed after Cleanup-4; only the existing Qt translation catalog warning appeared.
- `scripts/test.ps1` passed after Cleanup-4, 1/1 `spo_tests`, about 146 seconds.
- Added BoundaryAnalysisFailureReason for RegionBoundaryAnalyzer.
- Removed RegionMergeResult / RegionMergeOptions headers.
- `rg -n "RegionMergeResult|RegionMergeOptions|RegionMergeFailureReason|regionMergeFailureReasonToString" src tests CMakeLists.txt` has no active hit.
- Removed legacy analytic candidate enum values: PlaneLike / CylinderLike / ConeLike / SphereLike / TorusLike / FreeformG1 / FreeformG2.
- Removed MergeRegionGrower and `tests/test_analytic_candidate_detection.cpp`; MergePlanner now only delegates to FeatureBoundedRegionBuilder for FeatureBoundedRefit candidates.
- Simplified CandidateFilters, MainWindow reports/type filters, Viewer candidate coloring, ModelTree candidate counts and Face Inspect notes to the current FeatureBoundedRefit / Unknown surface.
- `rg -n "PlaneLike|SphereLike|CylinderLike|ConeLike|TorusLike|FreeformG1|FreeformG2|MergeRegionGrower|test_analytic_candidate_detection|enable_plane_candidates|enable_cylinder_candidates|enable_sphere_candidates|enable_cone_candidates|enable_torus_candidates" src tests CMakeLists.txt` has no active hit.
```

---

## 8. 关键判断

仓库现在真正该删的不是“所有 analytic 相关代码”，而是“已经没有当前入口和保护价值的残留实现”。  
按这个标准，Cylinder / Cone / Torus stub merger 已删除，Plane / Sphere 入口、Command 和后端也已删除；旧 analytic candidate 类型和检测也已收口。当前必须保留的是 FeatureBoundedRefit 候选主线、Geomagic patch preview / Apply、STL fitting input / crop 诊断路线、PatchReplacementRepair、StrictTopologyGate 和 undo / redo 缓存语义。
