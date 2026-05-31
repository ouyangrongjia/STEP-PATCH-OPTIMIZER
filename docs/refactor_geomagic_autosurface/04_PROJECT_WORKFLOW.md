# STEP-PATCH-OPTIMIZER 项目流程文档

> 草案版本：v0.1  
> 用途：定义从 STP + 原始 STL 输入，到局部 patch 生成、预览、替换和验证的完整流程。

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
    11. Viewer 叠加预览 patch。
    12. 用户确认后，使用原 STP boundary wire 进行 patch replacement。
    13. StrictTopologyGate 验证。
    14. 合法则提交 Command；不合法则回滚。
    15. 导出最终 STEP，并做 STEP roundtrip。
```

核心原则：

```text
STP 边界负责拓扑。
STL 点集负责几何。
Geomagic 负责拟合。
OCCT 负责替换与验证。
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
Preview Merge Candidates
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
Face count
Boundary edge count
Protected edge count
Boundary validity
Risk level
Status
```

---

### 2.3 用户筛选候选区域

候选状态：

```text
Pending
Accepted
Rejected
Hidden
```

交互建议：

```text
1. 先只显示 Top N 大候选区域。
2. 用户点击 candidate 查看 inspect 信息。
3. 用户接受少量低风险 candidate。
4. 只对 Accepted candidate 运行 Geomagic。
```

注意：

```text
Preview Candidates 不启动 Geomagic。
Accepted 也不自动替换。
```

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
```

失败处理：

```text
status = Rejected 或 HighRisk
不进入 STL crop
报告 failure_reason
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
workspace/region_XXXX/local_input.stl
workspace/region_XXXX/crop_report.json
```

重点：

```text
local STL 的边界不是最终 CAD 边界。
local STL 只用于拟合。
最终 CAD 边界来自原 STP boundary wire。
```

---

## 5. Geomagic AutoSurface

输入：

```text
local_input.stl
autosurface_config.json
```

调用：

```text
wrapCore.exe autosurface_pipeline.py autosurface_config.json
```

推荐配置：

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
  "convert_iges_to_step": true,
  "timeout_seconds": 1800
}
```

输出：

```text
local_output.igs
local_output.step
autosurface_stdout.log
autosurface_stderr.log
autosurface_result.json
```

失败处理：

```text
记录 error_msg。
保留 stdout/stderr。
candidate job status = Failed。
不导入 patch，不替换。
```

---

## 6. Patch 导入与预览

优先导入：

```text
local_output.step
```

如果失败，再尝试：

```text
local_output.igs
```

导入后生成：

```text
ImportedPatchInfo
```

字段：

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

Viewer 显示：

```text
原 STP candidate 保持高亮。
imported patch 作为 overlay 显示。
用户可以显示/隐藏 patch。
```

注意：

```text
patch preview 不修改主 ShapeDocument。
```

---

## 7. Patch Replacement

用户确认后：

```text
Replace Candidate With Patch
```

输入：

```text
candidate source faces
original STP boundary wire
imported patch shape
```

目标路径：

```text
1. 从原模型中删除 candidate source faces。
2. 从 imported patch 中提取 underlying surface / usable faces。
3. 优先使用 original STP boundary wire 构造 replacement face / patch。
4. 将 replacement patch 接回原 shell。
5. 执行 ShapeFix / SameParameter / Sewing。
6. 得到 after ShapeDocument。
```

禁止：

```text
不要直接把 Geomagic patch 的外边界当作最终边界。
不要直接用 STL 裁剪边界作为最终边界。
不要绕过 StrictTopologyGate 提交。
```

MVP 限制：

```text
单一 closed outer wire。
无 holes。
单 patch 或少量 patch。
失败直接回滚。
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
      autosurface_config.json
      autosurface_stdout.log
      autosurface_stderr.log
      autosurface_result.json
      local_output.igs
      local_output.step
      patch_import_report.json
      validation_report.json
```

---

## 10. Job 状态机

```text
Pending
→ CroppingStl
→ WaitingForGeomagic
→ RunningGeomagic
→ ImportingPatch
→ PreviewReady
→ Replacing
→ Validating
→ Accepted
```

失败分支：

```text
Failed
Rejected
Cancelled
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

缓存命中：

```text
local STL 已存在且 crop_report success。
autosurface_result success。
output patch exists。
patch import report valid。
```

命中后：

```text
跳过 STL crop 和 Geomagic。
直接进入 patch import / preview。
```

---

## 12. MVP 验收

### MVP-1

```text
1. 打开 STP。
2. 打开 STL。
3. 检测特征边。
4. 生成 FeatureBoundedRegion candidates。
5. 接受一个低风险 candidate。
6. BoundaryAnalyzer 通过。
7. 导出 local STL。
8. 调用 Geomagic 输出 local IGS / STEP。
9. 导入 patch。
10. Viewer 叠加显示 patch。
11. 输出完整日志。
```

### MVP-2

```text
1. 对单 closed boundary candidate 执行 replacement。
2. StrictTopologyGate 通过。
3. Command 可 undo/redo。
4. 导出 STEP。
5. STEP roundtrip 通过。
6. Creo 手动打开无缺面。
```
