# STEP-PATCH-OPTIMIZER 当前 TODO：Geomagic AutoSurface → T6 replacement / fitting input 路线

> 文档定位：这是当前执行 TODO 文档，用于随开发进度持续更新、替换和勾选。  
> 长期算法路线、阶段边界、历史决策和完整设计依据请维护在 `docs/merge_algorithm_roadmap.md`。  
> 当前阶段：以 `1f0c3d7`（文档同步）为已手动验证可缝合的几何基准；`d1c00e4` Improve boundary constrained Geomagic patch flow 与 Sharp Contours 实验不进入本轮主线。
> 更新时间：2026-06-15
> 当前判断：默认路线是 `STP Sampled Candidate Surface`，速度更快且效果与 STL crop 接近；`Global Cut Chain` 是可选 STL 全局切链裁剪器，不是默认模式。旧 Stage 3A-Approx / A6 保留为 OCCT 近似平面诊断分支，不再抢占当前主线。本轮只在 `1f0c3d7` 基准上合入 GUI 后台任务、状态显示和门控优化。

---

## 当前执行判断（2026-06-11）

```text
1. 默认 Geomagic fitting input mode：
   stp-sampled-candidate-surface
   → 从当前 STP candidate faces / boundary 采样生成 fitting STL
   → 不要求先打开原始 STL
   → 当前推荐作为默认模式

2. 备用 / 诊断路线：
   legacy-stl-crop
   conservative-boundary-band-stl-crop
   Global Cut Chain STL crop（独立 STL 裁剪路线，当前不是 automatic fitting input mode）
   → 需要原始 STL
   → 用于 A/B 验证、缺面诊断或必须使用源 STL 几何采样的场景

3. Apply 边界：
   最终 CAD boundary 仍只能来自原 STP candidate outer boundary wire。
   STP sampled fitting STL、裁剪 STL、Global Cut Chain 输出和 Geomagic patch outer boundary 都不能作为最终 CAD boundary。

4. 当前真实收口问题：
   以 1f0c3d7 的可缝合结果为准。
   d1c00e4 的 boundary constrained flow 改动已导致真实样例缝合回归，
   因此不作为当前主线基准。

5. 本轮 GUI 优化边界：
   openStepFile、previewMergeCandidates 和 Patch Apply 长任务转入后台线程。
   viewer / model tree / report / Process Status 更新必须回 GUI 线程执行。
   长任务期间禁用会修改 document / controller 的入口。
   Process Status / report 展示 Gate before / after / STEP roundtrip 的 BRepCheck、free edge、multiple edge 和拓扑计数。
```

---

## 0. 历史核心判断：Stage 3A-Approx / A6

当前 Geomagic Wrap 输出的 STP 中，视觉上看似平面的区域，底层通常不是 OCCT 原生 `GeomAbs_Plane`，而是：

```text
B-spline backed planar-like surface
Bezier / BSpline surface
Geomagic Wrap 拟合出的自由曲面 patch
```

因此，T2 的 strict native Plane 限制虽然安全，但会导致：

```text
显示可真实平面合并候选为空；
PlaneLike candidate 可以预览，但真实 merge 被 ApproximateSurfaceNotSupported 拦截；
当前样例无法获得有效 face reduction。
```

所以路线调整为：

```text
保留 T1-T4 作为安全门；
暂停 T5A/T5B 原生 Plane 专用边界修复；
新增 Stage 3A-Approx：B-spline backed planar-like candidate → approximate planar rebuild。
```

核心原则：

```text
不是删除 strict mode，
而是在 strict mode 之外新增 experimental approximate planar mode。
```

---

## 1. 当前仓库状态

| 编号 | 任务                                           | 状态   | 当前结论                                                  |
| ---: | ---------------------------------------------- | ------ | --------------------------------------------------------- |
|   T1 | BRepCheck Hard Failure + Export Roundtrip Gate | DONE   | 坏结果不会进入 document                                   |
|   T2 | Strict Input Freezing                          | DONE   | 非原生 Plane 会被 ApproximateSurfaceNotSupported 拒绝     |
|   T3 | Unsafe Candidate Rejection Report              | DONE   | GUI / Report 可以显示失败原因和 document rollback 状态    |
|   T4 | RegionBoundaryAnalyzer                         | DONE   | 可在合并前分析 boundary loop / holes / closedness         |
|  T5A | Conservative Boundary Wire Rebuild             | PAUSED | 只服务原生 Plane，当前样例收益低                          |
|  T5B | Planar Face / PCurve Fix                       | PAUSED | 只服务原生 Plane，当前样例收益低                          |
|   A1 | Approx Planar Mode Options                     | DONE   | 新增近似平面合并开关                                      |
|   A2 | B-spline PlaneLike Candidate Rebuild           | DONE   | 允许低误差 B-spline backed PlaneLike 进入平面重构         |
|   A3 | Approx Boundary Rebuild using T4               | DONE   | 使用 RegionBoundaryAnalyzer 输出的 ordered boundary edges |
|   A4 | Experimental GUI Entry                         | DONE   | 提供实验性近似平面合并入口                                |
|   A5 | Tests + Export Validation                      | DONE   | 保证 BRep 合法、STEP roundtrip、失败 rollback             |
|   A6 | Approx Boundary / PCurve Stabilization         | ACTIVE | 近似平面重建前拦截不安全 boundary / pcurve 风险           |

---

## 2. 保留的安全底座

### 2.1 T1 必须保留

所有真实 merge 成功前必须经过：

```text
1. ShapeValidator
2. BRepCheck
3. STEP 临时导出
4. STEP 重新读取
5. roundtrip BRepCheck
6. solid count preserved
7. failure rollback
```

不得绕过 T1。

### 2.2 T2 strict mode 必须保留

T2 的 strict native Plane 逻辑继续存在：

```text
allow_approximate_planar_surfaces == false:
  candidate.faces 中任意 face 不是 GeomAbs_Plane
  → ApproximateSurfaceNotSupported
```

strict mode 作为安全基准，不删除、不弱化。

### 2.3 T3 报告逻辑必须保留

失败时 GUI / Report 必须继续显示：

```text
candidate_id
candidate_type
failure_reason
message
document was not modified / rollback applied
face_count_before / after
edge_count_before / after
BRepCheck
```

### 2.4 T4 RegionBoundaryAnalyzer 必须复用

Stage 3A-Approx 不重新发明 boundary 分析，应复用 T4：

```text
RegionBoundaryAnalyzer
→ strict boundary analysis
→ ordered_boundary_edges
→ valid / failure_reason / message
```

不满足 strict boundary 的 candidate 直接拒绝，不进入 planar rebuild。

---

## 3. 暂停 T5A / T5B 的原因

T5A / T5B 原本目标是：

```text
T5A：Conservative Boundary Wire Rebuild
T5B：Planar Face / PCurve Fix
```

但它们默认服务对象是：

```text
原生 GeomAbs_Plane candidate
```

当前真实样例中几乎没有原生 Plane，所以继续做 T5 的问题是：

```text
1. 能提升原生 Plane 的边界稳定性；
2. 但当前样例没有可合并原生 Plane；
3. 对当前 Geomagic Wrap STP 的 face reduction 没有直接帮助；
4. 主要瓶颈已经从 boundary fix 转为 B-spline backed planar-like rebuild。
```

因此：

```text
T5A / T5B 暂停；
其部分能力后续并入 Stage 3A-Approx 的 boundary rebuild / face fix 内部。
```

---

## 4. 历史路线：Stage 3A-Approx

### 4.1 阶段目标

新增实验性近似平面重构能力：

```text
B-spline backed PlaneLike candidate
→ 拟合目标 Geom_Plane
→ 检查 deviation
→ 使用 T4 boundary analysis
→ 重建 planar trimmed face
→ 替换 candidate face group
→ BRepCheck
→ STEP export roundtrip
→ 成功才更新 document
```

### 4.2 与 strict mode 的关系

```text
Strict mode:
  只允许原生 GeomAbs_Plane。
  最安全。
  保留作为 baseline。

Approx mode:
  允许 B-spline backed PlaneLike。
  需要通过误差阈值、边界检查、BRepCheck、STEP roundtrip。
  标记为 experimental。
```

### 4.3 不允许做

```text
1. 不删除 strict native Plane mode。
2. 不修改 SameDomainUnifier。
3. 不修改 SphereRegionMerger / CylinderRegionMerger / ConeRegionMerger / TorusRegionMerger。
4. 不修改 MergePlanner / MergeRegionGrower 的候选生成逻辑。
5. 不做 Freeform B-spline / Plate Refit。
6. 不支持多 boundary loop。
7. 不支持 holes / inner loops。
8. 不支持 disconnected boundary。
9. 不绕过 BRepCheck。
10. 不绕过 STEP roundtrip。
11. 不让失败结果污染 document。
12. 不使用大 tolerance 强行 sewing。
```

---

## 5. A1：Approx Planar Mode Options

### 5.1 目标

在 `PlaneRegionMergeOptions` 中增加实验性近似平面开关。

建议字段：

```cpp
bool allow_approximate_planar_surfaces = false;
double approximate_plane_max_deviation = 0.01;
```

可选字段：

```cpp
bool mark_approximate_planar_mode_experimental = true;
```

### 5.2 验收标准

```text
[x] 默认 allow_approximate_planar_surfaces=false。
[x] 默认行为完全保持 T2 strict mode。
[x] false 时 B-spline backed PlaneLike 仍返回 ApproximateSurfaceNotSupported。
[x] true 时 B-spline backed PlaneLike 可以进入拟合与 deviation 检查。
[x] 选项不会影响 Sphere/Cylinder/Cone/Torus。
```

---

## 6. A2：B-spline PlaneLike Candidate Rebuild

### 6.1 目标

允许几何上足够接近平面的 B-spline backed PlaneLike candidate 被重建为 planar trimmed face。

### 6.2 输入条件

必须满足：

```text
1. document.hasShape() == true。
2. candidate.valid == true。
3. candidate.candidate_type == PlaneLike。
4. candidate.status != Rejected。
5. candidate.status != Hidden。
6. candidate.face_count >= min_region_faces。
7. internal_edges 不跨 protected_edges。
8. candidate face id / edge id 有效。
9. allow_approximate_planar_surfaces == true。
```

### 6.3 几何条件

必须满足：

```text
1. estimatePlaneFromCandidate() 成功。
2. computeDeviation() 成功。
3. max_deviation <= approximate_plane_max_deviation。
4. normal compatibility 在阈值内。
5. 面片组不能明显弯曲。
```

### 6.4 失败条件

```text
1. plane fit failed → PrimitiveFitFailed。
2. deviation too large → DeviationTooLarge。
3. boundary invalid → 使用 T4 的 failure_reason。
4. surface construction failed → SurfaceConstructionFailed。
5. topology replacement failed → TopologyReplacementFailed。
6. validation failed → ValidationFailed。
7. export roundtrip failed → ExportRoundtripFailed。
```

### 6.5 验收标准

```text
[x] allow_approximate_planar_surfaces=false 时旧 strict 行为不变。
[x] allow_approximate_planar_surfaces=true 时低 deviation B-spline PlaneLike 可以进入 rebuild。
[x] 高 deviation B-spline candidate 失败。
[x] 成功后 face_count_after < face_count_before。
[x] 成功后 BRepCheck valid。
[x] 成功后 STEP roundtrip valid。
[x] 失败时 document 不变。
```

---

## 7. A3：Approx Boundary Rebuild using T4

### 7.1 目标

Approx mode 中的 boundary 构造必须复用 T4 的 RegionBoundaryAnalyzer。

推荐流程：

```text
RegionBoundaryAnalyzer::analyze(document, candidate)
  ↓
analysis.valid == true
  ↓
analysis.ordered_boundary_edges
  ↓
construct boundary wire
  ↓
BRepBuilderAPI_MakeFace(fitted Geom_Plane, boundary wire)
  ↓
BRepCheck + STEP roundtrip
```

### 7.2 严格限制

第一版只支持：

```text
1. single outer loop。
2. no holes。
3. closed boundary。
4. no disconnected boundary。
5. no non-manifold / branch boundary。
```

不支持：

```text
1. multiple outer loops。
2. inner loops / holes。
3. open boundary。
4. disconnected boundary。
5. self-intersection boundary。
```

### 7.3 验收标准

```text
[x] Approx mode 使用 RegionBoundaryAnalyzer。
[x] analysis.valid=false 时直接失败。
[x] valid 时使用 ordered_boundary_edges。
[x] invalid boundary 不进入 MakeFace。
[x] 不尝试修复复杂 boundary。
```

---

## 8. A4：Experimental GUI Entry

### 8.1 目标

为近似平面合并提供明确的实验入口，避免和 strict native Plane 合并混淆。

推荐 GUI 入口：

```text
实验性合并当前近似平面候选
实验性合并所有近似平面候选
```

如果 GUI 改动成本高，第一版可以只做：

```text
后端 API + 测试；
GUI 暂时复用现有入口，但报告中明确显示 approximate planar experimental mode。
```

### 8.2 报告要求

报告必须显示：

```text
mode: strict native plane / approximate planar experimental
allow_approximate_planar_surfaces: true/false
candidate_id
candidate_type
failure_reason
message
max_deviation
mean_deviation
rms_deviation
BRepCheck
STEP roundtrip
document state
```

### 8.3 验收标准

```text
[x] 用户能区分 strict mode 和 approx experimental mode。
[x] Approx mode 的成功/失败报告明确。
[x] 失败时显示 document was not modified / rollback applied。
[x] 不影响原有 strict “显示可平面合并候选”。
```

---

## 9. A5：Tests + Export Validation

### 9.1 必须测试

```text
[x] strict false：B-spline backed planar-like 失败，reason=ApproximateSurfaceNotSupported。
[x] approx true：低 deviation B-spline backed planar-like 成功。
[x] approx true：高 deviation B-spline backed candidate 失败。
[x] invalid boundary 失败。
[x] disconnected boundary 失败。
[ ] multiple loop / hole 如已有构造能力则失败。
[x] 成功路径经过 BRepCheck。
[x] 成功路径经过 STEP roundtrip。
[x] roundtrip failure 失败。
[x] 失败不污染 document/stats。
[x] 原生 Plane 简单合并仍成功。
[ ] command undo/redo 不破坏。
```

### 9.2 手动验证

```text
1. 打开 Geomagic Wrap 输出 STP。
2. 点击“预览合并”。
3. 确认 PlaneLike candidate 存在。
4. strict 可合并平面候选可能为空，这是正常现象。
5. 执行 experimental approximate planar merge。
6. 检查 face/edge 是否减少。
7. 检查 BRepCheck 是否通过。
8. 检查 STEP roundtrip 是否通过。
9. 导出 STEP。
10. 使用外部 CAD 打开。
11. 检查是否无缺面、飞面、无限平面、开壳。
```

外部 CAD 建议：

```text
FreeCAD
CAD Assistant
Rhino
Geomagic
SolidWorks / Fusion 360，如可用
```

---

## 10. A6：Approx Boundary / PCurve Stabilization

### 10.1 背景

```text
A1-A5 已经允许低 deviation 的 B-spline backed PlaneLike candidate 进入实验性近似平面重建。
真实样例中仍出现：face 中心/采样 deviation 很低，但 MakeFace 后 BRepCheck 失败。

这说明风险不只来自拟合平面误差，还来自 boundary edge 的 3D curve / pcurve 与拟合平面不一致。
如果直接构造 planar trimmed face，可能得到 GUI 中看似存在、导出或 BRepCheck 中不合法的面。
```

### 10.2 当前策略

```text
[x] A6.1：在 approximate planar rebuild 进入 MakeFace 前，对 ordered boundary edges 做 3D curve 到拟合平面的保守预检。
[x] A6.1：boundary 曲线采样点超出保守阈值时，返回 DeviationTooLarge，并保持 document rollback。
[x] A6.1：补充测试，覆盖 face 采样低误差但 boundary 曲线偏离拟合平面的失败路径。
```

### 10.3 后续子任务

```text
[x] A6.2：对真实失败 STP 导出 candidate / boundary 诊断信息，用于定位 pcurve / 3D curve 不一致。
[ ] A6.3：评估是否需要 ShapeFix_Wire / ShapeFix_Face / SameParameter 的最小修复路径。
[ ] A6.4：如仍需要真实合并，设计 projected boundary / pcurve rebuild；未验证前不得放宽 T1 安全门。
```

### 10.4 A6.2：真实失败样例诊断输出

目标：

```text
把“为什么这个近似平面候选不能安全重建”变成可定位的数据，而不是只看到 BRepCheck failed。
```

最小实现范围：

```text
[ ] 在 PlaneRegionMerger approximate mode 失败时，输出 candidate id / face ids / boundary edge ids。
[ ] 输出拟合平面法向、平面点、face deviation、boundary deviation。
[ ] 输出每条 boundary edge 的：
    - edge id
    - 3D curve type
    - 起点/中点/终点到拟合平面的距离
    - 是否存在 pcurve
    - pcurve 所属 face / surface type
[ ] 输出 RegionBoundaryAnalyzer 结果：
    - loop 数
    - ordered boundary edges
    - closed / disconnected / branching / non-manifold 判定
[ ] 诊断输出只写日志/报告/可选临时文本，不改变 ShapeDocument。
[ ] 补测试：诊断路径不改变 document/stats。
```

当前完成状态：

```text
[x] RegionMergeResult 新增 diagnostic_report。
[x] approximate planar merge 失败路径会输出 candidate / face / edge / plane / deviation / boundary / pcurve 诊断。
[x] data/samples/3#_底部.stp 已纳入测试；文件存在时会读取真实样例并验证诊断报告。
[x] 诊断输出不改变 result.success，不放宽 BRepCheck / STEP roundtrip。
[x] 失败时 document/stats 保持不变。
```

不做：

```text
[ ] 不修复 boundary。
[ ] 不调用 ShapeFix。
[ ] 不重建 pcurve。
[ ] 不放宽 BRepCheck / STEP roundtrip gate。
```

验收：

```text
给定一个 BRepCheck 失败的 approximate planar candidate，
报告能说明失败更像是：
1. boundary 3D curve 不在拟合平面上；
2. pcurve 缺失或不匹配；
3. boundary loop 本身不安全；
4. 其他未知问题。
```

### 10.5 A6.3：最小 ShapeFix / SameParameter 评估

目标：

```text
判断 OCCT 修复工具是否能稳定修复 A6.2 暴露的问题。
只做评估和受控实验，不把不确定修复直接作为默认成功路径。
```

候选工具：

```text
[ ] BRepLib::SameParameter
[ ] BRepLib::BuildCurves3d
[ ] ShapeFix_Wire
[ ] ShapeFix_Face
[ ] ShapeFix_Shape（仅评估，不默认全局使用）
```

评估步骤：

```text
[ ] 对 A6.2 诊断出的失败 candidate 建立最小回归样例。
[ ] 在临时 shape 上尝试 SameParameter / BuildCurves3d / ShapeFix_Wire / ShapeFix_Face。
[ ] 每一步后执行：
    - ShapeValidator
    - BRepCheck
    - STEP export
    - STEP readback
    - roundtrip ShapeValidator / BRepCheck
[ ] 记录哪些修复能稳定通过，哪些只是 GUI 看起来变好但导出仍失败。
```

不做：

```text
[ ] 不默认对所有候选套 ShapeFix。
[ ] 不隐藏 BRepCheck failure。
[ ] 不把修复失败的结果写入 document。
```

验收：

```text
明确回答：
1. SameParameter / BuildCurves3d 是否足够；
2. ShapeFix_Wire / ShapeFix_Face 是否能稳定修复真实样例；
3. 如果不稳定，必须继续进入 A6.4，而不是强行放行。
```

### 10.6 A6.4：Projected Boundary / PCurve Rebuild 设计与实现

目标：

```text
如果 A6.3 证明普通 ShapeFix 不够，则对 approximate planar rebuild 做真正的边界重建：
把候选区域外边界投影到目标平面，重新生成目标平面上的合法 pcurve / wire / face。
```

核心流程：

```text
[ ] 使用 RegionBoundaryAnalyzer 的 ordered boundary edges，禁止使用未排序原始 boundary_edges。
[ ] 对每条 boundary 3D curve 采样并投影到拟合平面。
[ ] 在目标 gp_Pln / Geom_Plane 参数域中重建 2D 曲线。
[ ] 构造新的 planar wire。
[ ] 生成 planar trimmed face。
[ ] 保持原 outer boundary 的几何连续性和方向。
[ ] 对明显会改变外形的投影距离设置 hard failure。
[ ] 执行 T1 gate：
    - ShapeValidator
    - BRepCheck
    - STEP export
    - STEP readback
    - roundtrip BRepCheck
[ ] 失败 rollback。
```

第一版限制：

```text
[ ] 只支持单一闭合 outer loop。
[ ] 不支持 holes / inner loops。
[ ] 不支持 multiple outer loops。
[ ] 不支持 disconnected boundary。
[ ] 不支持 branching / non-manifold boundary。
[ ] 不支持跨 protected / locked / feature edge。
```

验收：

```text
至少一个真实 Geomagic Wrap B-spline backed PlaneLike candidate 能：
1. 从多个 face 重建为一个 planar trimmed face；
2. face/edge 数下降；
3. BRepCheck 通过；
4. STEP roundtrip 通过；
5. 外部 CAD 打开不缺面、不飞线、不出现无限平面。
```

### 10.7 A6.2-A6.4 是否足以形成稳定基线

判断：

```text
可以形成“近似平面重建合并”的稳定基线，但前提是按 A6.2 -> A6.3 -> A6.4 顺序推进。
```

原因：

```text
1. A6.2 解决可观测性：
   不再盲目猜测 BRepCheck 失败原因。

2. A6.3 解决最小修复边界：
   如果 OCCT 内置修复足够，就避免重写 pcurve rebuild。

3. A6.4 解决真正重建路径：
   如果内置修复不够，就显式投影/重建 boundary 与 pcurve。
```

稳定基线的定义：

```text
[x] 不安全 candidate 必须稳定失败，并明确原因。
[x] 成功 candidate 必须通过 BRepCheck。
[x] 成功 candidate 必须通过 STEP roundtrip。
[x] 失败不能污染 document。
[ ] 至少一个真实 B-spline backed PlaneLike 样例稳定成功。
[ ] 外部 CAD 验证通过。
```

不能保证的内容：

```text
A6.2-A6.4 不能保证所有视觉近似平面都能合并。
它们只能建立第一条可靠合并路径：
单一闭合外环、低偏差、无洞、无分叉、boundary 可安全投影到平面的近似平面候选。

带洞、多环、复杂裁剪边界、跨特征线、外边界离拟合平面较远的候选，仍应拒绝。
```

### 10.8 验收边界

```text
A6 不是放宽安全门。
当前已完成的是 unsafe approximate boundary preflight。
它可以把部分 BRepCheck failure 提前变成 DeviationTooLarge failure，但不会保证所有 B-spline backed PlaneLike 都能合并。
```

---

## 11. 推荐实现顺序

```text
commit 1:
  docs: update TODO for Stage 3A-Approx route

commit 2:
  Stage 3A-Approx: add approximate planar options

commit 3:
  Stage 3A-Approx: keep strict mode default and allow approximate mode opt-in

commit 4:
  Stage 3A-Approx: rebuild low-deviation B-spline PlaneLike as planar trimmed face

commit 5:
  Stage 3A-Approx: reuse RegionBoundaryAnalyzer ordered boundary edges

commit 6:
  Stage 3A-Approx: add tests for strict vs approx mode

commit 7:
  Stage 3A-Approx: add experimental GUI entry/report, if needed

commit 8:
  Stage 3A-Approx: reject approximate planar candidates with unsafe boundary curves
```

---

## 12. 下一步 Codex 任务

当前下一步不是 A6.3，也不是继续扩大 STL crop tolerance，而是：

```text
T6.7.4 后续 Apply 收口：
1. 继续以 STP Sampled Candidate Surface 作为默认 Geomagic fitting input mode。
2. 用真实样例对比 STP sampled 与 Global Cut Chain STL crop 的耗时、patch face count 和 Apply report；Global Cut Chain 若用于 Geomagic，当前需要手动 / 脚本喂入输出 STL。
3. 重点处理 split boundary / 邻接旧拓扑桥接闭合，使 PatchReplacementRepair 后 free edge / multiple edge 归零。
4. 仍由 StrictTopologyGate 决定是否提交，不绕过 BRepCheck、solid/watertight 和 STEP roundtrip。
```

极简 Codex 任务边界：

```text
只做 T6.7.4 后的输入路线对齐、诊断和 Apply 收口；
不把 STP sampled fitting STL / STL crop / Global Cut Chain 边界当最终 CAD boundary；
不回退到 Geomagic patch outer-boundary replacement；
不放宽 StrictTopologyGate；
不让 redo 重新运行 Geomagic / fitting STL generation / crop / import / repair；
不写死真实样例路径或 candidate id。
```

---

## 13. 当前周报表述

```text
T6.7.4 已把 Apply 路线从直接信任 Geomagic patch outer boundary，
收敛为用原 STP candidate outer boundary wire 做 boundary-constrained replacement。

T6.7.4 之后新增两条 fitting input 路线：
1. STP Sampled Candidate Surface：当前默认，直接从 STP candidate faces / boundary 生成 fitting STL，不要求源 STL，速度更快，效果与 STL crop 接近。
2. Global Cut Chain STL crop：可选源 STL 全局切链裁剪器，用于真实 STL 几何采样和诊断，当前不是 automatic fitting input mode，也不是默认模式。

当前下一步不是继续扩大 STL crop 或 sewing tolerance，
而是让 replacement shell / repair / StrictTopologyGate 在真实样例上稳定闭合：
free edge / multiple edge 归零、BRepCheck 通过、solid/watertight 保持、STEP roundtrip 通过。
```

---

## 14. 关键结论

```text
旧 A6 近似平面路线保留为历史诊断分支。
当前主线是 Geomagic patch preview / Apply：
STP sampled fitting input 默认，Global Cut Chain STL crop 作为独立备用裁剪路线，
最终 CAD boundary 只能来自原 STP candidate outer boundary wire。
```
