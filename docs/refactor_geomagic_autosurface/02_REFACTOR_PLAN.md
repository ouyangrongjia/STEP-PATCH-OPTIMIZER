# STEP-PATCH-OPTIMIZER 重构方案文档

> 草案版本：v0.1  
> 用途：指导 Codex 分阶段重构，不破坏现有功能。

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
→ Geomagic AutoSurface 生成少量 NURBS patch
→ OCCT 导入 patch
→ 原 STP boundary wire 约束替换
→ StrictTopologyGate
```

最终流程：

```text
1. 输入：当前 STP + 原始 STL。
2. 在 STP 上生成 FeatureBoundedRegion 候选区域。
3. 每个候选区域必须提取原 STP 的 closed boundary wire。
4. 用候选区域 bbox / boundary 从原始 STL 中裁剪局部 mesh，允许 margin。
5. 调用 wrapCore.exe + AutoSurface，对局部 STL 生成局部 IGS / STEP patch。
6. OCCT 导入局部 IGS / STEP。
7. 不直接使用 Geomagic patch 的边界作为最终边界，而是优先使用原 STP boundary wire 进行 trim / replace。
8. 用新 patch 替换原 candidate faces。
9. 做 StrictTopologyGate：BRepCheck、free boundary、shell closure、STEP roundtrip、误差检查。
10. 合法则提交 Command；不合法则回滚。
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
3. RegionBoundaryAnalyzer 作为真实替换前硬门槛。
4. STLRegionExtractor 负责从原 STL 裁剪 local STL。
5. GeomagicAutoSurfaceBackend 负责外部拟合。
6. PatchImportService 负责 patch 导入与预览。
7. StrictTopologyGate 负责最终提交判断。
```

### 2.3 暂停作为主线

```text
OCCT PlaneRegionMerge aggressive rebuild
OCCT Sphere/Cylinder/Cone/Torus 手工重建
OCCT 自由曲面拟合
直接把 Geomagic patch sew 回主模型
```

---

## 3. Codex 修改规则

```text
1. 每次只完成一个阶段。
2. 不重构无关模块。
3. 不删除现有 Plane/Sphere 代码。
4. 新模块先 mock，再接真实 wrapCore。
5. 所有新增 .cpp 必须加入 CMake。
6. 每阶段补测试。
7. 所有破坏性操作必须走 Command。
8. Geomagic 后端不许直接修改 ShapeDocument。
9. StrictTopologyGate 失败必须 rollback。
10. redo 不重新运行 Geomagic。
```

---

## 4. 分阶段重构计划

### Stage R0：文档和命名收敛

目标：

```text
统一主线命名：FeatureBoundedRefit / STL-guided AutoSurface / Boundary-constrained replacement。
```

修改：

```text
docs/
README 可选
```

验收：

```text
文档明确：最终边界来自原 STP，不来自 STL 或 Geomagic patch 外边界。
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
```

验收：

```text
未保护边可跨越；保护边不可跨越；用户锁边生效；候选区域状态仍可接受/拒绝/隐藏。
```

---

### Stage R2：RegionBoundaryAnalyzer 强化

修改：

```text
src/merge/RegionBoundaryAnalyzer.h/.cpp
tests/test_region_boundary_analyzer.cpp
```

MVP 通过条件：

```text
connected_component_count == 1
outer_wire_count == 1
boundary_closed == true
inner_wire_count == 0
has_holes == false
has_non_manifold_edges == false
```

验收：

```text
open boundary、multiple loop、hole、non-manifold 都被拒绝，且有明确 failure reason。
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
读取 STL 成功；导出 local STL 成功；local STL triangle count > 0；margin 增大 triangle count 不减少。
```

---

### Stage R4：Geomagic AutoSurface Backend

新增：

```text
scripts/geomagic_wrap/autosurface_pipeline.py
scripts/geomagic_wrap/autosurface_config.example.json
src/external/geomagic/GeomagicAutoSurfaceConfig.h
src/external/geomagic/GeomagicAutoSurfaceResult.h
src/external/geomagic/GeomagicAutoSurfaceBackend.h/.cpp
tests/test_geomagic_backend_mock.cpp
```

后端输入：

```json
{
  "input_stl": "workspace/region_0001/local_input.stl",
  "output_igs": "workspace/region_0001/local_output.igs",
  "output_step": "workspace/region_0001/local_output.step",
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
mock success / failure / timeout 都通过；真实 Geomagic 可手动跑通；result.json 可解析。
```

---

### Stage R5：Patch Import 与 Overlay Preview

新增：

```text
src/patch/ImportedPatchInfo.h
src/patch/PatchImportService.h/.cpp
src/patch/PatchPreviewModel.h/.cpp
tests/test_patch_import_service.cpp
```

GUI 接入：

```text
导入 local_output.step 或 local_output.igs
显示 patch overlay
不修改主 ShapeDocument
```

验收：

```text
patch 能显示、能清除、主模型不变、日志显示 patch face count/bbox。
```

---

### Stage R6：StrictTopologyGate

新增：

```text
src/validate/StrictTopologyGate.h/.cpp
tests/test_strict_topology_gate.cpp
```

硬 gate：

```text
BRepCheck valid
free edge 不增加
multiple edge 不增加
solid count 不变
shell closure 不破坏
bbox 不异常
STEP export 成功
STEP re-import 成功
roundtrip BRepCheck valid
```

验收：

```text
任一硬 gate 失败则 rejected；report 可 JSON 化；失败不提交 document。
```

---

### Stage R7：PatchReplacementCommand MVP

新增：

```text
src/command/PatchReplacementCommand.h/.cpp
src/patch/BoundaryConstrainedPatchBuilder.h/.cpp
tests/test_patch_replacement_command.cpp
```

MVP 限制：

```text
1. 只处理单 closed outer wire。
2. 无 holes。
3. 单 patch 或少量 patch。
4. replacement 失败直接 rollback。
```

验收：

```text
成功替换可 undo/redo；Gate 失败不改变 document；redo 不重新运行 Geomagic。
```

---

### Stage R8：Job Manager、日志、缓存

新增：

```text
src/jobs/RegionPatchJob.h/.cpp
src/jobs/RegionJobManager.h/.cpp
```

状态机：

```text
Pending → CroppingStl → WaitingForGeomagic → RunningGeomagic → ImportingPatch → PreviewReady → Replacing → Validating → Accepted
失败分支：Failed / Rejected / Cancelled
```

验收：

```text
每个 candidate 一个 workspace；默认 Geomagic 串行；缓存命中不重复跑 wrapCore；日志完整。
```

---

## 5. 推荐提交顺序

```text
Commit 1: FeatureBoundedRefit candidate type + builder tests
Commit 2: RegionBoundaryAnalyzer 强化
Commit 3: STL reader/writer + bbox cropper
Commit 4: Geomagic backend config/result/mock runner
Commit 5: autosurface_pipeline.py
Commit 6: PatchImportService + overlay preview
Commit 7: StrictTopologyGate
Commit 8: PatchReplacementCommand MVP behind experimental flag
Commit 9: RegionJobManager + logs/cache
Commit 10: GUI workflow and docs update
```

---

## 6. 验证命令

每阶段必须运行：

```powershell
.\scriptsuild_debug.ps1
.\scripts	est.ps1
```

GUI 手动验证：

```powershell
.\scriptsun_gui.ps1
```

第一版测试不能依赖真实 Geomagic。真实 Geomagic 只作为手动集成验证。
