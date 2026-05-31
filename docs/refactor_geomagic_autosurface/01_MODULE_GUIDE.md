# STEP-PATCH-OPTIMIZER 项目模块文档

> 草案版本：v0.2-preview-then-apply  
> 当前主线：候选区域预览 → Geomagic patch 生成 → patch overlay 叠加预览 → 用户 Apply → 真实贴回与验证。

---

## 1. 总体模块关系

```text
GUI
 ↓
AppController
 ↓
Candidate / Patch Workflow
 ├── Feature & Candidate
 ├── Boundary Analysis
 ├── STL Crop
 ├── Geomagic Backend
 ├── Patch Import / Preview
 ├── Patch Apply / Replacement
 └── StrictTopologyGate
 ↓
Command / ShapeDocument
```

硬约束：

```text
1. GUI 不直接修改 TopoDS_Shape。
2. Geomagic 后端不直接修改 ShapeDocument。
3. Patch import / overlay preview 不修改主模型。
4. 用户点击 Apply 后，才允许进入 PatchReplacementCommand。
5. 所有真实修改模型的操作必须通过 Command。
6. StrictTopologyGate 失败必须 rollback。
7. redo 不重新运行 Geomagic。
```

---

## 2. GUI 模块

目录：

```text
src/gui/
```

职责：

```text
1. 显示 STP 主模型。
2. 显示 feature/protected edges。
3. 显示 FeatureBoundedRefit candidates。
4. 管理 candidate accept / reject / hide。
5. 显示 Geomagic patch overlay。
6. 显示 patch preview report。
7. 只有 patch PreviewReady 后启用 Apply。
8. Apply 后显示 replacement / validation 报告。
```

GUI 必须区分两类预览：

```text
Candidate Preview:
    只显示 STP 上的候选区域，不启动 Geomagic，不修改主模型。

Patch Overlay Preview:
    Geomagic 已生成 patch，OCCT 已导入 patch；
    patch 作为 overlay 显示，仍不修改主模型。
```

Apply 按钮规则：

```text
1. 没有 selected candidate：禁用。
2. candidate boundary invalid：禁用。
3. patch not generated：禁用。
4. patch import failed：禁用。
5. patch PreviewReady：启用。
6. Apply 后进入 PatchReplacementCommand。
```

测试基准：

```text
1. 预览候选区域不改变 ShapeDocument。
2. 显示/隐藏 patch overlay 不改变 ShapeDocument。
3. Apply 失败后主模型不变。
4. Apply 成功后 undo/redo 可用。
```

---

## 3. AppController 模块

目录：

```text
src/app/
```

新增接口建议：

```cpp
Result openStlFile(const std::filesystem::path& path);

MergePlannerResult previewFeatureBoundedRegions(
    const MergePlannerOptions& options);

Result acceptCandidate(int candidateId);
Result rejectCandidate(int candidateId);
Result hideCandidate(int candidateId);

Result generatePatchForCandidate(int candidateId);
Result importPatchForCandidate(int candidateId);
Result previewPatchForCandidate(int candidateId);
Result clearPatchPreview(int candidateId);

Result applyPatchToCandidate(int candidateId);
```

职责边界：

```text
1. AppController 组织流程，不写复杂几何算法。
2. 生成 patch 时调用 STLRegionExtractor + GeomagicAutoSurfaceBackend + PatchImportService。
3. Apply 时创建并执行 PatchReplacementCommand。
4. AppController 不直接替换 TopoDS_Shape。
```

测试基准：

```text
1. 未加载 STP 时候选预览失败。
2. 未加载 STL 时 patch generation 失败。
3. patch import 失败时不能 Apply。
4. Apply 成功后进入 undo 栈。
```

---

## 4. Feature & Candidate 模块

目录：

```text
src/feature/
src/merge/
```

主候选类型：

```cpp
MergeCandidateType::FeatureBoundedRefit
```

职责：

```text
1. FeatureEdgeDetector 生成 feature edges。
2. UserConstraintSet 提供用户锁边。
3. protectedEdges = feature edges + locked edges + topological boundary。
4. FeatureBoundedRegionBuilder 基于 protectedEdges 分割 face regions。
5. MergePlanner 汇总 candidate。
```

执行标准：

```text
1. 不跨越 protectedEdges。
2. user locked edge 优先级最高。
3. free edge / non-manifold edge 必须作为 protected boundary。
4. Candidate status 变化不修改 B-rep。
```

测试基准：

```text
1. 未保护边可跨越。
2. 保护边不可跨越。
3. 用户锁边不可跨越。
4. 单 face region 可生成但可标记 Medium/High risk。
```

---

## 5. Boundary Analysis 模块

目录：

```text
src/merge/
src/brep/
```

核心类：

```text
RegionBoundaryAnalyzer
BoundaryWireBuilder
```

职责：

```text
1. 分析 candidate faces 是否连通。
2. 提取 boundary edges。
3. 排序 outer boundary。
4. 判断 closed outer wire。
5. 判断 holes / multiple loops / non-manifold / branch boundary。
6. 生成 ordered boundary edges。
7. BoundaryWireBuilder 构造 TopoDS_Wire。
```

MVP Apply 条件：

```text
connected_component_count == 1
outer_wire_count == 1
boundary_closed == true
inner_wire_count == 0
has_holes == false
has_non_manifold_edges == false
has_branching_boundary == false
```

测试基准：

```text
1. 单闭环通过。
2. open boundary 拒绝。
3. multiple loop 拒绝。
4. hole 拒绝。
5. branch boundary 拒绝。
```

---

## 6. STL Processing 模块

目录：

```text
src/stl/
src/io/
```

核心类：

```text
StlReader
StlWriter
StlMesh
StlRegionExtractor
StlCropReport
```

职责：

```text
1. 读取原始 STL。
2. 根据 candidate bbox + margin 裁剪 local STL。
3. 输出 crop report。
4. 写入 workspace/region_xxxx/local_input.stl。
```

执行标准：

```text
1. 第一版可用 bbox 粗裁剪。
2. local STL 应略大于 candidate。
3. STL 边界不是最终 CAD 边界。
4. 不自动缩放、不自动对齐。
```

测试基准：

```text
1. 读取 STL 成功。
2. 写出 local STL 后可再次读取。
3. margin 增大 triangle count 不减少。
4. 空裁剪结果失败。
```

---

## 7. Geomagic Backend 模块

目录：

```text
src/external/geomagic/
scripts/geomagic_wrap/
```

核心类：

```text
GeomagicAutoSurfaceConfig
GeomagicAutoSurfaceResult
GeomagicAutoSurfaceBackend
GeomagicJobCache
```

职责：

```text
1. 通过 QProcess 调用 wrapCore.exe。
2. 传入 autosurface_pipeline.py 与 config.json。
3. 输出 local_output.igs / local_output.step。
4. 输出 autosurface_result.json。
5. 捕获 stdout/stderr。
6. 支持 timeout 和 mock executable。
```

执行标准：

```text
1. 默认 Geomagic worker = 1。
2. 每个 candidate 一个 workspace。
3. 失败时保留日志。
4. 后端不接触 ShapeDocument。
```

测试基准：

```text
1. mock success。
2. mock failure。
3. timeout。
4. 输出文件缺失。
```

---

## 8. Patch Preview 模块

目录：

```text
src/patch/
src/gui/
```

核心类：

```text
PatchImportService
ImportedPatchInfo
PatchPreviewReport
PatchPreviewModel
```

职责：

```text
1. 导入 Geomagic 输出 STEP/IGS。
2. 统计 patch face count、edge count、bbox、BRepCheck。
3. 将 patch 作为 overlay 叠加在 Viewer。
4. 输出 patch_preview_report。
5. 不修改主 ShapeDocument。
```

执行标准：

```text
1. 优先导入 STEP，失败再尝试 IGS。
2. overlay 可显示/隐藏/清除。
3. patch bbox 明显异常时阻止 Apply 或标记 HighRisk。
4. preview 和 apply 是两个阶段。
```

测试基准：

```text
1. 导入 patch 不改变主模型。
2. 清除 overlay 不改变主模型。
3. 多次导入不残留旧 AIS 对象。
```

---

## 9. Patch Apply / Replacement 模块

目录：

```text
src/patch/
src/command/
```

核心类：

```text
BoundaryConstrainedPatchBuilder
PatchReplacementCommand
```

职责：

```text
1. 用户点击 Apply 后执行。
2. 使用 original STP outer boundary wire。
3. 从 imported patch 中提取可用 face/surface。
4. 构造 replacement face / patch。
5. 替换 candidate source faces。
6. 执行 sewing / ShapeFix / SameParameter。
7. 交给 StrictTopologyGate。
```

执行标准：

```text
1. 不直接使用 STL 裁剪边界作为 CAD 边界。
2. 不无条件信任 Geomagic patch 外边界。
3. 只处理单 closed outer boundary。
4. 失败必须 rollback。
5. redo 不重新运行 Geomagic。
```

测试基准：

```text
1. boundary invalid 时失败。
2. imported patch invalid 时失败。
3. Gate 失败时 document 不变。
4. 成功替换可 undo/redo。
```

---

## 10. Validation 模块

目录：

```text
src/validate/
```

核心类：

```text
ShapeValidator
StrictTopologyGate
ErrorMetric
ReportGenerator
```

StrictTopologyGate 检查：

```text
1. BRepCheck。
2. free edge 不增加。
3. multiple edge 不增加。
4. solid count 不变。
5. shell closure。
6. bbox 不异常。
7. STEP export。
8. STEP roundtrip。
9. source face count > patch face count 或局部 face count 减少。
```

执行标准：

```text
1. Gate 是提交前最后一道硬门槛。
2. Gate 失败时 PatchReplacementCommand 必须 rollback。
3. Gate report 必须可写入 JSON。
```

测试基准：

```text
1. BRepCheck false 拒绝。
2. free edge 增加拒绝。
3. solid count 改变拒绝。
4. STEP roundtrip 失败拒绝。
```

---

## 11. Jobs / Workspace 模块

目录：

```text
src/jobs/
```

核心类：

```text
RegionPatchJob
RegionJobManager
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

执行标准：

```text
1. 第一版可同步执行。
2. 必须保留状态和日志。
3. 每个 candidate 一个 workspace。
4. Geomagic 默认串行。
```

测试基准：

```text
1. 成功路径状态顺序正确。
2. 任一阶段失败进入 Failed / Rejected。
3. Cancelled 后不继续执行。
