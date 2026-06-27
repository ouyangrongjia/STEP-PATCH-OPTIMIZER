# 已完成功能与历史路线归档

> 文档定位：记录已经完成、已经验证或已经退役的功能事实。当前待办只放在 `docs/TODO.md`。

更新时间：2026-06-26

---

## 1. 当前主线能力

项目当前主线是 Geomagic AutoSurface patch preview / Apply：

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
  -OutputDir data\baseline_runs\<run-name> `
  -WrapCore "E:\Geomagic Wrap\wrapCore.exe" `
  -CreoRoot "E:\Proe\Creo 11.0.0.0"
```
