# 已完成功能与历史路线归档

> 文档定位：记录已经完成、已经验证或已经退役的功能事实。当前待办只放在 `docs/TODO.md`。

更新时间：2026-07-14

---

## 1. Geomagic 稳定基线能力

以下能力已经在 `feature-bounded-refit` 形成 Geomagic AutoSurface patch preview / Apply 稳定基线，并作为 `native-surface-refit` 分支的后备和对照路线：

```text
STEP/STP
→ FeatureBoundedRefit 候选区域
→ STP Sampled Candidate Surface fitting STL
→ Geomagic AutoSurface
→ patch import / preview
→ original STP boundary constrained Apply
→ PatchReplacementRepair
→ StrictTopologyGate
→ STEP export / readback
→ optional external CAD diagnostics
```

硬边界：

```text
1. 最终 CAD boundary 只能来自原 STP candidate outer boundary wire。
2. STP sampled fitting STL、STL crop boundary、support collar、Geomagic patch outer boundary 都不能作为最终 CAD boundary。
3. `PatchReplacementRepair` 和 `StrictTopologyGate` 不能绕过。
4. redo 只能复用缓存 afterDocument，不能重跑 Geomagic / crop / import / repair。
5. 真实样例路径和 candidate ordinal 不能写进默认测试。
```

---

## 2. 基础应用闭环

已完成：

- STEP/STP 读取、OCCT Viewer 显示、B-rep 拓扑索引。
- face / edge 命中选择、Shift 多选、Ctrl 移除选择。
- 特征边检测、用户锁边 / 解锁、锁边高亮。
- same-domain 合并、undo / redo、基础合法性检查。
- STEP/STP 导出和导出后二次读取校验。
- 打开 STEP、导出 STEP、候选预览和 Patch Apply 的后台任务化。
- Process Status Panel 展示长任务阶段、repair / gate / roundtrip 关键指标。

### 2.1 测试与实验运行归档

已完成：

- 新运行统一进入 `data/runs/<时间>__<流程>__<模型>__cand-<四位编号>__<变体>/`。
- 每轮固定生成 `run.json`、`RUN.md`、`artifacts/`、`diagnostics/`、`logs/`。
- `run.json` 记录 Git 快照、源文件哈希、入口参数、阶段结果和关键产物路径。
- `scripts/test.ps1` 保存 build / ctest 日志，不再只有控制台结论。
- `scripts/list_runs.ps1` 支持按流程、模型、候选、状态和最近数量查询。
- baseline、Route 2、Creo ModelCHECK 和 Creo Toolkit runner 已接入统一运行清单。
- 旧 `data/baseline_runs` 保持历史只读；共享 `data/crop_*` 仍为兼容暂存区，不能作为历史验收来源。

### 2.2 NSR-1 原生单曲面探针工程闭环

已完成：

- `SurfaceRefitter::fitSingleFace` 使用原始有序边界 G0 约束和原始 STEP 内部点生成一张拟合面。
- 训练样本与独立验证样本分离。
- 报告边界、角点、内部距离、法向、Jacobian、曲面复杂度和 BRepCheck。
- `native_surface_fit_probe` 可独立输出诊断 patch STEP 和 JSON，不修改 GUI 或主 `ShapeDocument`。
- 合成测试覆盖共面裁剪、人工切分自由曲面、尖角裁剪，以及孔洞、开放、非流形边界拒绝。
- candidate 179 中等档真实探针已写入独立 `data/runs` 目录；生成的一张面通过 BRepCheck 和 STEP 二次读取，但没有通过 0.03 几何容差。
- NSR-1.1 保留兼容的首个 `failure_reason`，新增有序 `failed_gates`，可同时表达 boundary、corner、interior、normal、Jacobian、复杂度和 BRep 失败。
- 报告新增 candidate face 数、boundary valid、完整 solver 配置、`diagnostic_patch`、`patch_acceptable`、`patch_role` 和 boundary/corner/interior 样本级诊断。
- 每轮在 `diagnostics/native_surface_fit/` 写出完整数值 CSV 和只含超限源点—投影点连线的 OBJ；合成测试覆盖多失败门与文件字段合同。
- 新增参数化 `scripts/run_native_surface_fit_tiers.ps1`，固定 validation=192、boundary samples=32、Jacobian grid=16、tolerance3d=0.03、normal tolerance=15°，只改变 training=24/48/96，并生成独立 run 与比较表。

固定事实：

```text
run: data/runs/20260713_200400_711__native-surface-fit__03_配件_Clay__cand-0179__nsr-1-medium
boundary max: 0.0381795，12 / 416 超限，worst edge=1540
corner max: 0.0381795，2 / 26 超限
interior max/p95: 0.0933887 / 0.0356314，6 / 96 超限，worst face=1709
normal max: 14.1263°，未超过 15°
Jacobian: 无翻转、无近奇异
BRepCheck: true
STEP readback: 一张面
```

该 STEP 是失败诊断产物，`patch_written=true` 不表示拟合验收通过。

NSR-1.1 固定三档已完成：

```text
sparse: InteriorErrorExceeded, NormalErrorExceeded
  interior max/p95=0.154917/0.114527, degree=8x8, poles=529, BRepCheck=true
medium: BoundaryErrorExceeded, CornerErrorExceeded, InteriorErrorExceeded, NormalErrorExceeded
  interior max/p95=0.093389/0.042576, degree=8x8, poles=256, BRepCheck=true
dense: BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded
  interior max/p95=0.041323/0.025610, degree=8x8, poles=529, BRepCheck=false
```

内部误差随训练密度显著下降，normal 超限样本数从 8 降到 1，因此当前证据支持继续 OCCT Filling，而不是只为 NSR-4 记录 Plate 能力不足。三档均为 `diagnostic_patch=true`、`patch_acceptable=false`；dense 需另立 BRepCheck 与法向离群诊断，尚未进入 NSR-2。

补充 STEP 二次读取事实：sparse / medium / dense 的诊断 patch 二次读取均为一张 face 且 `BRepCheck=true`，但 edge 数分别为 44 / 46 / 50，而内存态报告均遍历到 26 条边。该结果只能证明 STEP translator 重建后的表示可读，不能把 roundtrip 当作原生 patch 的隐式修复或绕过内存态 BRepCheck。

NSR-1.2 dense 结构诊断与固定 96 点采样分配 A/B 已完成：

- `SurfaceRefitReport` 分开记录 Filling 输出、方向反转后和 probe STEP roundtrip 的 BRepCheck。
- face / wire / edge / vertex 的直接与上下文 `BRepCheck_Status` 可序列化到 JSON。
- 每条输出边记录原始 boundary edge 映射、tolerance、degenerated、SameRange、SameParameter、pcurve / 3D curve 参数范围和 3D-COS 最大偏差。
- 每张候选源面记录 training / validation / validation-over-tolerance 数量；内部验证样本记录最近训练点距离。
- 法向样本记录源点、投影点、源法向、拟合法向、角度、UV、face id 和状态，并输出 `surface_normal_error_samples.csv`。
- 新增 `area_weighted` 与 `face_stratified_area_weighted` 两种确定性策略；分层策略在源面数超过训练预算时明确拒绝。
- 新增 `scripts/run_native_surface_fit_sampling_ab.ps1`，一次成功调用只保留两个策略 run 和一个 comparison run；异常时只清理本次调用在 `data/runs` 下创建的目录。

固定事实：

```text
area_weighted:
  failed_gates=BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded
  per-face training min/max/zero=0/4/2
  Filling/oriented/roundtrip BRep=false/false/true
  interior max/p95/over=0.041323/0.025610/4
  normal max/over=45.4750°/1

face_stratified_area_weighted:
  failed_gates=InteriorErrorExceeded, NormalErrorExceeded
  per-face training min/max/zero=1/3/0
  Filling/oriented/roundtrip BRep=true/true/true
  interior max/p95/over=0.068507/0.024468/8
  normal max/over=40.7441°/1
```

纯面积加权的具体结构失败是 face `UnorientableShape` 和 wire `SelfIntersectingWire`，方向反转前后相同；分层策略消除了该失败，但没有通过 interior / normal 门。两种策略均为失败诊断 patch，`patch_acceptable=false`，因此 NSR-2 未启动；该阶段留下的局部切平面约束任务随后由 NSR-1.3 完成，NSR-4 继续冻结。

NSR-1.3 局部切平面约束有限周期实验已完成：

- `SourceSample` 在采样时保留源 face、稳定 `TopoDS_Face` 支撑面和 UV；不通过 face id 反查或猜测支撑面。
- 保留原确定性 192 点作为 selection，新增独立 192 点 holdout；training / selection / holdout 三组空间交集四档均为 0。
- selection 残差按 `max(distance / 0.03, normal / 15°)` 排序，并使用确定性空间 NMS；误差簇映射到不同现有训练样本，形成 1 ⊂ 4 ⊂ 8。
- 选中训练样本改用 `BRepFill_Filling::Add(U, V, Support, GeomAbs_G1)`，其余训练点仍为 G0；总训练约束固定 96，实际 G1 与 0 / 1 / 4 / 8 配置一致。
- `native_surface_fit_probe` schema 4 明确拆分 selection / holdout 指标，记录全部 G1 选择依据、训练支撑面合同、三组交集和约束计数；CSV / OBJ 也带 sample set。
- 新增 `scripts/run_native_surface_fit_local_g1_tiers.ps1`；成功调用固定生成四个 tier run 和一个 comparison run。单档基础设施异常不会删除已完成 run，仍生成失败汇总后返回错误。
- 自动测试覆盖支撑面 UV 一致性、G1=0 数值回归、嵌套确定性选择、96 点替换计数、三集合互斥、holdout 接受门、报告字段和脚本部分失败合同。

candidate 179 固定事实：

| G1 | failed_gates | holdout interior max/p95/RMS/over | holdout normal max/p95/RMS/over | BRep filling/oriented/roundtrip | Jacobian near/flip | degree/poles/segments |
|---:|---|---|---|---|---|---|
| 0 | InteriorErrorExceeded, NormalErrorExceeded | 0.054879/0.026982/0.010159/8 | 23.5646°/7.2512°/3.9602°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 1 | InteriorErrorExceeded, NormalErrorExceeded | 0.054914/0.026859/0.010164/8 | 23.1334°/7.2574°/3.9184°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 4 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.038039/0.017075/0.007938/2 | 22.9609°/7.2526°/3.5619°/1 | false/false/true | 0/0 | 8x8/529/9 |
| 8 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.038155/0.017076/0.007676/2 | 22.8859°/6.7965°/3.4800°/1 | false/false/true | 0/0 | 8x8/529/9 |

G1=0 的 selection、边界、角点、Filling 误差、复杂度和 BRep 状态与 NSR-1.2 分层基线逐值一致。4 / 8 个 G1 明显改善内部距离，但法向仍超过 15°，并重新引入 face `UnorientableShape` 与 wire `SelfIntersectingWire`。四档均为诊断 patch、`patch_acceptable=false`；按停止规则不继续增加 G1 或混调 Filling 参数，也不进入 NSR-2。candidate 179 单例不足以解冻 NSR-4，下一证据任务是至少三个受支持真实候选的同类失败复核。

正式 comparison：

```text
data/runs/20260714_143049_479__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.3-local-g1-tiers
```

---

## 3. 候选区域与旧 analytic 路线清理

已完成：

- `MergePlanner` 当前只生成 `FeatureBoundedRefit` 候选区域。
- GUI 支持候选区域预览、选择、接受、拒绝、隐藏和恢复。
- Face / Candidate Inspect 可显示候选归属和基础统计。
- Plane / Sphere / Cylinder / Cone / Torus / Freeform 旧 analytic candidate 类型已删除。
- 旧 Plane / Sphere 真实合并入口、Command、AppController API、后端 merger 和测试已删除。
- `MergeRegionGrower`、`RegionMergeResult`、`RegionMergeOptions` 等旧路线残留已收口。

结论：

```text
旧 OCCT analytic region merge 只保留为历史背景，不再作为当前主线增强方向。
```

---

## 4. Geomagic 输入与候选面外扩路线

当前可执行输入主线：

- 默认 fitting input mode 是 `StpSampledCandidateSurface`。
- GUI 默认开启 `启用 STP 候选面外扩带`。
- 当前 Route 2 主扩宽机制是 Candidate / Source Face Parallel Over-Cover：沿原候选 STP 面边界在 source face 切平面上做 3D 平行外扩，再由 Apply 阶段用原 STP boundary strict re-trim 裁回。
- GUI 默认参数：
  - candidate over-cover width = `0.25`
  - rings = `3`
  - samples per edge = `64`
  - corner miter max scale = `1.25`
- Candidate over-cover 的保守稳定策略：
  - reverseEdge 遍历会反向 pcurve tangent 后再判断 left/right 外侧。
  - 外扩点优先使用 `P + D * distance` 的 source face 切平面 offset。
  - surface UV 外推只作为泄漏诊断参考；normal leakage 超过阈值时报告 fallback。
  - corner miter 遇到方向突变、相邻 offset 近似反向时只做 clamp，不放大角点。
  - candidate bridge / quad strip 会拒绝明显异常长三角。
- `StpSampledFittingReport` / `corner_baseline_probe` JSON 输出 candidate over-cover 诊断字段：
  - `candidate_surface_over_cover_normal_leakage_max`
  - `candidate_surface_over_cover_direction_fallback_count`
  - `candidate_surface_over_cover_long_triangle_count`
  - `candidate_surface_over_cover_max_triangle_edge_length`
  - `candidate_surface_over_cover_source_face_count`
  - `candidate_surface_over_cover_direction_flip_count`
- `AppController::applyCurrentPatchToCurrentCandidate` 会显式启用 `strictOriginalBoundaryRetrim`。
- 显式 strict retrim 生成的新重裁边不直接塞回旧 solid face 拓扑，而是用 face-compound assembly 后再进入 repair / sewing / StrictTopologyGate。

Adjacent-face support collar 当前降级为显式辅助上下文：

- `corner_baseline_probe` 默认辅助 collar width = `0.05`，rings = `1`。
- Route 2 runner 默认不叠加 adjacent collar；只有传 `-EnableAuxiliarySupportCollar` 时才拼接 `--b2-adjacent-face-support-collar` / `--support-collar-*`。
- adaptive support-collar 宽度公式：

```text
max(configured_width, 2*g_under, 2*h95)
```

这保证 adaptive 不会把显式传入的宽度压小。

历史已实现但已退役：

- B2.0 原 STP boundary 外 guard-band 采样曾实现并验证，但不是当前保留入口。
- B2.1 over-cover strip 曾实现并验证，但不再作为可执行实验入口维护。
- 2026-06-25 的 width=`0.25` adjacent-face support collar 真实样例证明输入 STL 干净，但 Geomagic 输出 72 faces 且 strong collar 会把输入曲面拉向相邻面 / 侧壁方向，因此不再作为主扩宽策略。
- 旧 `OverCoverWidth` / `OverCoverRings` CLI 参数已从 Route 2 runner 移除。
- `corner_baseline_probe` 不再接受 guard-band / over-cover flags，也不再输出对应 JSON 字段。

当前脚本入口事实：

```text
scripts/run_boundary_trim_fill_experiments.ps1
→ 显式传递 --candidate-surface-over-cover
→ 显式传递 --candidate-over-cover-width
→ 显式传递 --candidate-over-cover-rings
→ 显式传递 --candidate-over-cover-miter-max-scale
→ 默认不传 --b2-adjacent-face-support-collar
→ -EnableAuxiliarySupportCollar 时才传 --support-collar-width/rings/adaptive/under-cover
```

---

## 5. Patch Apply 与诊断能力

已完成：

- `PatchImportService` 支持 STEP/STP/IGS/IGES patch 导入和统计。
- `BoundaryConstrainedPatchBuilder` 使用 Geomagic surface 趋势和原 STP boundary 构造 replacement。
- `BoundaryConstrainedMultiSurfaceShellBuilder` 支持 multi-surface shell、内部 seam、closed wire / open wire 诊断。
- `PatchReplacementRepair` 执行 ShapeFix、SameParameter、adaptive sewing、shell-to-solid 和 best-result selection。
- `StrictTopologyGate` 检查 BRepCheck、free edge、multiple edge、solid/watertight、STEP export/readback。
- `CommercialCadLikeQualityGate` 输出 boundary / corner / feature drift、seam continuity 和 sampling report。
- `trim_diagnostics` 输出 under-cover、over-cover、boundary gap、internal seam gap 和 roundtrip_changed。
- `short_edge_diagnostics`、ModelCHECK parse-only、Creo diagnostic correlation 已接入。
- `raw_patch_unify_probe` 可对 normalized raw patch 执行 `stats -> UnifySameDomain -> stats` 诊断，并保留 unified STEP 与 JSON 报告。

2026-06-27 raw patch UnifySameDomain 诊断事实：

```text
input:
  data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810/route2_expanded_strict_trim/normalized_patch_mm.stp

output:
  data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810/route2_expanded_strict_trim/raw_patch_unify_probe/normalized_patch_mm_unified.stp
  data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810/route2_expanded_strict_trim/raw_patch_unify_probe/raw_patch_unify_report.json

before normalized patch:
  faces=68
  edges=326
  shells=1
  free_edges=26
  BRepCheck=true

after UnifySameDomain in memory:
  faces=59
  edges=306
  shells=1
  free_edges=26
  BRepCheck=false

after unified STEP roundtrip:
  faces=59
  edges=306
  shells=1
  free_edges=26
  BRepCheck=true
```

结论：

```text
UnifySameDomain 能减少同域碎面和碎边，但不减少 raw patch 的自由边。
内存态 BRepCheck 会失败，写出 STEP 后再读回才恢复通过。
因此它目前只能作为 raw patch 碎化诊断，不作为 Patch Apply / StrictTopologyGate 的直接修复策略。
```

验收判断：

```text
OCCT strict 通过只是必要条件。
用户验收仍要求 Creo 打开为灰色水密实体。
SHORT_EDGES 不一定必须为 0，但非水密或 Creo 非灰色实体就是失败。
```

---

## 6. 外部 CAD / Creo 诊断

已完成：

- `scripts/run_creo_step_diagnostic.ps1` 可显式运行后台 Creo STEP import + ModelCHECK。
- 诊断对象只允许是 Apply 成功后导出的已合并 STEP。
- 原始 Geomagic patch STEP 只能作为 `PatchPreflightOnly`，不能证明合并模型成功。
- ModelCHECK XML 解析会保留 failed checks、short-edge items、import validation 和关联摘要。
- `creo_toolkit_phase1_repair_probe` 可运行 Creo import-feature sewing 实验，但当前真实样例没有救回水密实体。
- Creo STL import -> STEP 最小反证已完成：Toolkit 调用和 ModelCHECK 可能成功，但导出 STEP 可为空 B-rep，因此不能替代 Geomagic STL->B-rep fitting。

当前结论：

```text
Creo API 调用成功、ModelCHECK runner 成功、STEP 文件存在，都不是验收成功。
验收必须看导出 STEP 的 B-rep 实体、水密性、StrictTopologyGate 和 Creo 灰色实体显示。
```

---

## 7. 真实样例固定事实

当前重点真实样例：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
```

已知历史结论：

- BestFitFreeform 已移除，不再投入。
- 默认按多面片处理，单面片只是少数情况。
- 旧失败不是单个 open wire，而至少包含两类问题：
  - unowned Geomagic fringe / internal seam faces
  - boundary-owned faces 的 original-boundary segments + internal seams 不闭合
- 支撑带必须和主体 fitting STL 拓扑连通；只靠视觉贴近不够。
- Route 2 当前重点是确认 candidate/source-face parallel over-cover 的方向、角点覆盖、连通性，以及后续 Apply / Creo 诊断。

---

## 8. 验证命令

默认验证：

```powershell
.\scripts\build_debug.ps1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
git diff --check
```

Route 2 真实样例入口：

```powershell
.\scripts\run_boundary_trim_fill_experiments.ps1 `
  -SourceStep data\stp\03_配件_Clay.stp `
  -CandidateId 179 `
  -CandidateOverCoverWidth 0.25 `
  -CandidateOverCoverRings 3 `
  -WrapCore "E:\Geomagic Wrap\wrapCore.exe" `
  -CreoRoot "E:\Proe\Creo 11.0.0.0"
```

默认输出为新的 `data/runs/<run-id>/`；只在需要外部编排时显式传入一个尚未包含 `run.json` 的独立 `-OutputDir`。
