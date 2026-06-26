# 已完成功能与历史路线归档

> 文档定位：记录已经完成、已经验证或已经退役的功能事实。当前待办只放在 `docs/TODO.md`。

更新时间：2026-06-25

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

## 4. Geomagic 输入与支撑带路线

当前可执行输入主线：

- 默认 fitting input mode 是 `StpSampledCandidateSurface`。
- GUI 默认开启 `启用 STP 邻接面支撑带`。
- 当前唯一保留的 STL 扩宽机制是 adjacent-face support collar。
- GUI 默认参数：
  - support collar width = `0.25`
  - rings = `2`
  - samples per edge = `64`
  - adaptive width = `true`
- adaptive support-collar 宽度公式：

```text
max(configured_width, 2*g_under, 2*h95)
```

这保证 adaptive 不会把显式传入的宽度压小。

历史已实现但已退役：

- B2.0 原 STP boundary 外 guard-band 采样曾实现并验证，但不是当前保留入口。
- B2.1 over-cover strip 曾实现并验证，但不再作为可执行实验入口维护。
- 旧 `OverCoverWidth` / `OverCoverRings` CLI 参数已从 Route 2 runner 移除。
- `corner_baseline_probe` 不再接受 guard-band / over-cover flags，也不再输出对应 JSON 字段。

当前脚本入口事实：

```text
scripts/run_boundary_trim_fill_experiments.ps1
→ 显式传递 --support-collar-width
→ 显式传递 --support-collar-rings
→ 显式传递 --adaptive-support-collar-width
→ 显式传递 --support-collar-under-cover
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
- Route 2 当前重点是确认新版 support collar 输入 STL 的连通性、有效宽度、角点覆盖和后续 Apply / Creo 诊断。

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
  -OutputDir data\baseline_runs\<run-name> `
  -WrapCore "E:\Geomagic Wrap\wrapCore.exe" `
  -CreoRoot "E:\Proe\Creo 11.0.0.0"
```
