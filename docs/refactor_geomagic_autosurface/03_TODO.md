# STEP-PATCH-OPTIMIZER 当前阶段 TODO

> 草案版本：v0.3-preview-then-apply  
> 当前主线：**候选区域预览 → 接入 Geomagic 后端生成 STP/IGS patch → patch 叠加预览 → 用户点击 Apply → 真实贴回与边界缝合 → StrictTopologyGate 验证**。  
> 核心调整：不直接在 Geomagic patch 生成后立即替换主模型；必须先叠加预览，用户确认后再执行真实贴回。

---

## 0. 当前路线定义

### 0.1 总体目标

```text
输入：当前 STP + 原始 STL

1. 在 STP 上生成 FeatureBoundedRegion candidates。
2. 用户在 GUI 中预览候选区域。
3. 用户接受 / 选择一个 candidate。
4. 对 candidate 提取并验证原 STP closed boundary wire。
5. 从原始 STL 中裁剪对应 local STL，允许 margin。
6. 调用 wrapCore.exe + AutoSurface，生成 local IGS / STEP patch。
7. OCCT 导入 Geomagic 输出 patch。
8. 在 Viewer 中把 patch 与原 candidate 区域叠加预览。
9. 用户点击“应用 / Apply”。
10. 使用原 STP boundary wire 约束 patch 替换。
11. 尝试 sewing / ShapeFix / SameParameter。
12. StrictTopologyGate 验证。
13. 合法则提交 Command；不合法则 rollback。
```

### 0.2 核心原则

```text
STP 提供拓扑边界。
STL 提供几何采样。
Geomagic 提供曲面拟合。
OCCT 负责 patch 导入、叠加预览、真实替换、缝合与验证。
用户确认是 patch preview 和真实替换之间的硬分界线。
```

禁止：

```text
不要把 STL 裁剪边界作为最终 CAD 边界。
不要直接信任 Geomagic patch 的外边界。
不要在 patch 生成后自动贴回主模型。
不要绕过用户确认执行真实替换。
不要绕过 StrictTopologyGate 提交模型。
不要让 redo 重新运行 Geomagic。
```

---

## 1. MVP 定义

### MVP-A：候选区域预览

```text
输入 STP
→ 检测 feature edges / user locked edges / model boundary
→ 生成 FeatureBoundedRegion candidates
→ GUI 高亮候选区域
→ 用户可接受 / 拒绝 / 隐藏 candidate
```

验收：

```text
1. FeatureBoundedRefit candidate 可显示。
2. 候选区域不跨越 protectedEdges。
3. candidate 状态变化不修改主模型。
4. GUI 能显示 candidate face count / boundary count / risk。
```

---

### MVP-B：Geomagic patch 生成 + 叠加预览

```text
用户选择一个 accepted candidate
→ RegionBoundaryAnalyzer 通过
→ 裁剪 local STL
→ wrapCore.exe + AutoSurface 生成 local IGS / STEP
→ PatchImportService 导入 patch
→ Viewer 叠加预览 patch
→ 输出完整 workspace 和日志
```

验收：

```text
1. local STL 成功生成。
2. Geomagic 输出 local_output.igs / local_output.step。
3. result.json 可解析。
4. patch 能导入 OCCT。
5. Viewer 能叠加显示 patch。
6. 清除 overlay 后主模型不变。
7. 此阶段不修改主 ShapeDocument。
```

---

### MVP-C：用户点击应用后的单候选真实贴回

```text
用户在 patch overlay 预览后点击 Apply
→ PatchReplacementCommand
→ BoundaryConstrainedPatchBuilder
→ sewing / ShapeFix / SameParameter
→ StrictTopologyGate
→ 成功提交 Command
→ 失败 rollback
→ 支持 undo/redo
```

验收：

```text
1. 只处理单 closed outer boundary candidate。
2. 无 holes。
3. 无 non-manifold boundary。
4. Gate 失败时主模型不变。
5. Gate 成功后可导出 STEP 并 roundtrip。
6. undo/redo 正常。
7. redo 不重新运行 Geomagic。
```

---

### MVP-D：批量与工程化增强

```text
多个 accepted candidates
→ 按 candidate 逐个生成 patch
→ 每个 patch 先 overlay preview
→ 用户逐个或批量 Apply
→ job queue / cache / report
```

MVP-D 在 MVP-C 单候选闭环稳定后再做。

---

# P0：候选区域主线重构

## T1.1 新增候选类型

文件：

```text
src/merge/MergeCandidate.h
src/merge/MergeCandidate.cpp
```

任务：

```text
新增 MergeCandidateType::FeatureBoundedRefit。
同步 toString、统计、GUI 显示。
```

验收：

```text
编译通过。
GUI/报告可显示 FeatureBoundedRefit。
现有 Plane/Sphere 测试不受影响。
```

---

## T1.2 新增 FeatureBoundedRegionBuilder

文件：

```text
src/merge/FeatureBoundedRegionBuilder.h
src/merge/FeatureBoundedRegionBuilder.cpp
tests/test_feature_bounded_region_builder.cpp
CMakeLists.txt
```

任务：

```text
输入 ShapeDocument + protectedEdges。
基于 face adjacency BFS/DFS。
不跨越 protectedEdges。
输出 FeatureBoundedRefit candidate。
```

验收：

```text
未保护边可跨越。
保护边不可跨越。
用户锁边不可跨越。
feature edge 不可跨越。
free edge / model boundary 成为区域边界。
```

---

## T1.3 MergePlanner 接入新开关

文件：

```text
src/merge/MergePlanner.h
src/merge/MergePlanner.cpp
```

新增：

```cpp
bool enable_feature_bounded_refit_candidates = true;
int min_feature_bounded_region_faces = 2;
```

验收：

```text
开关关闭时不生成 FeatureBoundedRefit。
开关开启时生成 FeatureBoundedRefit。
不破坏现有 PlaneLike / CylinderLike / SphereLike 入口。
```

---

## T1.4 GUI 候选区域预览入口

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/gui/MainWindow.cpp
src/gui/OccViewWidget.h
src/gui/OccViewWidget.cpp
src/gui/ModelTreePanel.cpp
src/gui/LogPanel.cpp
```

任务：

```text
1. GUI 新增 / 复用“预览 FeatureBoundedRegion 候选区域”入口。
2. 候选区域可高亮显示。
3. 用户可接受 / 拒绝 / 隐藏 candidate。
4. candidate 状态变化只影响候选管理，不修改 TopoDS_Shape。
```

验收：

```text
候选预览能跑通。
用户点击 accepted candidate 后能在报告面板看到 candidate id、face count、boundary edge count、risk。
```

---

# P0：Boundary 合法性门槛

## T2.1 强化 RegionBoundaryAnalyzer

文件：

```text
src/merge/RegionBoundaryAnalyzer.h
src/merge/RegionBoundaryAnalyzer.cpp
tests/test_region_boundary_analyzer.cpp
```

任务：

```text
分析 candidate faces 连通性。
提取并排序 outer boundary。
构造或输出 ordered boundary edges。
判断单闭环。
判断 inner holes。
判断 non-manifold / branch boundary。
输出 failure reason。
```

MVP-C 通过条件：

```text
connected_component_count == 1
outer_wire_count == 1
boundary_closed == true
inner_wire_count == 0
has_holes == false
has_non_manifold_edges == false
has_branching_boundary == false
```

验收：

```text
单闭环通过。
open boundary 拒绝。
multiple loop 拒绝。
holes 拒绝。
non-manifold boundary 拒绝。
branch boundary 拒绝。
失败原因可读。
```

---

## T2.2 BoundaryWireBuilder

文件：

```text
src/brep/BoundaryWireBuilder.h
src/brep/BoundaryWireBuilder.cpp
tests/test_boundary_wire_builder.cpp
CMakeLists.txt
```

任务：

```text
根据 RegionBoundaryAnalyzer 的 ordered_boundary_edges 构造 TopoDS_Wire。
不允许直接使用未排序 candidate.boundary_edges MakeWire。
```

验收：

```text
ordered closed edge loop 可构造 TopoDS_Wire。
open loop 构造失败。
乱序 edges 不能绕过 analyzer。
```

---

# P0：STL 输入与裁剪

## T3.1 新增 STL 数据结构

文件：

```text
src/stl/StlMesh.h
src/stl/StlMesh.cpp
```

任务：

```text
定义 StlTriangle、StlMesh、bbox 统计、triangle count 统计。
```

验收：

```text
StlMesh 可保存 triangle list。
可计算 bbox。
空 mesh 返回 invalid bbox。
```

---

## T3.2 新增 StlReader / StlWriter

文件：

```text
src/io/StlReader.h
src/io/StlReader.cpp
src/io/StlWriter.h
src/io/StlWriter.cpp
tests/test_stl_io.cpp
CMakeLists.txt
```

任务：

```text
读取原始 STL。
写出 local_region_XXXX/local_input.stl。
第一版优先支持 binary STL；ASCII STL 可后置。
```

验收：

```text
读取测试 STL 成功。
写出后再次读取成功。
triangle count 保持。
不存在文件返回失败。
```

---

## T3.3 新增 StlRegionExtractor

文件：

```text
src/stl/StlRegionExtractor.h
src/stl/StlRegionExtractor.cpp
src/stl/StlCropReport.h
src/stl/StlCropReport.cpp
tests/test_stl_region_extractor.cpp
CMakeLists.txt
```

任务：

```text
candidate bbox
→ expand margin
→ 裁剪相交 triangles
→ 输出 local STL
→ 输出 crop report。
```

默认参数：

```cpp
double bboxMarginRatio = 0.01;
double minMargin = 0.1;
```

验收：

```text
local STL triangle count > 0。
margin 增大，triangle count 不减少。
空结果返回失败。
local STL bbox 覆盖 candidate bbox。
```

注意：

```text
STL 裁剪只用于曲面拟合采样。
STL 裁剪边界不是最终 CAD 边界。
```

---

# P0：Geomagic AutoSurface 后端

## T4.1 配置和结果结构

文件：

```text
src/external/geomagic/GeomagicAutoSurfaceConfig.h
src/external/geomagic/GeomagicAutoSurfaceResult.h
```

任务：

```text
定义 wrapCorePath、scriptPath、input/output、numPatches、fallback、detail、geometry、timeout、logPath。
```

建议结构：

```cpp
struct GeomagicAutoSurfaceConfig {
    std::filesystem::path wrapCorePath;
    std::filesystem::path scriptPath;
    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path workDir;

    bool adaptiveFit = false;
    bool autoMerge = true;
    int numPatches = 1;
    std::vector<int> fallbackNumPatches = {2, 4, 8};
    double detail = 0.35;
    double tolerance = 0.05;
    std::string geometry = "Organic";
    bool convertIgesToStep = true;
    int timeoutSeconds = 1800;
};
```

验收：

```text
config 可写入 JSON。
result 可从 JSON 读取。
路径为空时返回配置错误。
```

---

## T4.2 GeomagicAutoSurfaceBackend

文件：

```text
src/external/geomagic/GeomagicAutoSurfaceBackend.h
src/external/geomagic/GeomagicAutoSurfaceBackend.cpp
tests/test_geomagic_backend_mock.cpp
CMakeLists.txt
```

任务：

```text
QProcess 调用 wrapCore.exe。
传入 autosurface_pipeline.py 和 config.json。
捕获 stdout/stderr。
读取 result.json。
支持 timeout。
支持 mock executable。
```

验收：

```text
mock success 通过。
mock failure 通过。
timeout 通过。
输出文件不存在判失败。
真实 Geomagic 不参与自动测试。
```

---

## T4.3 Geomagic Python 脚本

文件：

```text
scripts/geomagic_wrap/autosurface_pipeline.py
scripts/geomagic_wrap/autosurface_config.example.json
scripts/geomagic_wrap/README.md
```

任务：

```text
读取 config.json。
ReadFile 输入 local STL。
AutoSurface 输出 IGS。
可选 ReadFile(IGS).cadModel + WriteFile(STEP214)。
写 result.json。
```

默认参数：

```json
{
  "adaptive_fit": false,
  "auto_merge": true,
  "num_patches": 1,
  "fallback_num_patches": [2, 4, 8],
  "detail": 0.35,
  "geometry": "Organic",
  "convert_iges_to_step": true
}
```

验收：

```text
真实 Geomagic 环境手动验证。
result.json 可被 C++ backend 解析。
失败时 error_msg 不为空。
```

---

# P1：Geomagic patch 导入与叠加预览

> 本阶段是“应用前确认”阶段。  
> 它不是长期停留点，但必须存在：用户需要看到 Geomagic 得到的 STP/IGS patch 与原 candidate 的空间关系，然后再点击 Apply。

## T5.1 PatchImportService

文件：

```text
src/patch/ImportedPatchInfo.h
src/patch/PatchImportService.h
src/patch/PatchImportService.cpp
tests/test_patch_import_service.cpp
CMakeLists.txt
```

任务：

```text
导入 Geomagic 输出 STEP 或 IGS。
返回 patch shape、face count、edge count、bbox、BRepCheck。
不修改主 ShapeDocument。
```

优先级：

```text
优先导入 local_output.step。
如果 STEP 不存在或导入失败，再尝试 local_output.igs。
```

验收：

```text
导入成功 valid=true。
导入失败不影响主模型。
bbox 和 face count 可显示。
patch bbox 与 candidate bbox 偏差过大时标记 HighRisk。
```

---

## T5.2 Patch overlay 叠加预览

文件：

```text
src/gui/OccViewWidget.h
src/gui/OccViewWidget.cpp
src/app/AppController.h
src/app/AppController.cpp
```

任务：

```text
显示 imported patch overlay。
保持原 candidate 高亮。
支持显示 / 隐藏 / 清除 patch overlay。
支持重新生成 patch 后替换旧 overlay。
```

验收：

```text
patch overlay 能显示。
candidate highlight 与 patch overlay 可同时存在。
清除 overlay 后主模型不变。
多次导入不残留旧 AIS 对象。
```

---

## T5.3 Patch preview report

文件：

```text
src/patch/PatchPreviewReport.h
src/patch/PatchPreviewReport.cpp
src/gui/LogPanel.cpp
src/gui/ModelTreePanel.cpp
```

任务：

```text
输出 patch preview 报告：
- candidate id
- source face count
- source boundary edge count
- patch path
- patch face count
- patch edge count
- patch bbox
- candidate bbox
- bbox deviation
- import BRepCheck result
- recommended action
```

验收：

```text
用户在 Apply 前能看到 patch 是否明显偏离原 candidate。
patch import 失败时报告明确。
patch bbox 明显异常时阻止 Apply 或标记 HighRisk。
```

---

## T5.4 Apply 按钮和候选状态

文件：

```text
src/gui/MainWindow.cpp
src/gui/ModelTreePanel.cpp
src/app/AppController.h
src/app/AppController.cpp
src/merge/MergeCandidate.h
src/merge/MergeCandidate.cpp
```

新增状态建议：

```cpp
enum class RegionPatchStatus {
    NotGenerated,
    Generating,
    Generated,
    PreviewReady,
    ApplyPending,
    Applied,
    ApplyFailed
};
```

任务：

```text
1. patch overlay preview ready 后，GUI 启用 Apply。
2. Apply 只对当前 selected candidate 生效。
3. Apply 前再次检查 candidate boundary 和 imported patch 有效性。
4. Apply 后进入 PatchReplacementCommand。
```

验收：

```text
未生成 patch 时 Apply 禁用。
patch import 失败时 Apply 禁用。
PreviewReady 后 Apply 可点击。
Apply 失败后主模型不变。
```

---

# P1：用户点击 Apply 后的真实贴回

## T6.1 BoundaryConstrainedPatchBuilder

文件：

```text
src/patch/BoundaryConstrainedPatchBuilder.h
src/patch/BoundaryConstrainedPatchBuilder.cpp
tests/test_boundary_constrained_patch_builder.cpp
CMakeLists.txt
```

任务：

```text
输入：
- candidate source faces
- original STP outer boundary wire
- imported Geomagic patch shape

输出：
- replacement patch / replacement face / replacement shell fragment
```

第一版实现策略：

```text
1. 从 imported patch 中选择可用 face/surface。
2. 使用原 STP outer boundary wire 作为优先边界。
3. 尝试构造 replacement face。
4. 对 replacement face 做 ShapeFix_Face / SameParameter。
```

允许实验 fallback：

```text
若无法从 imported patch 抽取可用 underlying surface，
允许进入 direct patch sewing fallback，
但必须标记为 experimental，并必须通过 StrictTopologyGate。
```

验收：

```text
单 patch + 单 closed outer wire 可生成 replacement。
boundary invalid 时失败。
imported patch 无可用 face 时失败。
不直接使用 STL 裁剪边界作为最终边界。
```

---

## T6.2 PatchReplacementCommand

文件：

```text
src/command/PatchReplacementCommand.h
src/command/PatchReplacementCommand.cpp
tests/test_patch_replacement_command.cpp
CMakeLists.txt
```

任务：

```text
1. 输入 candidate + imported patch。
2. 保存 beforeDocument。
3. 删除 / 替换 candidate source faces。
4. 接入 BoundaryConstrainedPatchBuilder。
5. 尝试 sewing / ShapeFix / SameParameter。
6. 调用 StrictTopologyGate。
7. Gate 成功才提交 afterDocument。
8. Gate 失败 rollback。
9. 支持 undo/redo。
```

限制：

```text
只处理单 closed outer wire。
无 holes。
不处理复杂多 patch。
不处理 boundary 自交。
不处理跨 shell candidate。
```

验收：

```text
成功替换可 undo/redo。
gate 失败不改变 document。
redo 不重新运行 Geomagic。
失败报告包含 rollback_applied=true。
```

---

## T6.3 Sewing / ShapeFix 集成

文件：

```text
src/patch/BoundaryConstrainedPatchBuilder.cpp
src/command/PatchReplacementCommand.cpp
src/validate/StrictTopologyGate.cpp
```

任务：

```text
对替换后的临时 shape 执行必要修复：
- BRepLib::SameParameter
- ShapeFix_Face
- ShapeFix_Wire
- 可选 BRepBuilderAPI_Sewing
```

执行标准：

```text
1. sewing 只作用于临时 shape。
2. sewing 后必须重新统计 free edges / multiple edges。
3. sewing 不能掩盖拓扑破坏；Gate 仍是最终裁决。
```

验收：

```text
替换后如果 free edge 增加，Gate 拒绝。
sewing 成功但 BRepCheck 失败，Gate 拒绝。
sewing 后 solid count 改变，Gate 拒绝。
```

---

## T6.4 StrictTopologyGate 最小可用版

文件：

```text
src/validate/StrictTopologyGate.h
src/validate/StrictTopologyGate.cpp
tests/test_strict_topology_gate.cpp
CMakeLists.txt
```

检查：

```text
BRepCheck。
free edge 不增加。
multiple edge 不增加。
solid count 不变。
shell closure。
bbox 异常。
STEP export。
STEP roundtrip。
source face count > patch face count 或局部 face count 减少。
```

验收：

```text
gate 失败返回清晰原因。
report 可写入 JSON。
gate 失败不允许 Command 提交。
STEP roundtrip 失败时拒绝。
```

---

## T6.5 AppController / GUI 接入 Apply 流程

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/gui/MainWindow.cpp
src/gui/ModelTreePanel.cpp
src/gui/LogPanel.cpp
```

新增操作：

```text
Apply Current Patch To Candidate
```

内部流程：

```text
selected candidate
→ check PreviewReady
→ check boundary analysis
→ check imported patch
→ PatchReplacementCommand
→ StrictTopologyGate
→ report
```

验收：

```text
用户必须先看到 overlay preview，才能 Apply。
单候选区域可以从 GUI 完成真实贴回。
失败时日志可复盘。
成功后模型更新且 undo/redo 可用。
```

---

# P1：workspace 与日志规范

## T7.1 workspace 规范

每个 candidate 一个目录：

```text
workspace/session_YYYYMMDD_HHMMSS/region_0001/
    candidate.json
    boundary_report.json
    local_input.stl
    crop_report.json
    autosurface_config.json
    autosurface_stdout.log
    autosurface_stderr.log
    autosurface_result.json
    local_output.igs
    local_output.step
    patch_import_report.json
    patch_preview_report.json
    replacement_report.json
    validation_report.json
```

验收：

```text
失败时能根据 workspace 复盘。
成功时能保存完整过程文件。
每个阶段都有 report。
```

---

## T7.2 最小同步 RegionPatchJob

文件：

```text
src/jobs/RegionPatchJob.h
src/jobs/RegionPatchJob.cpp
```

状态：

```text
Pending
AnalyzingBoundary
CroppingStl
RunningGeomagic
ImportingPatch
PreviewReady
ApplyPending
Replacing
Validating
Applied
Rejected
Failed
Cancelled
```

说明：

```text
第一版可以同步执行，不必须完整异步队列。
但必须保留状态枚举和日志。
```

验收：

```text
成功路径状态顺序正确：
Pending → AnalyzingBoundary → CroppingStl → RunningGeomagic → ImportingPatch → PreviewReady → ApplyPending → Replacing → Validating → Applied。

失败路径进入 Failed 或 Rejected。
Cancelled 不继续执行后续阶段。
```

---

# P2：批量处理与缓存

## T8.1 RegionJobManager

文件：

```text
src/jobs/RegionJobManager.h
src/jobs/RegionJobManager.cpp
tests/test_region_job_manager.cpp
CMakeLists.txt
```

任务：

```text
管理多个 accepted candidates。
默认 Geomagic 串行。
支持取消。
支持状态查询。
支持多个 patch overlay preview。
```

验收：

```text
多个 candidate 可排队生成 patch。
任一 candidate 生成失败不影响其他 candidate。
GUI 能查看每个 candidate 状态。
```

---

## T8.2 GeomagicJobCache

文件：

```text
src/external/geomagic/GeomagicJobCache.h
src/external/geomagic/GeomagicJobCache.cpp
```

cache key：

```text
source STEP hash
source STL hash
candidate face ids
boundary edge ids
crop margin
AutoSurface 参数
autosurface_pipeline.py version
```

验收：

```text
cache 命中不重复运行 wrapCore。
参数变化后 cache 失效。
candidate 变化后 cache 失效。
```

---

## T8.3 批量 patch 生成与批量应用

文件：

```text
src/app/AppController.h/.cpp
src/gui/MainWindow.cpp
src/gui/LogPanel.cpp
```

新增：

```text
Generate Patches For Accepted Candidates
Apply Previewed Patches
```

限制：

```text
默认只处理 LowRisk accepted candidates。
HighRisk 必须手动单独执行。
默认 Geomagic worker = 1。
批量 Apply 前必须保证每个 candidate 都处于 PreviewReady。
```

验收：

```text
批量生成有总报告。
每个 candidate 有独立 patch preview report。
批量 Apply 中失败 candidate 不污染成功 candidate。
```

---

# 当前不要做

```text
1. 不继续把 OCCT PlaneRegionMerge 作为主线增强。
2. 不实现 OCCT 自由曲面拟合。
3. 不直接用 STL 裁剪边界作为最终 CAD 边界。
4. 不无条件信任 Geomagic patch 外边界。
5. 不在 patch preview 前自动替换主模型。
6. 不默认并行启动多个 wrapCore.exe。
7. 不把临时 STL / IGS / STEP / log 提交到仓库。
8. 不在单候选 Apply 闭环稳定前做全模型批量自动合并。
9. 不删除 Plane/Sphere 旧代码，只保留为 experimental / baseline。
```

---

# 建议 Codex 执行顺序

```text
Commit 1:
T1.1 FeatureBoundedRefit enum/string/statistics.

Commit 2:
T1.2 FeatureBoundedRegionBuilder + tests.

Commit 3:
T1.3 MergePlanner switch + GUI/Report display.

Commit 4:
T1.4 GUI candidate preview route.

Commit 5:
T2.1 RegionBoundaryAnalyzer strengthen.

Commit 6:
T2.2 BoundaryWireBuilder.

Commit 7:
T3.1/T3.2 STL mesh + reader/writer.

Commit 8:
T3.3 StlRegionExtractor.

Commit 9:
T4.1/T4.2 Geomagic backend config/result/mock.

Commit 10:
T4.3 autosurface_pipeline.py + manual Geomagic validation.

Commit 11:
T5.1 PatchImportService.

Commit 12:
T5.2/T5.3 patch overlay + preview report.

Commit 13:
T5.4 Apply button / candidate patch status.

Commit 14:
T6.1 BoundaryConstrainedPatchBuilder.

Commit 15:
T6.2 PatchReplacementCommand.

Commit 16:
T6.3 Sewing / ShapeFix integration.

Commit 17:
T6.4 StrictTopologyGate minimal.

Commit 18:
T6.5 GUI Apply flow.

Commit 19:
T7.1 workspace reports.

Commit 20:
T7.2 minimal RegionPatchJob.

Commit 21:
P2 batch/cache.
```

---

# 每阶段必须运行

```powershell
.\scripts\build_debug.ps1
.\scripts\test.ps1
```

真实 Geomagic 手动验证：

```powershell
.\scripts\run_gui.ps1
```

至少验证一个完整单候选区域：

```text
STP + STL
→ FeatureBoundedRefit candidate preview
→ accepted candidate
→ local STL crop
→ Geomagic local STEP / IGS
→ patch overlay preview
→ Apply
→ PatchReplacementCommand
→ StrictTopologyGate
→ undo/redo
→ STEP roundtrip
```
