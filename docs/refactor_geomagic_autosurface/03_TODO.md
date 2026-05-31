# STEP-PATCH-OPTIMIZER 当前阶段 TODO

> 草案版本：v0.1  
> 当前优先级：先完成 **局部 STL 裁剪 + Geomagic AutoSurface patch 生成 + patch 叠加预览**，不要立刻做真实贴回。

---

## 0. MVP 定义

### MVP-1：Patch Generation + Preview

```text
输入 STP + 原始 STL
→ 生成 FeatureBoundedRegion candidates
→ 选择一个低风险 candidate
→ RegionBoundaryAnalyzer 通过
→ 裁剪 local STL
→ wrapCore.exe + AutoSurface 生成 local IGS / STEP
→ 导入 patch
→ Viewer 叠加预览
→ 输出完整日志
```

不做：

```text
真实替换主模型
最终 STEP 导出
Creo 验证
```

### MVP-2：Single Candidate Replacement

```text
单 closed boundary candidate
→ 使用原 STP boundary wire 约束替换
→ StrictTopologyGate
→ Command undo/redo
→ STEP export/roundtrip
```

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
编译通过；GUI/报告可显示 FeatureBoundedRefit；现有 Plane/Sphere 测试不受影响。
```

---

## T1.2 新增 FeatureBoundedRegionBuilder

文件：

```text
src/merge/FeatureBoundedRegionBuilder.h/.cpp
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
free edge / model boundary 成为区域边界。
```

---

## T1.3 MergePlanner 接入新开关

文件：

```text
src/merge/MergePlanner.h/.cpp
```

新增：

```cpp
bool enable_feature_bounded_refit_candidates = true;
int min_feature_bounded_region_faces = 2;
```

验收：

```text
开关关闭时不生成。
开关开启时生成。
不破坏现有 PlaneLike/CylinderLike/SphereLike 入口。
```

---

# P0：Boundary 合法性门槛

## T2.1 强化 RegionBoundaryAnalyzer

文件：

```text
src/merge/RegionBoundaryAnalyzer.h/.cpp
tests/test_region_boundary_analyzer.cpp
```

任务：

```text
分析 candidate faces 连通性。
提取并排序 outer boundary。
判断单闭环。
判断 inner holes。
判断 non-manifold / branch boundary。
输出 failure reason。
```

验收：

```text
单闭环通过。
open boundary 拒绝。
multiple loop 拒绝。
holes 拒绝。
non-manifold boundary 拒绝。
```

---

# P0：STL 输入与裁剪

## T3.1 新增 STL 数据结构

文件：

```text
src/stl/StlMesh.h/.cpp
```

任务：

```text
定义 StlTriangle、StlMesh、bbox 统计。
```

---

## T3.2 新增 StlReader / StlWriter

文件：

```text
src/io/StlReader.h/.cpp
src/io/StlWriter.h/.cpp
```

任务：

```text
读取原始 STL。
写出 local_region_XXXX/local_input.stl。
第一版优先支持 binary STL。
```

验收：

```text
读取测试 STL 成功。
写出后再次读取成功。
triangle count 保持。
```

---

## T3.3 新增 StlRegionExtractor

文件：

```text
src/stl/StlRegionExtractor.h/.cpp
src/stl/StlCropReport.h/.cpp
tests/test_stl_region_extractor.cpp
```

任务：

```text
candidate bbox → expand margin → 裁剪相交 triangles → 输出 local STL → 输出 crop report。
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

---

## T4.2 GeomagicAutoSurfaceBackend

文件：

```text
src/external/geomagic/GeomagicAutoSurfaceBackend.h/.cpp
tests/test_geomagic_backend_mock.cpp
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

# P1：Patch 导入与叠加预览

## T5.1 PatchImportService

文件：

```text
src/patch/ImportedPatchInfo.h
src/patch/PatchImportService.h/.cpp
tests/test_patch_import_service.cpp
```

任务：

```text
导入 Geomagic 输出 STEP 或 IGS。
返回 patch shape、face count、edge count、bbox、BRepCheck。
不修改主 ShapeDocument。
```

验收：

```text
导入成功 valid=true。
导入失败不影响主模型。
bbox 和 face count 可显示。
```

---

## T5.2 Viewer patch overlay

文件：

```text
src/gui/OccViewWidget.h/.cpp
src/app/AppController.h/.cpp
```

任务：

```text
显示 imported patch overlay。
支持清除 overlay。
保持 candidate 高亮。
```

验收：

```text
patch overlay 能显示。
清除 overlay 后主模型不变。
多次导入不残留旧 AIS 对象。
```

---

# P1：日志、工作目录、缓存

## T6.1 workspace 规范

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
    validation_report.json
```

验收：

```text
失败时能根据 workspace 复盘。
```

---

## T6.2 Job 状态机

文件：

```text
src/jobs/RegionPatchJob.h/.cpp
src/jobs/RegionJobManager.h/.cpp
```

状态：

```text
Pending
CroppingStl
WaitingForGeomagic
RunningGeomagic
ImportingPatch
PreviewReady
Replacing
Validating
Accepted
Rejected
Failed
Cancelled
```

验收：

```text
成功路径状态顺序正确。
失败路径进入 Failed。
Cancelled 不继续执行后续阶段。
```

---

# P2：StrictTopologyGate

## T7.1 新增 StrictTopologyGate

文件：

```text
src/validate/StrictTopologyGate.h/.cpp
tests/test_strict_topology_gate.cpp
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
gate 失败返回清晰原因。
report 可写入 JSON。
gate 失败不允许 Command 提交。
```

---

# P2：PatchReplacementCommand MVP

## T8.1 新增 PatchReplacementCommand

文件：

```text
src/command/PatchReplacementCommand.h/.cpp
tests/test_patch_replacement_command.cpp
```

限制：

```text
只处理单 closed outer wire。
无 holes。
不处理复杂多 patch。
不处理 boundary 自交。
```

验收：

```text
成功替换可 undo/redo。
gate 失败不改变 document。
redo 不重新运行 Geomagic。
```

---

# 当前不要做

```text
不要继续增强 OCCT PlaneRegionMerge 作为主线。
不要实现 OCCT 自由曲面拟合。
不要用 STL 裁剪边界作为最终 CAD 边界。
不要默认并行启动多个 wrapCore.exe。
不要把临时 STL/IGS/STEP 提交到仓库。
不要要求第一版自动批量处理所有 candidate。
```
