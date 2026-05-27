# STEP-PATCH-OPTIMIZER 当前 TODO：Stage 3A-Fix 新分工方案

> 文档定位：这是当前执行 TODO 文档，用于随开发进度持续更新、替换和勾选。  
> 长期算法路线、阶段边界、历史决策和完整设计依据请维护在 `merge_algorithm_roadmap.md`。  
> 当前阶段：`Stage 3A-Fix：PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild`  
> 更新时间：2026-05-27

---

## 0. 当前唯一优先级

当前唯一优先级：

```text
Stage 3A-Fix：PlaneRegionMerge 导出稳定性收口
```

目标不是新增更多合并类型，而是让已有 `PlaneRegionMerge` 从：

```text
GUI 看起来能合并
```

提升为：

```text
STEP 导出后重新读取仍稳定，
外部 CAD 软件不再出现缺面、飞面、无限平面、开壳。
```

---

## 1. 本 TODO 与长期 roadmap 的关系

```text
TODO.md
  当前阶段执行清单。
  可以频繁更新、替换、勾选。
  记录当前任务拆分、状态、验收项和 Codex 投喂顺序。

merge_algorithm_roadmap.md
  长期遵循文档。
  不应大幅删减。
  保留完整阶段路线、设计原则、实现边界、历史诊断和长期规划。
```

本 TODO 不替代 roadmap。

---

## 2. 暂停事项

Stage 3A-Fix 完成前，暂停以下方向：

```text
1. 不新增 CylinderRegionMerge 真实合并。
2. 不新增 ConeRegionMerge 真实合并。
3. 不新增 TorusRegionMerge 真实合并。
4. 不扩展 SphereRegionMerge 的强合并能力。
5. 不新增 Freeform B-spline / Plate Refit。
6. 不继续扩展候选检测。
7. 不修改 SameDomainUnifier。
8. 不做大范围 GUI 重构。
```

已有 SphereRegionMerge 可以保留为实验入口，但不作为本轮主线。

---

## 3. 当前任务拆分

| 编号 | 任务                                           | 优先级 | 状态 | 目标                                         |
| ---: | ---------------------------------------------- | -----: | ---- | -------------------------------------------- |
|   T1 | BRepCheck Hard Failure + Export Roundtrip Gate |     P0 | DONE | 坏结果不能进入 document                      |
|   T2 | Strict Input Freezing                          |     P0 | DONE | 只允许原生 Plane + 简单边界进入真实合并      |
|   T3 | Unsafe Candidate Rejection Report              |     P1 | DONE | GUI / Report 明确显示拒绝原因                |
|   T4 | RegionBoundaryAnalyzer                         |     P1 | TODO | 独立分析 boundary loops / holes / closedness |
|  T5A | Conservative Boundary Wire Rebuild             |     P2 | TODO | 保守修复 edge order / orientation            |
|  T5B | Planar Face / PCurve Fix                       |     P2 | TODO | 必要时再修 ShapeFix_Face / pcurve            |

推荐执行顺序：

```text
T1 → T2 → T3 → T4 → T5A → T5B
```

最低可交付版本：

```text
完成 T1 + T2 + T3。
```

---

## 4. T1：BRepCheck Hard Failure + Export Roundtrip Gate

### 4.1 目标

将 `PlaneRegionMerger` 的成功条件升级为：

```text
1. 合并后 ShapeDocument 有效。
2. BRepCheck 通过。
3. topology usable。
4. solid count preserved。
5. face count reduced。
6. 临时 STEP 导出成功。
7. 临时 STEP 重新读取成功。
8. roundtrip BRepCheck 通过。
9. roundtrip stats 合理。
```

### 4.2 修改范围

允许修改：

```text
src/merge/PlaneRegionMerger.cpp
src/merge/PlaneRegionMerger.h
src/merge/RegionMergeResult.h
src/io/StepWriter.cpp / .h
src/io/StepReader.cpp / .h
tests/test_plane_region_merger.cpp
```

不允许修改：

```text
SameDomainUnifier
SphereRegionMerger
CylinderRegionMerger
ConeRegionMerger
TorusRegionMerger
MergePlanner
MergeRegionGrower
GUI 大结构
```

### 4.3 验收标准

```text
[x] BRepCheck invalid 时 result.success=false。
[x] BRepCheck invalid 不再 warning success。
[x] PlaneRegionMerger::merge() 成功前必须执行 STEP 导出重读。
[x] STEP 写出失败时 result.success=false。
[x] STEP 重读失败时 result.success=false。
[x] roundtrip BRepCheck invalid 时 result.success=false。
[x] 失败时 result.document 保持原 document。
[x] PlaneRegionMergeCommand 失败时不污染 CommandContext.document。
[x] 原有 plane merge 测试通过。
```

### 4.4 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 1 步：为 PlaneRegionMerger 增加 BRepCheck hard failure 和 Export Roundtrip Gate。

背景：
当前 PlaneRegionMerger::merge() 在合并后会执行 ShapeValidator / BRepCheck，但 BRepCheck invalid 仍可能以 warning 形式返回 success=true。现在需要将 PlaneRegionMerge 的成功标准提升为导出级合法性闭环。

请严格按以下要求修改：

1. 只修改 PlaneRegionMerger 相关代码、必要的 STEP IO 复用代码和测试。
2. 不修改 SameDomainUnifier。
3. 不修改 SphereRegionMerger / CylinderRegionMerger / ConeRegionMerger / TorusRegionMerger。
4. 不修改 MergePlanner / MergeRegionGrower。
5. 不修改候选检测、候选预览、GUI 大结构。
6. PlaneRegionMerger::merge() 中：
   - 合并后先构造 ShapeDocument。
   - 执行 ShapeValidator。
   - 如果 validation.brep_check_valid == false，则返回 success=false。
   - 不允许再出现 “BRepCheck warning but success” 的语义。
7. 增加导出重读验证：
   - 将 merged shape 写入临时 STEP 文件。
   - 重新读取该临时 STEP 文件。
   - 对重读后的 ShapeDocument 再执行 ShapeValidator。
   - 如果写出失败、重读失败、roundtrip BRepCheck 失败，则返回 success=false。
8. 失败时 result.document 必须保持原始 document，不允许把坏模型写入 result.document。
9. 成功时 result.message 中说明 export roundtrip validation passed。
10. PlaneRegionMerger::mergeBatch() 也要走同样验证逻辑。
11. 保持现有 Command undo/redo 语义：Command 层只在 result.success==true 时更新 context.document。
12. 新增或修改测试：
    - BRepCheck invalid 不再 success。
    - export roundtrip 失败时 merge 返回 failure。
    - failure 不污染原 document stats。
    - 原有 plane region merge 正常样例仍通过。
13. 不要顺手重构无关代码。
14. 不要格式化无关文件。
15. 修改完成后运行现有测试，并只修复与本任务直接相关的问题。

完成后请总结：
- 修改了哪些文件；
- 新增了哪些失败条件；
- 新增/修改了哪些测试；
- 是否存在仍需外部软件人工验证的项目。
```

---

## 5. T2：Strict Input Freezing

### 5.1 目标

Stage 3A-Fix 期间临时收紧真实合并输入范围。

只允许：

```text
1. 原生 GeomAbs_Plane。
2. 单 outer wire。
3. 无 inner loop。
4. boundary wire 闭合。
5. 不跨 protected edge。
6. 不跨 locked edge。
7. 合并后 solid count preserved。
```

明确拒绝：

```text
1. B-spline backed planar-like candidate。
2. 多边界环。
3. 有洞。
4. 边界不闭合。
5. candidate 引用不存在 face / edge id。
```

### 5.2 验收标准

```text
[x] 原生 Plane 简单候选仍可合并。
[x] B-spline backed planar-like candidate 被拒绝。
[x] 多边界环被拒绝。
[x] 有洞候选被拒绝。
[x] 边界不闭合候选被拒绝。
[x] 失败时 document 不变。
[x] failure_reason 与 message 明确。
```

### 5.3 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 2 步：收紧 PlaneRegionMerger 的真实合并输入范围。

背景：
当前 PlaneRegionMerger 支持 NURBS-backed planar region，但 Stage 3A-Fix 当前优先目标是导出稳定性。因此真实合并入口必须临时冻结输入范围，只允许原生 Plane 且边界简单的候选进入真实重建。

请严格按以下要求修改：

1. 只修改 PlaneRegionMerger、RegionMergeResult / RegionMergeFailureReason 和相关测试。
2. 不修改 SameDomainUnifier。
3. 不修改 SphereRegionMerger。
4. 不修改 Candidate 生成逻辑；近似平面仍然可以被生成和预览，但真实 merge 必须拒绝。
5. 在 PlaneRegionMerger 的 candidate 准备阶段增加严格检查：
   - candidate.faces 中每个 face 的 BRepAdaptor_Surface::GetType() 必须是 GeomAbs_Plane。
   - 如果任一 face 不是 GeomAbs_Plane，返回 failure。
   - failure_reason 使用 ApproximateSurfaceNotSupported；如果该枚举不存在，请新增。
   - message 说明：B-spline backed planar-like candidate is preview-only and not supported by strict PlaneRegionMerge.
6. 增加边界约束检查：
   - 只允许单 outer wire。
   - 不允许 inner loop / holes。
   - boundary wire 必须闭合。
   - 多边界环、有洞、不闭合时必须返回明确 failure_reason 和 message。
7. 不要尝试在本任务中修复复杂 boundary。
   - 本任务只负责拒绝不安全输入。
   - Safe Boundary Rebuild 留给后续任务。
8. 修改测试：
   - 原生 Plane 两面合并仍成功。
   - NURBS-backed planar region 现在应失败，并返回 ApproximateSurfaceNotSupported。
   - invalid boundary 仍失败且不污染 stats。
   - 新增多环 / 有洞 / 不闭合测试；如果构造成本过高，先覆盖不闭合和非原生 Plane。
9. 不要修改 GUI。
10. 不要顺手重构无关代码。
11. 修改完成后运行测试。

完成后请总结：
- 哪些输入现在被拒绝；
- 哪些历史测试语义发生变化；
- 哪些复杂 boundary 仍留给后续任务。
```

---

## 6. T3：Unsafe Candidate Rejection Report

### 6.1 目标

让 GUI / ReportPanel 明确显示失败原因。

报告至少包含：

```text
candidate_id
candidate_type
success / failure
failure_reason
message
face_count_before / after
edge_count_before / after
brep_check_valid
是否 rollback / document not modified
```

### 6.2 验收标准

```text
[x] failure_reason 有稳定字符串输出。
[x] 近似平面拒绝时报告显示 preview-only。
[x] 多环拒绝时报告显示 multiple boundary loops not supported。
[x] 有洞拒绝时报告显示 inner loops not supported。
[x] 导出重读失败时报告显示 export roundtrip failed。
[x] 失败时报告说明 document not modified / rollback applied。
```

### 6.3 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 3 步：让 PlaneRegionMerge 的失败原因在 GUI / 报告中明确显示。

背景：
Stage 3A-Fix 会新增更多拒绝路径，例如近似平面不支持、多边界环不支持、有洞不支持、导出重读失败等。用户需要在 GUI 报告面板中看到明确原因，而不是只看到 generic merge failed。

请严格按以下要求修改：

1. 只修改报告展示相关代码和必要的 result 字段辅助函数。
2. 不修改 PlaneRegionMerger 的几何算法。
3. 不修改 SameDomainUnifier。
4. 不修改 SphereRegionMerger。
5. 检查当前 MainWindow / AppController 中输出 RegionMergeResult 的位置。
6. 为 RegionMergeFailureReason 增加一个稳定的字符串转换函数，例如：
   - toString(RegionMergeFailureReason)
   - 或 regionMergeFailureReasonToString()
7. 报告中至少显示：
   - candidate_id
   - candidate_type
   - success / failure
   - failure_reason 字符串
   - result.message
   - face_count_before / after
   - edge_count_before / after
   - brep_check_valid
8. 当 result.success == false 时，报告明确说明 document was not modified / rollback applied。
9. 不要改变 Command 层成功/失败语义。
10. 不要新增大面积 GUI 重构。
11. 修改或新增测试：
   - 如果已有 GUI 测试困难，可以给纯函数 toString 增加测试。
   - 确保新增枚举都有字符串输出。
12. 不要顺手格式化无关文件。

完成后请总结：
- 新增了哪些 failure reason 显示；
- 报告中现在会输出哪些字段；
- 哪些 GUI 行为没有修改。
```

---

## 7. T4：RegionBoundaryAnalyzer

### 7.1 目标

新增独立边界分析模块，避免 PlaneRegionMerger 直接依赖脆弱的 `makeBoundaryWire()`。

新增：

```text
src/merge/RegionBoundaryAnalyzer.h
src/merge/RegionBoundaryAnalyzer.cpp
tests/test_region_boundary_analyzer.cpp
```

### 7.2 建议输出结构

```cpp
struct RegionBoundaryAnalysis {
    bool valid = false;

    int connected_component_count = 0;
    int outer_wire_count = 0;
    int inner_wire_count = 0;

    bool boundary_closed = false;
    bool has_holes = false;
    bool has_non_manifold_edges = false;

    std::vector<EdgeId> ordered_boundary_edges;
    std::vector<std::vector<EdgeId>> boundary_loops;

    RegionMergeFailureReason failure_reason = RegionMergeFailureReason::None;
    std::string message;
};
```

### 7.3 第一版 strict mode

只允许：

```text
connected_component_count == 1
outer_wire_count == 1
inner_wire_count == 0
boundary_closed == true
has_non_manifold_edges == false
```

其他情况只拒绝，不修复。

### 7.4 验收标准

```text
[ ] 简单闭合边界 valid。
[ ] 缺失边界 invalid。
[ ] 多 loop invalid。
[ ] 有洞 invalid。
[ ] non-manifold invalid。
[ ] PlaneRegionMerger 使用分析结果拒绝危险候选。
[ ] 原有简单平面合并仍通过。
```

### 7.5 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 4 步：新增 RegionBoundaryAnalyzer，用于在 PlaneRegionMerger 执行真实重建前分析候选区域边界。

背景：
当前 PlaneRegionMerger 直接用 candidate.boundary_edges 调用 makeBoundaryWire()，边界排序、闭合、多环、有洞等情况缺少独立诊断。Stage 3A-Fix 需要将不安全候选明确拒绝，并给出原因。

请严格按以下要求修改：

1. 新增 RegionBoundaryAnalyzer.h / RegionBoundaryAnalyzer.cpp。
2. 新增 tests/test_region_boundary_analyzer.cpp，并接入 CMakeLists.txt。
3. 不修改 MergePlanner / MergeRegionGrower。
4. 不修改 SameDomainUnifier。
5. 不修改 SphereRegionMerger。
6. RegionBoundaryAnalyzer 输入：
   - const ShapeDocument&
   - const MergeCandidate&
7. RegionBoundaryAnalyzer 输出结构包含：
   - valid
   - connected_component_count
   - outer_wire_count
   - inner_wire_count
   - boundary_closed
   - has_holes
   - has_non_manifold_edges
   - ordered_boundary_edges
   - boundary_loops
   - failure_reason
   - message
8. 第一版只支持 strict mode：
   - connected_component_count == 1
   - outer_wire_count == 1
   - inner_wire_count == 0
   - boundary_closed == true
   - has_non_manifold_edges == false
9. 不要在本任务中做 ShapeFix_Wire / ShapeFix_Face。
10. 不要尝试修复复杂边界；只分析并返回明确失败原因。
11. PlaneRegionMerger 在 preparePlaneMerge() 中调用 RegionBoundaryAnalyzer：
   - 如果 analysis.valid == false，直接 fail(result, analysis.failure_reason, analysis.message)。
   - 如果 valid，则使用 analysis.ordered_boundary_edges 构造 wire。
12. 新增测试覆盖：
   - simple closed boundary success
   - missing boundary edge failure
   - multiple loops failure，如构造复杂则先用 synthetic candidate 覆盖
   - invalid edge id failure
13. 保持现有 PlaneRegionMerger 简单样例测试通过。
14. 不要顺手重构无关代码。

完成后请总结：
- 新增了哪些文件；
- RegionBoundaryAnalyzer 当前能识别哪些风险；
- 哪些复杂情况仍未修复，只会拒绝。
```

---

## 8. T5A：Conservative Boundary Wire Rebuild

### 8.1 目标

在 RegionBoundaryAnalyzer 通过后，对安全边界做保守 wire 构造：

```text
1. 使用 ordered_boundary_edges。
2. 确保 edge 方向连续。
3. 必要时使用 edge.Reversed()。
4. ShapeFix_Wire 只做保守修复。
5. 修复失败则拒绝，不继续 MakeFace。
```

### 8.2 验收标准

```text
[ ] reversed boundary edge 能被正确处理。
[ ] discontinuous boundary 被拒绝。
[ ] simple plane merge 仍成功。
[ ] export roundtrip gate 仍生效。
```

### 8.3 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 5A 步：对 PlaneRegionMerger 的 boundary wire 构造做保守修复。

背景：
RegionBoundaryAnalyzer 已经负责识别安全边界。本任务只在安全边界上改进 wire 构造，不处理多环、有洞、不闭合等复杂情况。

请严格按以下要求修改：

1. 只修改 PlaneRegionMerger 中 boundary wire 构造相关函数，或新增局部 helper。
2. 不修改 SameDomainUnifier。
3. 不修改 SphereRegionMerger。
4. 不新增 Cylinder/Cone/Torus 真实合并。
5. 使用 RegionBoundaryAnalyzer 提供的 ordered_boundary_edges。
6. 构造 wire 时确保：
   - 前一条 edge 的 end vertex 与后一条 edge 的 start vertex 一致。
   - 如果方向相反，则使用 edge.Reversed()。
   - 如果无法连续，返回 BoundaryLoopInvalid。
7. 调用 ShapeFix_Wire 时必须保守：
   - 不使用大 tolerance。
   - 不吞掉失败。
   - 修复后仍必须检查 wire.Closed()。
8. 如果 ShapeFix_Wire 后 wire 不闭合或异常，返回 failure，不继续 MakeFace。
9. 不要在本任务中重建 pcurve。
10. 保持 export roundtrip gate 仍然生效。
11. 新增或修改测试：
    - reversed boundary edge 能被正确处理。
    - discontinuous boundary 被拒绝。
    - simple plane merge 仍成功。
12. 不要顺手重构无关代码。

完成后请总结：
- wire 构造逻辑如何变化；
- 哪些情况会被修复；
- 哪些情况仍会被拒绝。
```

---

## 9. T5B：Planar Face / PCurve Fix

### 9.1 目标

在 T1-T5A 稳定后，再做：

```text
1. ShapeFix_Face。
2. BRepLib::BuildCurves3d。
3. planar pcurve 缺失检查。
4. 必要时重建 planar pcurve。
```

### 9.2 前置条件

必须满足：

```text
[ ] T1 已完成。
[ ] T2 已完成。
[x] T3 已完成。
[ ] T4 已完成。
[ ] T5A 已完成。
[ ] 仍存在真实 STEP 样例导出后外部软件异常。
```

### 9.3 验收标准

```text
[ ] 修复后 face 非空。
[ ] 修复后只有一个 wire。
[ ] 修复后 wire closed。
[ ] 修复后 BRepCheck valid。
[ ] export roundtrip valid。
[ ] 修复失败时 document 不变。
```

### 9.4 Codex Prompt

```text
你正在修改 STEP-PATCH-OPTIMIZER 仓库。

当前任务是 Stage 3A-Fix 的第 5B 步：对 PlaneRegionMerger 新建 planar face 做保守的 face / curve 修复。

背景：
当前已经完成 Export Roundtrip Gate、Strict Input Freezing、RegionBoundaryAnalyzer 和 Conservative Wire Fix。本任务只处理简单单外环原生 Plane 合并后的 face 修复，不支持复杂多环、有洞或近似平面。

请严格按以下要求修改：

1. 只修改 PlaneRegionMerger 的新 face 构造后处理逻辑。
2. 不修改 SameDomainUnifier。
3. 不修改 SphereRegionMerger。
4. 不新增 Cylinder/Cone/Torus 真实合并。
5. 在 BRepBuilderAPI_MakeFace 成功后，对 mergedFace 做保守修复：
   - 可使用 ShapeFix_Face。
   - 可使用 BRepLib::BuildCurves3d。
   - 必须保持 tolerance 保守，不允许大幅放宽。
6. 修复后必须重新检查：
   - face 非空。
   - 只有一个 wire。
   - wire closed。
   - BRepCheck valid。
7. 如果修复失败，返回 SurfaceConstructionFailed 或 ValidationFailed，不继续替换拓扑。
8. 不要强行修复多环、有洞、不闭合边界；这些应在前置阶段直接拒绝。
9. 保持 export roundtrip gate 作为最终成功条件。
10. 新增测试：
    - 修复后 simple plane merge 仍通过。
    - 修复失败路径不会污染 document。
    - export roundtrip 仍通过。
11. 不要顺手重构无关代码。

完成后请总结：
- 使用了哪些 OCCT 修复 API；
- 修复失败时如何回退；
- 是否仍需要真实 STEP 样例人工验证。
```

---

## 10. 推荐 commit 顺序

```text
commit 1: Stage 3A-Fix: make PlaneRegionMerge fail on invalid BRepCheck
commit 2: Stage 3A-Fix: add export roundtrip gate for PlaneRegionMerge
commit 3: Stage 3A-Fix: reject non-native planar candidates in strict mode
commit 4: Stage 3A-Fix: report unsafe candidate rejection reasons
commit 5: Stage 3A-Fix: add RegionBoundaryAnalyzer
commit 6: Stage 3A-Fix: integrate RegionBoundaryAnalyzer into PlaneRegionMerger
commit 7: Stage 3A-Fix: conservative boundary wire rebuild
commit 8: Stage 3A-Fix: conservative planar face and curve repair
```

---

## 11. 当前周报可交付版本

本周最低可汇报目标：

```text
Stage 3A-Fix v1：PlaneRegionMerge Export-Stable Gate
```

包含：

```text
1. BRepCheck invalid hard failure。
2. STEP export roundtrip validation。
3. roundtrip BRepCheck。
4. solid count preserved。
5. failure rollback。
6. result.message 明确说明失败原因。
7. 原有测试通过。
```

周报表述：

```text
本周将 PlaneRegionMerge 的验收标准从 GUI 层显示正确提升为 STEP 导出级稳定。
合并结果必须通过 BRepCheck、临时 STEP 导出、重新读取和二次验证。
若任意环节失败，系统自动回滚，不污染当前 ShapeDocument。
```

---

## 12. 完成后再推进

Stage 3A-Fix 通过后，再考虑：

```text
1. 将 export roundtrip gate 复用于 SphereRegionMerge。
2. 评估 CylinderRegionMerge 推进条件。
3. Stage 4：Freeform Candidate Detection。
4. Stage 5：Freeform B-spline / Plate Refit。
5. ConeRegionMerge 后置。
6. TorusRegionMerge 可选。
```
