# 当前 TODO

> 文档定位：只记录当前待办、下一步实验和验收边界。已完成或已退役的历史计划放在 `docs/completed_features.md`。

更新时间：2026-06-25

---

## 1. 当前判断

当前主线是 Route 2：

```text
STP
→ 选取 FeatureBoundedRefit 候选区域
→ 扩大后的 STP sampled fitting input
→ Geomagic AutoSurface
→ normalized patch mm
→ strict original-boundary constrained trim / Apply
→ OCCT diagnostics / gate
→ Creo diagnostics
```

当前可执行的 STL 扩宽机制只有 adjacent-face support collar。旧 guard-band 和 over-cover strip 已从可执行实现与脚本入口移除，不再作为下一轮实验方向。

---

## 2. 最新真实样例结果

样例：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
branch: feature-bounded-refit
baseline commit: 1481f62a457f35d6ea24c80cf44310d6223e7573
latest output: data/baseline_runs/route2_support_collar_real_20260625_0020
```

最新结论：

```text
support collar input is clean:
  components=1
  boundaryCycles=1
  effective width min/mean/max=0.25/0.25/0.25

normalized patch is millimeter.
CommercialCadLikeQualityGate passed on the patch.

Route 2 still failed before OCCT applied STEP export:
  Geomagic patch face count=72
  preview high_risk=true
  multi-surface built_faces=6
  closed_wires=11
  open_wires=2
  failed_patch_face_index=2
  failed original boundary edge ids=1540,1543,1546
  failed open-wire endpoint gap=0.879063

Creo Toolkit sewing still failed final acceptance:
  exported STEP solids=0
  shells=2
  BRepCheck=false
  after_free_edges=60
  ModelCHECK diagnostic_passed=false
```

## 3. 当前 P0：定位 Route 2 open-wire closure

下一步不是继续加宽 support collar，而是定位 failed patch face 的 closure 组成。

必须检查：

- failed patch face index `2` 的 8 条 face edge 来自哪些 patch internal seams / original-boundary segments。
- original-boundary edge `1540`、`1543`、`1546` 的 split segment 参数区间和 owner face。
- endpoint gap `0.879063` 是 patch internal seam 缺失、segment ordering 错误、还是 fitted surface 局部覆盖断裂。
- 为什么 pcurve rebuild 41/41 成功且 max projection distance 只有 `0.0108314`，仍无法形成 closed wire。
- 72-face Geomagic patch 是否造成过碎的 internal seam network，是否需要拒绝或约束 AutoSurface 输出，而不是在 Apply 侧硬缝。

验收：

```text
1. 失败产物也必须保留。
2. 不因为 ModelCHECK、StrictTopologyGate 或 Creo solid acceptance 失败而删除 run directory。
3. OCCT strict 通过但 Creo 非灰色实体仍失败。
4. SHORT_EDGES 不一定要求为 0；水密实体才是关键。
```

---

## 4. P1：支撑带输入质量判定

重点不是继续添加新扩宽路线，而是判断当前唯一支撑带路线是否提供了足够输入质量。

需要判定：

- support collar 与主体 STL 是否拓扑连通。
- Geomagic pre-repair `components` 是否为 `1`。
- `boundaryCycles` 是否为 `1`。
- support collar effective width 是否至少为 `0.25`。
- 角点覆盖是否比旧结果改善。
- normalized patch 是否仍为 millimeter。

失败分类：

```text
components > 1:
  先修支撑带与主体 STL 的拓扑连通，不分析 AutoSurface 参数。

effective width < configured width:
  先修 adaptive width / 参数转发，不分析 downstream Apply。

输入 STL 干净但 Apply 失败:
  转向 strict retrim / multi-surface shell / owner split / local coverage 诊断。

OCCT 水密但 Creo 非灰色实体:
  转向 Creo / commercial CAD failure localization，不把 OCCT strict 当最终成功。
```

---

## 5. 近期后续方向

优先级从高到低：

1. 基于本轮 JSON 输出指标表，定位 failed_patch_face_index=2 的 open-wire closure。
2. 如果定位为 72-face patch 过碎导致 internal seam network 不稳定，优先设计 patch 输出拒绝/约束或 seam selection 诊断，不先加宽 support collar。
3. 如果定位为 segment ordering / owner split 问题，先做最小诊断和 synthetic test，再碰 Apply 逻辑。
4. 如果 Apply/OCCT 通过但 Creo 失败，优先获取 Creo 失败几何的空间定位或更强报告，而不是继续调 STL 输入。
5. 文档同步只记录当前结论，不再把完整历史实验计划堆回 TODO。

---

## 6. 冻结范围

不要做：

- 不恢复 BestFitFreeform。
- 不恢复旧 guard-band / over-cover 可执行入口。
- 不把 Geomagic patch outer boundary、STL crop boundary、support collar 外环当最终 CAD boundary。
- 不放宽 `StrictTopologyGate`。
- 不删除或绕过 `PatchReplacementRepair`。
- 不提交 pre-repair、diagnostic best sewing 或 raw Geomagic patch 作为最终结果。
- 不在默认测试中硬编码 `03_配件_Clay.stp` 或 candidate `179`。
- 不因失败而清理本轮生成的 patch、base_removed_candidate、normalized patch、applied/sewn STEP。

---

## 7. 文档维护规则

```text
docs/TODO.md:
  只放当前待办和下一步判定。

docs/completed_features.md:
  放已完成、已验证或已退役的功能事实。

docs/implementation_status.md:
  放较完整的实现状态和阶段性验收记录。

D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\03_DEV_LOG.md / 04_PITFALL_LOG.md / 06_REVIEW.md:
  追加型历史记录文档已删除，不再作为启动依赖。
```
