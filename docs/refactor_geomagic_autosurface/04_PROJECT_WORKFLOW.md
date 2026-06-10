# STEP-PATCH-OPTIMIZER 项目流程文档

> 草案版本：v0.2-preview-then-apply  
> 用途：定义从 STP + 原始 STL 输入，到候选预览、Geomagic patch 生成、patch 叠加预览、用户 Apply、真实贴回和验证的完整流程。

---

## 1. 总体流程

```text
输入：
    当前 STP
    原始 STL

流程：
    1. 读取 STP，构建 ShapeDocument / FaceIndex / EdgeIndex / TopologyGraph。
    2. 读取原始 STL，构建 StlMesh。
    3. 在 STP 上检测 feature edges，合并 user locked edges，得到 protectedEdges。
    4. 通过 protectedEdges 分割 STP faces，生成 FeatureBoundedRegion candidates。
    5. 用户预览、接受、拒绝或隐藏候选区域。
    6. 对 accepted candidate 做 RegionBoundaryAnalyzer。
    7. 仅允许 closed single outer boundary candidate 进入 patch generation。
    8. 根据 candidate bbox / boundary 从原始 STL 裁剪 local STL，允许 margin。
    9. 调用 wrapCore.exe + AutoSurface，生成 local IGS / STEP patch。
    10. OCCT 导入 local patch。
    11. Viewer 叠加预览 patch，原 candidate 高亮保持。
    12. 输出 patch preview report。
    13. 用户检查 overlay 后点击 Apply。
    14. 使用原 STP boundary wire 进行 boundary-constrained patch replacement：先尝试 single-surface re-trim，失败且 all-surface coverage 成立时尝试 strict multi-surface boundary shell。
    15. 执行 sewing / ShapeFix / SameParameter。
    16. StrictTopologyGate 验证。
    17. 合法则提交 Command；不合法则 rollback。
    18. 成功后可 undo/redo。
    19. 导出最终 STEP，并做 STEP roundtrip。
```

核心原则：

```text
STP 边界负责拓扑。
STL 点集负责几何。
Geomagic 负责拟合。
OCCT 负责导入、预览、替换与验证。
用户 Apply 是 patch preview 和真实贴回之间的硬分界线。
```

---

## 2. 用户操作流程

### 2.1 加载输入

```text
File → Open STEP/STP
File → Open Original STL
```

要求：

```text
1. STP 和 STL 必须位于同一坐标系。
2. 第一版不自动对齐、不自动缩放。
3. bbox 差异过大时提示用户坐标不一致。
```

检查项：

```text
STP bbox
STL bbox
bbox center distance
bbox size ratio
```

---

### 2.2 生成候选区域

```text
Detect Feature Edges
Preview FeatureBoundedRegion Candidates
```

内部：

```text
FeatureEdgeDetector
+ UserConstraintSet
+ model boundary / topological boundary
→ protectedEdges

FeatureBoundedRegionBuilder
→ FeatureBoundedRefit candidates
```

候选显示字段：

```text
Candidate ID
Candidate Type
Face count
Boundary edge count
Protected edge count
Boundary validity
Risk level
Candidate status
Patch status
```

候选状态：

```text
Pending
Accepted
Rejected
Hidden
```

注意：

```text
Preview Candidates 不启动 Geomagic。
Candidate status 变化不修改主 ShapeDocument。
```

---

### 2.3 用户筛选候选区域

推荐交互：

```text
1. 先只显示 Top N 大候选区域。
2. 用户点击 candidate 查看 inspect 信息。
3. 用户接受少量低风险 candidate。
4. 只对 Accepted candidate 运行 Geomagic patch generation。
```

candidate 被 accepted 后，仍然不会自动替换主模型。

---

## 3. Boundary 检查

对每个 accepted candidate：

```cpp
RegionBoundaryAnalyzer.analyze(document, candidate)
```

MVP 硬要求：

```text
candidate faces 连通
outer wire count = 1
boundary closed = true
inner wire count = 0
has holes = false
has non-manifold edges = false
has branching boundary = false
```

失败处理：

```text
status = Rejected 或 HighRisk
不进入 STL crop
不进入 Geomagic
报告 failure_reason
```

通过后：

```text
BoundaryWireBuilder 根据 ordered_boundary_edges 构造 original STP outer boundary wire。
```

---

## 4. STL 局部裁剪

输入：

```text
原始 STL
candidate bbox
candidate boundary report
margin 参数
```

第一版算法：

```text
expanded_bbox = candidate_bbox.expand(max(diagonal * marginRatio, minMargin))
keep triangle if triangle_bbox intersects expanded_bbox
write local_input.stl
```

默认参数：

```text
bboxMarginRatio = 0.01
minMargin = 0.1 mm
```

输出：

```text
workspace/session_YYYYMMDD_HHMMSS/region_XXXX/local_input.stl
workspace/session_YYYYMMDD_HHMMSS/region_XXXX/crop_report.json
```

重点：

```text
local STL 的边界不是最终 CAD 边界。
local STL 只用于拟合。
最终 CAD 边界来自原 STP boundary wire。
boundary-loop coverage repair 必须保持与当前 crop mesh 连通；孤立命中点或漂浮三角片不能作为有效修补。
```

---

## 5. Geomagic AutoSurface

输入：

```text
local_input.stl
FIT_REGION_INPUT / FIT_REGION_OUTPUT / FIT_REGION_LOG_FILE
```

调用：

```text
wrapCore.exe --script autosurface_pipeline.py
```

推荐配置：

```json
{
  "input_stl": "data/crop_stl/region_0001/local_input.stl",
  "output_step": "data/crop_stp/region_0001/local_output.stp",
  "fit_region_log": "data/crop_stp/region_0001/local_output_fit_region.log",
  "repair_mesh": true,
  "fill_hole_max_edges": 80,
  "fill_hole_length_ratio": 1.0,
  "adaptive_fit": false,
  "auto_merge": true,
  "num_patches": 1,
  "fallback_num_patches": [2, 4, 8],
  "detail": 0.10,
  "geometry": "Mechanical",
  "skip_remesh": true,
  "convert_iges_to_step": true,
  "timeout_seconds": 1800
}
```

输出：

```text
data/crop_stp/<relative>/<name>.stp
data/crop_stp/<relative>/<name>_autosurface.igs   # keepTemp 时保留
autosurface_stdout.log
autosurface_stderr.log
fit_region.log
```

运行前处理：

```text
backend 和 autosurface_pipeline.py 会删除 stale STEP / IGS 输出。
IGES→STEP 转换前也会删除旧 STP。
因此 patch import 不应把上一次 AutoSurface 的旧文件误判为本次输出。
```

失败处理：

```text
记录 error_msg。
保留 stdout/stderr 和 fit_region log。
candidate job status = Failed。
不导入 patch。
不允许 Apply。
```

当前真实样例结论：

```text
RepairMesh / RemoveNonManifoldVertices / FillSmallHoles 负责处理 STL 内孔和非流形顶点。
Mechanical + autoMerge=true 是当前 crop STL 的默认最佳组合；Organic 会导致 STEP face 数量过高。
numPatches=1 只是 Geomagic AutoSurface 的近似目标，不保证最终 STEP face count 等于 1。
Remesh 默认跳过；如果显式启用 Remesh，脚本会在 Remesh 失败时记录 warning 并继续使用原 mesh，在 Remesh 后 AutoSurface 全失败时 retry pre-remesh mesh。
```

---

## 6. Patch 导入与叠加预览

优先导入：

```text
data/crop_stp/<relative>/<name>.stp
```

GUI 的“从 local STL 定位并导入 Patch”入口选择的是 local STL，不是把 STL 当 patch 导入。它会用 local STL 的 `data/crop_stl/<relative>/<name>.stl` 作为 artifact key，自动定位 `data/crop_stp/<relative>/<name>.stp` / strategy suffix / IGES fallback，并把 local STL 交给 crop boundary diagnostics。

如果失败，再尝试：

```text
<output_stp_stem>_autosurface.igs
或兼容字段中的 data/crop_igs/<relative>/<name>.igs
```

导入后生成：

```text
ImportedPatchInfo
PatchPreviewReport
```

`ImportedPatchInfo` 字段：

```text
candidate_id
patch_path
num_faces
num_edges
bbox
brep_check_valid
has_usable_face
message
```

`PatchPreviewReport` 字段：

```text
candidate id
source face count
source boundary edge count
patch path
patch face count
patch edge count
candidate bbox
patch bbox
bbox deviation
import BRepCheck result
recommended action
```

Viewer 显示：

```text
原 STP candidate 保持高亮。
imported patch 作为 overlay 显示。
用户可以显示/隐藏/清除 patch。
```

注意：

```text
patch preview 不修改主 ShapeDocument。
Patch PreviewReady 后，Apply 按钮才可启用。
```

---

## 7. Apply 与 Patch Replacement

用户确认 overlay 后点击：

```text
Apply Current Patch To Candidate
```

Apply 前检查：

```text
selected candidate 存在。
candidate status == Accepted。
patch status == PreviewReady。
boundary analysis 仍然 valid。
imported patch valid。
patch bbox 未明显异常。
```

输入：

```text
candidate source faces
original STP boundary wire
imported patch shape
```

目标路径：

```text
1. 保存 beforeDocument。
2. 从 imported patch 中提取 underlying surface / usable faces。
3. 优先使用 original STP boundary wire 构造 replacement face / patch。
4. 从原模型中替换 candidate source faces。
5. 将 replacement patch 接回原 shell。
6. 执行 ShapeFix / SameParameter / Sewing。
7. 得到临时 after ShapeDocument。
8. 送入 StrictTopologyGate。
```

禁止：

```text
不要直接把 Geomagic patch 的外边界当作最终边界。
不要直接用 STL 裁剪边界作为最终边界。
不要绕过 StrictTopologyGate 提交。
不要在 patch preview 前自动 Apply。
```

MVP 限制：

```text
单一 closed outer wire。
无 holes。
单 patch 或少量 patch。
失败直接 rollback。
```

---

## 8. StrictTopologyGate

检查：

```text
beforeStats
afterStats
BRepCheck
free boundary
multiple edge
solid count
shell closure
bbox stability
STEP export
STEP roundtrip
local STL deviation
face / edge reduction
```

硬 gate：

```text
after.hasShape == true
after.brepCheckValid == true
after.freeEdges <= before.freeEdges
after.multipleEdges <= before.multipleEdges
after.solidCount == before.solidCount
STEP export success
STEP re-import success
roundtrip.brepCheckValid == true
```

失败处理：

```text
accepted = false
rollback_applied = true
CommandContext.document 保持 beforeDocument
GUI 显示失败原因
workspace 写 validation_report.json
```

成功处理：

```text
CommandContext.document = afterDocument
candidate patch status = Applied
进入 undo 栈
允许导出最终 STEP
```

---

## 9. Workspace 规范

推荐：

```text
workspace/
  session_YYYYMMDD_HHMMSS/
    input/
      source.step
      source.stl

    region_0001/
      candidate.json
      boundary_report.json
      local_input.stl
      crop_report.json
      autosurface_stdout.log
      autosurface_stderr.log
      fit_region.log
      local_output.stp
      local_output_autosurface.igs
      patch_import_report.json
      patch_preview_report.json
      replacement_report.json
      validation_report.json
```

---

## 10. Job 状态机

```text
Pending
→ AnalyzingBoundary
→ CroppingStl
→ RunningGeomagic
→ ImportingPatch
→ PreviewReady
→ ApplyPending
→ Replacing
→ Validating
→ Applied
```

失败分支：

```text
Failed
Rejected
Cancelled
ApplyFailed
```

默认并发：

```text
maxConcurrentGeomagicJobs = 1
```

原因：

```text
wrapCore 可能占用许可证。
多进程可能冲突。
Geomagic 启动开销大。
```

---

## 11. 缓存策略

缓存 key：

```text
source STEP path + modified time
source STL path + modified time
candidate source face ids
candidate boundary edge ids
crop margin
AutoSurface parameters
autosurface_pipeline.py version
```

缓存命中条件：

```text
local STL 已存在且 crop_report success。
fit_region.log 存在且 output STEP exists。
patch import report valid。
patch preview report valid。
```

命中后：

```text
跳过 STL crop 和 Geomagic。
直接进入 patch import / overlay preview。
```

注意：

```text
缓存命中不代表可以自动 Apply。
Apply 仍需用户确认。
```

---

## 12. MVP 验收

### MVP-A：Candidate Preview

```text
1. 打开 STP。
2. 检测特征边。
3. 生成 FeatureBoundedRegion candidates。
4. 显示 candidate overlay。
5. candidate 可 accepted / rejected / hidden。
```

### MVP-B：Patch Generation + Overlay Preview

```text
1. 打开 STP。
2. 打开 STL。
3. 接受一个低风险 candidate。
4. BoundaryAnalyzer 通过。
5. 导出 local STL。
6. 调用 Geomagic 输出 local IGS / STEP。
7. 导入 patch。
8. Viewer 叠加显示 patch。
9. 输出 patch_preview_report。
10. 主 ShapeDocument 不变。
```

### MVP-C：Apply + Replacement

当前实现状态：

```text
T6.0 已完成 replacement 前置输入与 imported patch 拓扑分析。
已新增 PatchReplacementInput / PatchReplacementReport / MultiFacePatchAnalyzer。
当前可以在真正 replacement 前统一校验 document、candidate、boundary、imported patch 和 preview report。
MultiFacePatchAnalyzer 已支持 TopoDS_Face / TopoDS_Shell / TopoDS_Solid / TopoDS_Compound 形态下的 one-face 与 multi-face patch。
patchFaceCount > 1 不作为失败条件；multi-face patch 是后续 T6.1 replacement fragment 的主路径输入。
T6.2 已完成 StrictTopologyGate 最小可用版，并提前于 T6.1 / PatchReplacementCommand 落地。
StrictTopologyGate 检查 after BRepCheck、free/multiple edge 增量、solid/shell 一致性、bbox、STEP export 和 STEP roundtrip。
replacementFaceCount > 1 不作为失败条件；multi-face replacement 只记录 multiFaceReplacement 和 warning。
Gate 本身不修改 ShapeDocument，不调用 Geomagic，不重新裁剪 STL。
T6.1 已完成 BoundaryConstrainedPatchBuilder 最小可用版。
BoundaryConstrainedPatchBuilder 可从 MultiFacePatchAnalysis 构造 one-face fragment 或 multi-face compound replacement fragment，保留 internal patch seams，并仅把 boundary mismatch 作为 warning。
Builder 本身不修改 ShapeDocument，不创建 Command，不接入 GUI，不调用 Geomagic。
T6.3 已完成 PatchReplacementCommand 最小可用版：Command 会执行 input validation、MultiFacePatchAnalyzer、BoundaryConstrainedPatchBuilder、StrictTopologyGate，Gate 成功才提交 afterDocument，Gate 失败 rollback，undo/redo 复用缓存文档且 redo 不重新运行 Geomagic。
T6.3 当前未真正删除 candidate source faces，未执行 sewing / ShapeFix / SameParameter，未接入 GUI；普通局部 patch 若无法通过 StrictTopologyGate 会失败并保持主 ShapeDocument 不变。
T6.4 已完成最小 repair + gate 管线：PatchReplacementCommand 使用 BRepTools_ReShape 尝试替换 candidate source faces，然后执行 SameParameter、ShapeFix_Wire、ShapeFix_Face 和 Sewing，并记录 repair 前后拓扑统计。
T6.4 仍由 StrictTopologyGate 作为最终提交门；Gate 失败 rollback，redo 只复用缓存 afterDocument，不重新运行 Geomagic、不裁剪 STL、不读取 patch、不重新 repair。
T6.5 已完成 AppController / GUI 真实 Apply 接入：GUI 按钮调用 AppController::applyCurrentPatchToCurrentCandidate，并通过 CommandHistory 执行 PatchReplacementCommand。成功后主 ShapeDocument 显示 afterDocument 并清除 patch overlay / preview state；失败后主 ShapeDocument 不变且保留 overlay / preview state。GUI Apply 启用严格水密 Gate，要求 after 与 STEP roundtrip 后都通过 BRepCheck、solid count 保持、free edge=0、multiple edge=0。one-face path 会用 imported patch surface + 原 STP candidate boundary wire 重建 trimmed face；multi-face fragment 仍进入主路径，最终提交性由 repair + StrictTopologyGate 判断。redo 仍只复用缓存 afterDocument，不重新运行 Geomagic、不裁剪 STL、不导入 patch、不重新 repair。
T6.5.1 Apply/Gate 失败诊断增强已完成：PatchReplacementReport 记录 repair 前后 stats、Gate before/after stats、STEP roundtrip stats、BRepCheck/free/multiple edge 和 watertight 选项，GUI Patch Apply report 会直接展示这些字段；可选 rejected after debug artifact 路径尚未实现。
T6.6 Industrial Adaptive Sewing 已完成最小 C++ 集成：PatchReplacementRepair 执行 ShapeFix_Shape、ShapeUpgrade_UnifySameDomain、多 tolerance Sewing loop、ShapeFix_Shell、BRepBuilderAPI_MakeSolid、ShapeFix_Solid、collapsed guard 和 best-result selection，并把 selected tolerance、attempt count、best stats 写入 report / GUI。该路线只增强 repair 和诊断，不放宽 StrictTopologyGate，不绕过 rollback，不信任 STL crop boundary 或 Geomagic patch outer boundary。
T6.6.1 Crop Boundary Diagnostics 已完成：用原 STP candidate outer boundary loop 采样点验证 local STL crop 和 imported patch outer boundary 是否存在缺口或覆盖不足，并在 GUI overlay / patch preview report 中显示 suspected gap。
T6.6.2 Process Status Panel 已完成第一版：GUI 底部“进程”页显示 crop、Geomagic、import、replacement、repair、adaptive sewing、StrictTopologyGate 的当前阶段、candidate、路径、sewing best stats、Gate reason 和最近 message / warning；undo/redo 显示 cached undo / cached redo，不伪装成重新运行 Geomagic、crop、import 或 repair。
T6.6.3 STL Crop Boundary-Band Diagnostics 已完成：在 boundary point coverage 外增加 candidate 内侧 boundary-band coverage，并对 source STL triangles 复现当前 centroid-only crop predicate，统计 rejected near-boundary triangles 与 conservative keep candidate；GUI overlay 新增橙色 boundary-band missing area 和紫色 near-boundary rejected triangles，crop bbox 线框保留手动开关但默认隐藏，避免干扰观察 STL、原 STP loop 和诊断 segment。
T6.6.4 STL Region Extractor 保守裁剪修复已完成：crop 可在 conservative-boundary-band 模式下不只依赖 triangle centroid inside candidate region，新增 vertex-inside、edge-midpoint-inside 与 original CAD boundary-band inclusion，并用 output bbox / triangle-count leak guard 限制局部扩边。该扩边只改善 Geomagic 输入 STL 采样，不把 STL crop boundary 当作最终 CAD boundary。
T6.6.4.1 STL Crop Mode Switch 已完成：由于手动验证显示 conservative-boundary-band 可能让 AutoSurface 生成过多 patch faces 并扩大 patch boundary mismatch，默认 crop mode 已回到 legacy centroid-only；GUI `STL -> 启用保守边界裁剪` 只用于显式 A/B 验证，单独裁剪和“生成并预览当前 Patch”共用同一开关。
T6.6.5 Regenerate Geomagic Patch + Apply Verification 已完成手动验证：legacy centroid-only crop 产生较简单 patch 但 patch outer boundary 仍局部 mismatch；conservative-boundary-band crop 补齐 STL 但会让 AutoSurface patch face count 暴涨并扩大全环 boundary mismatch。两者共同证明：directly using Geomagic patch outer boundary as replacement boundary is invalid for this class of feature-bounded regions。
T6.6.4.2 STL Boundary Loop Repair Connectivity Guard 已完成：boundary-loop repair 只允许加入与当前 crop mesh 顶点近似连通的三角片；仅满足边界距离但不连通的候选会计入 orphan repair candidates 并被拒绝，避免生成漂浮碎片污染 AutoSurface。
T6.7 Boundary-Constrained Surface Re-trim 与 T6.7.4 Strict Multi-surface Boundary Shell 已完成增强版：Geomagic AutoSurface 只提供 surface / 曲面趋势，最终 CAD boundary 必须来自原 STP candidate outer boundary wire；默认 replacement 先由 selected Geomagic surface + original CAD boundary wire 重新 trim / rebuild，单 surface 不覆盖但 all-surface coverage 成立时，再由 T6.7.4 按原 CAD boundary edge 整段优先分配 surface；若整条 edge 无单一 surface 覆盖但采样点均被 surface 集合覆盖，则只对该失败 edge 分段。T6.7.4 保留 imported patch 内部 seam、构造 multi-face bounded shell；同一 patch face 形成多个 closed wire 时，所有 closed wire 都构造成 replacement face。Apply report 同时显示最佳单 surface 投影统计、all-surface 最近投影 coverage / uncovered edge ids，以及 multi-surface attempted / used / assigned segments / split edges / built faces / closed wires / open wires / failed face / failed edges。T6.7.4 失败不回退 Geomagic patch outer boundary；成功结果进入 face-compound assembly、PatchReplacementRepair 与 StrictTopologyGate。
Geomagic pipeline hygiene 已完成：backend / autosurface_pipeline.py 运行前删除 stale STEP / IGS，Remesh 默认跳过但可由 GUI Patch 菜单显式启用，并通过 FIT_REGION_* 环境变量传递 repair / remesh / AutoSurface 参数；Remesh 失败只记录 warning 后继续原 mesh，Remesh 后 AutoSurface 全失败时 retry pre-remesh mesh。
```

```text
1. 对 PreviewReady candidate 点击 Apply。
2. 执行 PatchReplacementCommand。
3. StrictTopologyGate 通过。
4. Command 可 undo/redo。
5. 导出 STEP。
6. STEP roundtrip 通过。
```

---

## 13. 最小手动验证流程

```text
STP + STL
→ Preview FeatureBoundedRefit candidates
→ Accept one low-risk candidate
→ Analyze boundary
→ Crop local STL
→ Run Geomagic AutoSurface
→ Import local STEP / IGS
→ Overlay preview patch
→ Click Apply
→ Replacement + sewing
→ StrictTopologyGate
→ Undo / Redo
→ Export STEP
→ STEP roundtrip
```

### 13.1 T6.5 后续工业级 sewing 路线

```text
T6.5.1 DONE:
ApplyFailed
→ PatchReplacementReport 展示 repair 前后 face/edge/shell/solid
→ 展示 StrictTopologyGate before/after stats
→ 展示 STEP roundtrip stats
→ 可选导出 rejected after debug STEP 尚未实现

T6.6 DONE:
assembled after candidate
→ ShapeFix_Shape
→ ShapeFix_Face orientation / ShapeFix_Wire / ShapeFix_Face
→ ShapeUpgrade_UnifySameDomain
→ adaptive BRepBuilderAPI_Sewing tolerance loop
→ ShapeFix_Shell
→ BRepBuilderAPI_MakeSolid
→ ShapeFix_Solid
→ collapsed guard / best-result selection
→ StrictTopologyGate

T6.6.1 DONE:
original STP candidate outer loop
→ per-edge boundary sampling
→ compare with local STL crop coverage
→ compare with imported patch outer boundary
→ suspected gap segment report
→ GUI overlay highlights original loop / STL coverage issue / patch outer boundary / gap
→ patch preview report includes CropBoundaryDiagnosticsReport fields

T6.6.2 DONE:
GUI process status panel
→ current stage
→ candidate id / paths / counts
→ Geomagic / crop / import / Apply params
→ adaptive sewing tolerance attempt and best stats
→ StrictTopologyGate reason
→ redo displayed as cached redo, not rerun
→ first version is stage-level; per-tolerance live progress remains a later job/progress-callback task

T6.6.3 DONE:
original STP candidate outer loop
→ candidate-inner boundary-band sampling
→ compare band samples with local STL crop coverage
→ audit source STL triangles against current bbox + centroid-inside crop predicate
→ report rejected near-boundary triangles and conservative keep candidates
→ GUI overlay highlights orange band missing area and purple rejected near-boundary triangles
→ crop bbox remains available but hidden by default for cleaner diagnostic viewing

T6.6.4 DONE:
StlRegionExtractor conservative crop mode
→ keep centroid-inside triangles as the baseline path
→ also keep bbox-intersecting triangles whose vertex or edge midpoint is inside candidate region
→ also keep bbox-intersecting triangles near the original STP candidate boundary band
→ record centroid / vertex / edge-midpoint / boundary-band keep counts in StlCropReport
→ enforce output bbox and triangle-count leak guards
→ GUI crop report shows keep/reject counts for manual recrop verification

T6.6.4.1 DONE:
STL crop mode switch
→ default crop mode is centroid-only, matching the legacy extractor behavior
→ conservative-boundary-band remains available behind a GUI STL menu toggle
→ crop reports and automatic Patch preview reports show crop mode
→ the toggle affects crop input only, not replacement, ShapeDocument, StrictTopologyGate, or redo

T6.6.4.2 DONE:
STL boundary-loop repair connectivity guard
→ boundary-loop repair candidates must connect to the currently kept crop mesh
→ orphan repair candidates are rejected and reported
→ missing-after=0 is not sufficient if it depends on floating triangles
→ crop repair still affects Geomagic input only, not final CAD boundary

T6.6.5 DONE:
manual patch regeneration and Apply verification
→ centroid-only crop keeps AutoSurface patch simple but does not make patch outer boundary CAD-compatible
→ conservative-boundary-band crop improves STL coverage but can overfit boundary wrinkles and adjacent features
→ direct multi-face AutoSurface outer-boundary replacement remains blocked by free edges and BRepCheck
→ route decision: stop treating crop / sewing tolerance as the primary fix

T6.7 DONE:
Boundary-Constrained Surface Re-trim + strict multi-surface boundary shell
→ select or fit surface geometry from the imported Geomagic patch
→ project / build original STP candidate outer boundary wire on that surface
→ rebuild a trimmed replacement face when one surface covers the boundary
→ when one surface fails but all-surface coverage passes, assign original CAD boundary edges to Geomagic surfaces, split only the edges that cannot be owned by one surface, and build a bounded multi-face shell from original boundary segments plus internal patch seams
→ when one patch face produces multiple closed wires, build every closed wire as a replacement face instead of discarding extra loops
→ assemble multi-surface results as non-source original faces plus replacement faces before repair sewing
→ report both selected single-surface coverage and all-surface nearest-projection coverage
→ report multi-surface attempted / used / assigned segments / split edges / built faces / closed wires / open wires / failed patch face / failed edge ids
→ direct patch outer-boundary fragment is no longer the default replacement path
→ final result still must pass PatchReplacementRepair and StrictTopologyGate
→ real GUI / Apply report diagnosis shows the remaining failure is split-boundary adjacency / bridge closure after multi-surface shell, not patch outer-boundary fallback or STL crop tolerance

Geomagic pipeline hygiene DONE:
→ stale STEP / IGS outputs are removed before a new AutoSurface run
→ stale STP is removed before IGES→STEP WriteFile attempts
→ Remesh is skipped by default, can be enabled explicitly, and AutoSurface failures after Remesh retry the pre-remesh mesh
→ backend passes FIT_REGION_* repair / remesh / AutoSurface parameters to the script
```

注意：

```text
adaptive sewing 不能替代 StrictTopologyGate。
free edge 变 0 但 face count 严重塌缩的结果不能提交。
multi-face patch 内部 seam 可以保留；外边界是否能形成实体由 repair + gate 判断。
redo 只复用缓存 afterDocument，不重新运行 adaptive sewing。
T6.6.1 overlay 只显示诊断几何，不参与 replacement，不改变 ShapeDocument。
T6.6.3 overlay 只显示诊断几何，不参与 crop inclusion、不参与 replacement，不改变 ShapeDocument。
T6.6.4 boundary-band inclusion 只影响 local STL crop 输入，不参与 replacement，不改变 ShapeDocument，不改变最终 CAD boundary。
T6.6.4.1 crop mode 开关只决定 local STL 采样策略，不改变 patch replacement / Apply / StrictTopologyGate 规则。
T6.6.4.2 boundary-loop repair connectivity guard 只拒绝不连通的 STL 修补候选，不用 STL 修补结果替代原 STP boundary。
T6.6.5 结论已否定“Geomagic patch outer boundary 可直接作为最终 replacement boundary”的路线。
T6.7 起，Patch Apply 的核心判断从“patch outer boundary 是否贴合”转为“Geomagic surface 是否覆盖原 STP candidate boundary，并能否用原 STP wire 重新 trim 或构造 strict multi-surface bounded shell”。
T6.7 的 all-surface coverage 是 T6.7.4 的进入条件之一，但仍不把 Geomagic patch outer boundary 或 STL crop boundary 变成最终 CAD boundary。
T6.7.4 multi-surface shell 成功不等于 Apply 成功；repair 后仍必须 free edges=0、multiple edges=0、BRepCheck=true、solid/watertight 和 STEP roundtrip 均通过。
如果 T6.6.1 / T6.6.3 证明 STL crop 或 patch outer boundary 有缺口，修复应优先发生在 crop / boundary sampling / patch generation 输入层，而不是继续放宽 Gate 或强行采用 sewing result。
Geomagic stale output cleanup 是流程正确性要求，不代表 patch 可 Apply；Apply 仍必须通过 repair 与 StrictTopologyGate。
```
