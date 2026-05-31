# STEP-PATCH-OPTIMIZER 重构方案文档

> 草案版本：v0.2-preview-then-apply  
> 用途：指导 Codex 按“预览候选 → 生成 Geomagic patch → 叠加预览 → 用户 Apply → 真实贴回”的节奏重构。  
> 目标：推进速度要快，但必须保留用户确认与 StrictTopologyGate 两个硬门槛。

---

## 1. 重构目标

把当前主线从：

```text
PlaneLike / SphereLike / CylinderLike candidate
→ OCCT 解析图元重建
→ 替换原面片
```

调整为：

```text
FeatureBoundedRegion candidate
→ 原始 STL 局部裁剪
→ Geomagic AutoSurface 生成 local IGS / STEP patch
→ OCCT 导入 patch
→ Viewer overlay 叠加预览
→ 用户点击 Apply
→ 原 STP boundary wire 约束替换
→ sewing / ShapeFix / SameParameter
→ StrictTopologyGate
→ 成功提交 Command，失败 rollback
```

最终流程：

```text
1. 输入：当前 STP + 原始 STL。
2. 在 STP 上生成 FeatureBoundedRegion 候选区域。
3. 用户预览候选区域，接受 / 拒绝 / 隐藏。
4. 对 accepted candidate 提取并验证原 STP closed boundary wire。
5. 用 candidate bbox / boundary 从原始 STL 裁剪 local STL，允许 margin。
6. 调用 wrapCore.exe + AutoSurface，生成 local IGS / STEP patch。
7. OCCT 导入局部 IGS / STEP。
8. 在 Viewer 中叠加预览 patch。
9. 用户点击 Apply。
10. 不直接使用 Geomagic patch 的边界作为最终边界，而是优先使用原 STP boundary wire 进行 trim / replace。
11. 用新 patch 替换原 candidate faces。
12. 做 sewing / ShapeFix / SameParameter。
13. 做 StrictTopologyGate：BRepCheck、free boundary、shell closure、STEP roundtrip、误差检查。
14. 合法则提交 Command；不合法则回滚。
```

---

## 2. 重构原则

### 2.1 保留

```text
STEP/STP 读写
OCCT Viewer
face/edge/candidate 选择
特征边检测
用户锁边
MergeCandidate/MergePlanner/MergeRegionGrower
Candidate GUI Preview
Command/undo/redo
ShapeValidator/STEP roundtrip
SameDomainUnifier baseline
Plane/Sphere 作为 experimental backend
```

### 2.2 改造

```text
1. MergeCandidate 新增 FeatureBoundedRefit。
2. MergePlanner 新增 FeatureBoundedRegionBuilder 通道。
3. RegionBoundaryAnalyzer 作为 patch generation 和 Apply 前的硬门槛。
4. STLRegionExtractor 负责从原 STL 裁剪 local STL。
5. GeomagicAutoSurfaceBackend 负责外部拟合。
6. PatchImportService 负责 patch 导入。
7. Patch overlay preview 负责 Apply 前可视化确认。
8. BoundaryConstrainedPatchBuilder 负责原 STP boundary wire 约束替换。
9. PatchReplacementCommand 负责真实模型修改。
10. StrictTopologyGate 负责最终提交判断。
```

### 2.3 暂停作为主线

```text
OCCT PlaneRegionMerge aggressive rebuild
OCCT Sphere/Cylinder/Cone/Torus 手工重建
OCCT 自由曲面拟合
patch 生成后自动贴回主模型
直接把 Geomagic patch sew 回主模型作为稳定方案
```

---

## 3. Codex 修改规则

```text
1. 每次只完成一个小阶段。
2. 不重构无关模块。
3. 不删除现有 Plane/Sphere 代码。
4. 新模块先 mock，再接真实 wrapCore。
5. 所有新增 .cpp 必须加入 CMake。
6. 每阶段补测试。
7. 所有破坏性操作必须走 Command。
8. Geomagic 后端不许直接修改 ShapeDocument。
9. Patch import / overlay preview 不许修改 ShapeDocument。
10. 只有用户点击 Apply 后才能进入 PatchReplacementCommand。
11. StrictTopologyGate 失败必须 rollback。
12. redo 不重新运行 Geomagic。
```

---

## 4. 分阶段重构计划

### Stage R0：文档和命名收敛

目标：

```text
统一主线命名：
FeatureBoundedRefit
STL-guided AutoSurface
Patch overlay preview
Apply
Boundary-constrained replacement
StrictTopologyGate
```

验收：

```text
文档明确：
1. 最终边界来自原 STP，不来自 STL 或 Geomagic patch 外边界。
2. patch 生成后先 overlay preview。
3. 用户点击 Apply 后才真实贴回。
```

---

### Stage R1：FeatureBoundedRegion Candidate

新增：

```text
src/merge/FeatureBoundedRegionBuilder.h/.cpp
tests/test_feature_bounded_region_builder.cpp
```

修改：

```text
src/merge/MergeCandidate.h/.cpp
src/merge/MergePlanner.h/.cpp
src/merge/MergeRegionGrower.h/.cpp
CMakeLists.txt
```

实现要求：

```text
1. 新增 MergeCandidateType::FeatureBoundedRefit。
2. 输入 protectedEdges。
3. 对 face graph 做 BFS/DFS。
4. 不跨越 protectedEdges。
5. 输出被特征线/锁边围起来的连通区域。
6. GUI 可显示并管理 candidate status。
```

验收：

```text
未保护边可跨越。
保护边不可跨越。
用户锁边生效。
候选区域状态仍可接受/拒绝/隐藏。
Candidate preview 不修改 TopoDS_Shape。
```

---

### Stage R2：Boundary Analysis + BoundaryWireBuilder

修改：

```text
src/merge/RegionBoundaryAnalyzer.h/.cpp
src/brep/BoundaryWireBuilder.h/.cpp
tests/test_region_boundary_analyzer.cpp
tests/test_boundary_wire_builder.cpp
```

MVP 通过条件：

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
open boundary、multiple loop、hole、non-manifold、branch boundary 都被拒绝，且有明确 failure reason。
ordered_boundary_edges 可构造 TopoDS_Wire。
```

---

### Stage R3：STL 输入与局部裁剪

新增：

```text
src/io/StlReader.h/.cpp
src/io/StlWriter.h/.cpp
src/stl/StlMesh.h/.cpp
src/stl/StlRegionExtractor.h/.cpp
src/stl/StlCropReport.h/.cpp
tests/test_stl_io.cpp
tests/test_stl_region_extractor.cpp
```

第一版算法：

```text
candidate bbox
→ bbox expand by margin
→ triangle bbox intersects expanded bbox
→ keep triangle
→ write local_input.stl
```

验收：

```text
读取 STL 成功。
导出 local STL 成功。
local STL triangle count > 0。
margin 增大 triangle count 不减少。
local STL bbox 覆盖 candidate bbox。
```

---

### Stage R4：Geomagic AutoSurface Backend

新增：

```text
src/external/geomagic/GeomagicAutoSurfaceConfig.h
src/external/geomagic/GeomagicAutoSurfaceResult.h
src/external/geomagic/GeomagicAutoSurfaceBackend.h/.cpp
tests/test_geomagic_backend_mock.cpp

scripts/geomagic_wrap/autosurface_pipeline.py
scripts/geomagic_wrap/autosurface_config.example.json
scripts/geomagic_wrap/README.md
```

实现要求：

```text
1. QProcess 调用 wrapCore.exe。
2. 传入 autosurface_pipeline.py 和 config.json。
3. 捕获 stdout/stderr。
4. 读取 result.json。
5. 支持 timeout。
6. 支持 mock executable。
7. 真实 Geomagic 只做手动集成测试。
```

验收：

```text
mock success 通过。
mock failure 通过。
timeout 通过。
输出文件不存在判失败。
真实 Geomagic 环境能生成 local_output.igs / local_output.step。
```

---

### Stage R5：Patch Import + Overlay Preview

新增：

```text
src/patch/ImportedPatchInfo.h
src/patch/PatchImportService.h/.cpp
src/patch/PatchPreviewReport.h/.cpp
src/patch/PatchPreviewModel.h/.cpp
tests/test_patch_import_service.cpp
```

修改：

```text
src/gui/OccViewWidget.h/.cpp
src/app/AppController.h/.cpp
src/gui/LogPanel.cpp
src/gui/ModelTreePanel.cpp
```

实现要求：

```text
1. 优先导入 local_output.step。
2. STEP 失败时尝试 local_output.igs。
3. 返回 patch shape、face count、edge count、bbox、BRepCheck。
4. Viewer 中以 overlay 显示 patch。
5. 原 candidate 高亮保留。
6. 输出 patch_preview_report。
7. 此阶段不修改 ShapeDocument。
```

验收：

```text
导入 patch 成功。
patch overlay 可显示/隐藏/清除。
清除 overlay 后主模型不变。
patch bbox 异常时标记 HighRisk 或阻止 Apply。
```

---

### Stage R6：Apply + Boundary-Constrained Replacement

新增：

```text
src/patch/BoundaryConstrainedPatchBuilder.h/.cpp
src/command/PatchReplacementCommand.h/.cpp
tests/test_boundary_constrained_patch_builder.cpp
tests/test_patch_replacement_command.cpp
```

实现要求：

```text
1. Apply 只对 PreviewReady candidate 生效。
2. Apply 前再次检查 boundary analysis 和 imported patch。
3. 使用 original STP outer boundary wire。
4. 从 imported patch 中提取可用 face/surface。
5. 构造 replacement patch。
6. 替换 candidate source faces。
7. 支持 undo/redo。
8. redo 不重新运行 Geomagic。
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
未 PreviewReady 时 Apply 禁用。
成功替换可 undo/redo。
失败时主模型不变。
```

---

### Stage R7：Sewing / ShapeFix + StrictTopologyGate

新增：

```text
src/validate/StrictTopologyGate.h/.cpp
tests/test_strict_topology_gate.cpp
```

修改：

```text
src/patch/BoundaryConstrainedPatchBuilder.cpp
src/command/PatchReplacementCommand.cpp
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
face/edge reduction。
```

验收：

```text
Gate 失败返回清晰原因。
Gate report 可写入 JSON。
Gate 失败不允许 Command 提交。
STEP roundtrip 失败时拒绝。
```

---

### Stage R8：Workspace / RegionPatchJob

新增：

```text
src/jobs/RegionPatchJob.h/.cpp
src/jobs/RegionJobManager.h/.cpp
```

状态机：

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

验收：

```text
每个 candidate 有独立 workspace。
每个阶段都有 report。
失败时能复盘。
```

---

## 5. 建议 Commit 顺序

```text
Commit 1: FeatureBoundedRefit enum/string/statistics.
Commit 2: FeatureBoundedRegionBuilder + tests.
Commit 3: MergePlanner switch + GUI/Report display.
Commit 4: GUI candidate preview route.
Commit 5: RegionBoundaryAnalyzer strengthen.
Commit 6: BoundaryWireBuilder.
Commit 7: STL mesh + reader/writer.
Commit 8: StlRegionExtractor.
Commit 9: Geomagic backend config/result/mock.
Commit 10: autosurface_pipeline.py + manual validation.
Commit 11: PatchImportService.
Commit 12: patch overlay + preview report.
Commit 13: Apply button / candidate patch status.
Commit 14: BoundaryConstrainedPatchBuilder.
Commit 15: PatchReplacementCommand.
Commit 16: Sewing / ShapeFix integration.
Commit 17: StrictTopologyGate.
Commit 18: GUI Apply flow.
Commit 19: workspace reports + RegionPatchJob.
```

---

## 6. 每阶段验证命令

```powershell
.\scripts\build_debug.ps1
.\scripts\test.ps1
```

真实 Geomagic 手动验证：

```powershell
.\scripts\run_gui.ps1
```

完整单候选验证：

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
