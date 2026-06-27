# 当前 TODO

> 文档定位：只记录当前待办、下一步实验和验收边界。已完成或已退役的历史计划放在 `docs/completed_features.md`。

更新时间：2026-06-26

---

## 1. 当前判断

当前主线仍是 Route 2：

```text
STP
→ 选取 FeatureBoundedRefit 候选区域
→ STP sampled fitting input
→ Geomagic AutoSurface
→ normalized patch mm
→ strict original-boundary constrained trim / Apply
→ OCCT diagnostics / StrictTopologyGate
→ Creo diagnostics / ModelCHECK
```

2026-06-25 的真实样例结果已经排除两个旧怀疑：

```text
1. support collar 输入 STL 不是脏的：
   components=1
   boundaryCycles=1
   nonManifoldEdges=0
   nonManifoldVertices=0
   degenerateTriangles=0

2. adaptive support collar width 没有把显式宽度压小：
   effective width min/mean/max = 0.25/0.25/0.25
```

但截图和结果同时说明：

```text
强 adjacent-face support collar 会把 Geomagic 输入曲面拉向相邻面 / 侧壁方向。
问题已经不是单纯“宽度不够”，而是主扩宽方向错误。
```

因此当前路线已经改为：

```text
Candidate / Source Face Parallel Over-Cover 主导扩宽
adjacent-face support collar 只保留为窄辅助上下文
最终仍由原 STP boundary strict re-trim 裁回
```

---

## 2. 本轮已落地能力

已完成并归档到 `docs/completed_features.md`：

```text
1. StpSampledFittingMeshBuilder 新增 candidate/source-face parallel over-cover。
2. GUI 默认开关改为“启用 STP 候选面外扩带”。
3. GUI 默认参数：
   candidateSurfaceOverCoverWidth = 0.25
   candidateSurfaceOverCoverRingCount = 3
   candidateSurfaceOverCoverSamplesPerEdge = 64
   candidateSurfaceOverCoverCornerMiterMaxScale = 1.25
4. corner_baseline_probe 支持 --candidate-surface-over-cover 与 candidate over-cover JSON 字段。
5. run_boundary_trim_fill_experiments.ps1 Route 2 默认启用 candidate over-cover。
6. run_corner_baseline_gate.ps1 的 B2 / B2.2 改为 candidate over-cover。
7. adjacent-face support collar 默认降级为辅助：
   width = 0.05
   rings = 1
8. GUI Apply 显式启用 strictOriginalBoundaryRetrim。
9. 显式 strict retrim 的 replacement face 使用 face-compound assembly 后再 repair / sewing。
10. Candidate over-cover 已做保守稳定修复：
    - reverseEdge 遍历会反向 pcurve tangent 后再判断外侧。
    - 外扩点优先使用 source face 切平面 3D offset，surface UV 外推只作为泄漏诊断参考。
    - corner miter 默认降到 1.25，方向突变时只 clamp 不放大。
    - candidate bridge / quad strip 会记录并拒绝明显异常长三角。
    - report / JSON 新增 normalLeakage、directionFallback、longTriangle、maxTriangleEdgeLength、sourceFaceCount、directionFlip 指标。
```

已验证：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
```

---

## 3. 最新真实样例结果

必须用同一个真实样例做对比：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
previous baseline: data/baseline_runs/route2_support_collar_real_20260625_0020
```

本轮输出目录：

```text
data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810
```

命令：

```powershell
.\scripts\run_boundary_trim_fill_experiments.ps1 `
  -SourceStep data\stp\03_配件_Clay.stp `
  -CandidateId 179 `
  -CandidateOverCoverWidth 0.25 `
  -CandidateOverCoverRings 3 `
  -CandidateOverCoverSamples 64 `
  -OutputDir data\baseline_runs\route2_candidate_overcover_conservative_<timestamp> `
  -WrapCore "E:\Geomagic Wrap\wrapCore.exe" `
  -CreoRoot "E:\Proe\Creo 11.0.0.0"
```

不要默认叠加 adjacent-face support collar。只有需要专门做 A/B 时才加：

```powershell
  -EnableAuxiliarySupportCollar `
  -SupportCollarWidth 0.05 `
  -SupportCollarRings 1
```

结论：

```text
1. candidate over-cover 保守修复后，STL 文件本体仍为单组件输入：
   components=1
   boundaryCycles=1
   nonManifoldEdges=0
   degenerateTriangles=0
   candidate_surface_over_cover_boundary_coverage=1

2. 但 over-cover 诊断暴露当前保守策略过度拒绝：
   candidate_surface_over_cover_normal_leakage_max=0.7464534307
   direction_fallback_count=3935
   long_triangle_count=9984
   rejected_count=9984
   max_triangle_edge_length=0.3965649168
   direction_flip_count=0
   over-cover triangle_count=2184

3. Geomagic 能输出 patch，但重新变成 high risk：
   faces=68
   edges=326
   high_risk=true

4. normalized patch 单位正确：
   raw patch=meter
   normalized patch=millimeter

5. Apply 仍失败在 multi-surface open-wire closure，但失败位置已变化：
   failed_patch_face_index=7
   failed original boundary edge ids=1585
   endpoint_gap=0.7593067905601413
   pcurve rebuild failure=0
   SameParameter failure=0

6. Creo sewing 仍失败：
   exported STEP solids=0
   shells=2
   BRepCheck=false
   after_free_edges=52
   ModelCHECK diagnostic_passed=false
   SHORT_EDGES=1593
```

判读：

```text
这轮修复证明 reverseEdge 不再出现方向翻转（direction_flip_count=0），但 normal leakage 很大，且长三角保护拒绝过多。
当前 blocker 已不是“STL 拓扑脏”，而是 over-cover 采样点对应/桥接策略过保守或错配，导致写入的有效外扩带不足，Geomagic 输出又碎化。
下一步应先降低 longTriangle rejection 的误伤，或改为局部 bevel/重采样补洞，而不是进入 Apply / sewing 调参。
```

---

## 4. 当前 P0：修正 over-cover 长三角拒绝过多 / 有效外扩带不足

下一步不是继续调 candidate over-cover 宽度，也不是恢复 adjacent collar。

本轮保守修复后的真实样例已经证明：

```text
direction_flip_count=0
normal_leakage_max=0.7464534307
long_triangle_count=9984
rejected_count=9984
Geomagic patch faces=68
preview high_risk=true
```

必须检查：

```text
1. longTriangleCount 中有多少是 bridge 错配，多少是 quad strip 相邻 ring 错配。
2. 当前 5 * meanSpacing 阈值是否误杀正常角点/高曲率段外扩三角。
3. 被拒绝的三角是否应改为局部 bevel / fan 补洞，而不是直接丢弃。
4. normal leakage 是否集中在少数 source face / edge；如果集中，记录 problem edge ids。
5. 对 source face 切平面 offset 后的 ring 对应关系是否仍按原 boundary 采样顺序一一配对；若不是，应先重采样/重排 ring。
6. 只有当 longTriangleCount 和 rejectedCount 收到可解释低值后，再继续定位 failed_patch_face_index=7 / original boundary edge 1585 的 open-wire closure。
```

---

## 5. 通过 / 失败判定

最低通过标准：

```text
1. fitting STL 是单连通输入：
   components = 1
   boundaryCycles 合理
   nonManifoldEdges = 0
   nonManifoldVertices = 0

2. candidate over-cover 覆盖完整：
   boundary_coverage 接近 1
   rejected_count = 0 或有明确可解释原因

3. 视觉上方向正确：
   不再沿相邻面 / 侧壁向下翻
   角点不被圆角化或抹掉

4. Apply 不接受伪成功：
   OCCT strict 通过只是必要条件
   Creo 打开必须是灰色水密实体
```

如果仍失败，按下面分流：

```text
STL 输入仍向下翻：
  修 candidate over-cover 外侧方向判定，不进入 Apply 调参。

STL 输入正确，但 Geomagic 仍圆角化或输出过碎：
  优先看 Geomagic 参数和角点 miter 采样，不先改 sewing。

Geomagic patch 覆盖原边界，但 strict re-trim 失败：
  定位 pcurve 投影、SameParameter、surface coverage。

strict re-trim 成功，但 open-wire closure 失败：
  回到 failed_patch_face_index / owner split / internal seam network 诊断。

OCCT 水密但 Creo 失败：
  获取 Creo 失败区域空间定位，不把 OCCT strict 当最终成功。
```

---

## 6. 冻结范围

不要做：

- 不恢复 BestFitFreeform。
- 不恢复旧 guard-band / 旧 over-cover strip 的历史实现作为主路线。
- 不把 Geomagic patch outer boundary、STL crop boundary、support collar 外环当最终 CAD boundary。
- 不放宽 `StrictTopologyGate`。
- 不通过继续加大 adjacent-face support collar 主宽度来掩盖方向错误。
- 不直接进入 OCCT 全曲面重建；只有局部反复无法覆盖时才考虑局部 filling / refit。
- 不删除或绕过 `PatchReplacementRepair`。
- 不提交 pre-repair、diagnostic best sewing 或 raw Geomagic patch 作为最终结果。
- 不在默认测试中硬编码 `03_配件_Clay.stp` 或 candidate `179`。

---

## 7. 文档维护规则

```text
docs/TODO.md:
  只放当前待办、下一轮实验、验收边界和失败分流。

docs/completed_features.md:
  放已落地功能、已验证功能、已退役历史路线和当前能力事实。

docs/implementation_status.md:
  放实现进度、关键真实样例结论和验证命令。
```
