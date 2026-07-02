# 当前 TODO

> 文档定位：只记录当前待办、下一步实验和验收边界。已完成或已退役的历史计划放在 `docs/completed_features.md`。

更新时间：2026-07-02

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
11. Candidate over-cover rejected/normal-leakage 可定位诊断已补：
    - rejected count 分桶为 body bridge、boundary-to-ring quad、ring-to-ring quad、edge-build。
    - JSON 输出 per edge/source face problem summary 和 max normal leakage edge/source face。
    - corner_baseline_probe 会导出：
      overcover_rejections.csv
      overcover_rejected_body_bridge.stl
      overcover_rejected_boundary_to_ring.stl
      overcover_rejected_ring_to_ring.stl
      overcover_worst_points.obj
12. 2026-07-02 本地基线恢复到 conservative candidate/source-face over-cover：
    - 放弃 2026-07-01 global wire-ring 与 2026-07-02 direction-field follow-up 作为主实现。
    - StpSampledFittingMeshBuilder 继续按 original boundary wire 的 edge 顺序逐 edge 采样；每条 edge 的 ring1/ring2/ring3 全部成功后才并入全局 ring。
    - 不恢复旧 guard-band / adjacent collar 主路线，不改 Apply、PatchReplacementRepair、StrictTopologyGate。
    - 保留并扩展诊断：ring closure、direction flip、index mismatch、source-face transition、edge/source face distribution。
    - corner_baseline_probe 保留 overcover_rejections.csv / rejected STL / worst points OBJ，并新增 overcover_ring_diagnostics.csv。
13. 2026-07-02 Route 2 / Geomagic face-count 诊断层已落地：
    - 不改变默认 conservative over-cover；新增显式 A/B 参数：
      --candidate-over-cover-disable-outer-rings
      --candidate-over-cover-body-bridge-mode <normal|none|smooth>
      --candidate-over-cover-disable-body-bridge
      --candidate-over-cover-smooth-transition
      --candidate-over-cover-smooth-strip
      --geomagic-no-sharpen-contours
    - JSON/CSV 新增 generated/emitted/disabled ring count、emitted sample count、body bridge mode、body bridge triangle count、smooth strip/transition 标记。
    - candidate_surface_over_cover_diagnostics 新增 overcover_experiment_summary.json/csv。
    - corner_baseline_probe 新增 probe_artifacts，保存本轮 fitting STL、raw patch STP/IGES、fit_region log 的 run-local copy。
    - run_boundary_trim_fill_experiments.ps1 会把 candidate_surface_over_cover_diagnostics 复制进 route2_artifacts。
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

本轮复跑输出目录：

```text
data/baseline_runs/route2_conservative_restored_baseline_20260702_134331/route2_artifacts/
```

历史对照基线目录：

```text
data/baseline_runs/route2_candidate_overcover_conservative_rerun_20260628_015653/route2_artifacts/
```

后续每次测试都必须保留同类 route2_artifacts 角色文件：

```text
ROUTE2_SUMMARY.md
GEOMAGIC_RUN.md
crop_stl_candidate_<id>.stl
raw_geomagic_crop_stp_candidate_<id>.stp
normalized_patch_mm.stp
base_removed_candidate.stp
geomagic_fit_region.log
geomagic_generation.stdout.txt
geomagic_generation.stderr.txt
creo_api_merged.stp（若生成）
```

本轮复跑结果：

```text
status=AllRoutesFailed
crop STL exists=true
raw Geomagic crop STP exists=true
normalized patch STP exists=true
base_removed_candidate STP exists=true
OCCT applied/merged STEP exists=false
Creo API merged STEP exists=true

candidate over-cover:
  rejected_count=9984
  boundary_to_ring_rejected_count=3328
  ring_to_ring_rejected_count=6656
  body_bridge_rejected_count=208
  long_triangle_count=9984
  triangle_count=2184
  ring_point_count=1664
  index_mismatch_count=0
  direction_flip_count=0
  source_face_transition_count=22
  ring_closure_max_ratio=9.21257873636548
  max_triangle_edge_length=0.39656491680174

Patch faces=68
Patch edges=326
Patch preview high_risk=true
Boundary max distance=0.0101759896178517
Boundary p95 distance=0.000647815619539007
Corner max distance=0.0101759896178517
Seam max abs normal offset=0.0028804812728491

OCCT apply success=false
OCCT failed patch face index=7
OCCT failed original boundary edge ids=1585
OCCT failed endpoint gap=0.759306790560141

Creo Toolkit runner success=true
StrictTopologyGate on Creo exported STEP=false
StrictTopologyGate failure=BRepCheckFailed
after_free_edges=52
after_multiple_edges=0
ModelCHECK diagnostic_passed=false
SHORT_EDGES item count=1593
```

2026-07-02 face-count A/B 真实复跑：

```text
output:
  data/baseline_runs/route2_facecount_ab_20260702_180826/
comparison:
  data/baseline_runs/route2_facecount_ab_20260702_180826/comparison.csv
source:
  data/stp/03_配件_Clay.stp
candidate:
  179
```

| variant | emitted rings | bridge mode | over-cover tris | rejected | patch faces | patch edges | high risk | boundary max/p95 | seam max/p95 abs | result |
|---|---:|---|---:|---:|---:|---:|---|---:|---:|---|
| A_conservative_no_sharpen | 3 | normal | 2184 | 9984 | 68 | 326 | true | 0.010176 / 0.000648 | 0.002880 / 0.000568 | Apply gate failed |
| B_minus_outer_ring | 2 | normal | 2184 | 6656 | 68 | 326 | true | 0.010176 / 0.000648 | 0.002880 / 0.000568 | Apply gate failed |
| C_smooth_transition | 3 | smooth | 5096 | 9984 | 633 | 3054 | true | 0.007553 / 0.000129 | 0.001282 / 0.000106 | Apply gate failed |
| D_sharpen_contours | 3 | normal | 2184 | 9984 | 68 | 326 | true | 0.010176 / 0.000633 | 0.002880 / 0.000563 | Apply gate failed |
| E_disable_body_bridge | 3 | none | 0 | 9984 | 5 | 20 | false | 0.123518 / 0.044745 | 0.065219 / 0.026933 | Quality gate failed |
| F_smooth_strip | 3 | normal | 2196 | 9984 | 0 | 0 | false | n/a | n/a | Geomagic failed: Initialization of surface data failed |

判读：

```text
1. A -> B：禁用最外 ring 只把 rejected_count 从 9984 降到 6656；实际 emitted STL triangle_count、patch faces/edges/high_risk 完全不变。
   说明这次 68-face patch network 不是最外 ring 的已写出三角片导致；那些 ring-to-ring 三角本来就被 long-triangle guard 拒绝，没有进入 STL。

2. A -> D：sharpenConstrainedContours 开/关没有改变 faces=68 / edges=326 / high_risk=true。
   当前 face count 主因不是这个 Geomagic contour 开关。

3. A -> C：把 body-to-ring transition 做成 smooth bridge 后，patch faces 从 68 增到 633。
   说明 Geomagic 对 body bridge / transition 附近的人造三角结构非常敏感，平滑过渡如果增加可见三角带，反而会诱导更多 contour。

4. A -> E：禁用 body bridge 后，patch faces 降到 5、high_risk=false，但 boundary/corner/seam drift 大幅超限。
   说明 body bridge 是当前覆盖质量所必需，同时也是最可能诱导 68-face segmentation 的输入结构。
```

当前结论：

```text
conservative 输入视觉够用，是因为 body bridge 把主体 STL 连到原 boundary 附近；
Geomagic 仍导出 68 faces，最可能不是外圈 ring 数量或 sharpen 参数，而是 body-to-ring bridge 形成的人造特征线 / 三角化过渡。
下一步不要继续加宽或恢复 full over-cover；应诊断并降低 body bridge 的可见 contour 复杂度，同时保持 boundary/corner/seam drift 接近 A 基线。
```

判读：

```text
conservative over-cover 的视觉覆盖与拐角保留优于 wire-ring / direction-field 尝试，当前本地代码以它为新基线继续开发。
当前 face-count A/B 已指向 body-to-ring bridge 人造三角化过渡；后续问题不是继续加宽外扩带，而是降低该 transition 的可见 contour 复杂度，并让 multi-surface segmentation 可缝合。
wire-ring / direction-field 两次实验可作为失败证据保留在历史 run 目录中，但不作为最新实现基线。
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

7. raw patch UnifySameDomain 诊断已完成：
   input=route2_expanded_strict_trim/normalized_patch_mm.stp
   output=route2_expanded_strict_trim/raw_patch_unify_probe/normalized_patch_mm_unified.stp
   report=route2_expanded_strict_trim/raw_patch_unify_probe/raw_patch_unify_report.json

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

判读：

```text
这轮修复证明 reverseEdge 不再出现方向翻转（direction_flip_count=0），但 normal leakage 很大，且长三角保护拒绝过多。
当前 blocker 已不是“STL 拓扑脏”，而是 over-cover 采样点对应/桥接策略过保守或错配，导致写入的有效外扩带不足，Geomagic 输出又碎化。
raw patch UnifySameDomain 可把 Geomagic patch 从 68 面降到 59 面、326 边降到 306 边，但 free_edges 仍为 26；内存态 BRepCheck 还会变为 false，写出 STEP 再读回才恢复 true。
因此 UnifySameDomain 不能作为当前 Apply 的直接修复步骤，只能作为“Geomagic 输出确实存在同域碎片”的诊断证据。
下一步仍应先降低 longTriangle rejection 的误伤，或改为局部 bevel/重采样补洞，而不是进入 Apply / sewing 调参。
```

---

## 4. 当前 P0：降低 Geomagic raw patch 碎化 / 修正 over-cover 有效外扩带不足

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

已排除：

```text
raw patch UnifySameDomain 不是直接修复：
  faces 68 -> 59
  edges 326 -> 306
  free_edges 26 -> 26
  in-memory BRepCheck true -> false
  unified STEP roundtrip BRepCheck=true
```

继续检查：

```text
1. 用新的 over-cover artifacts 复跑完整 Route 2 真实样例，确认 rejected 分桶是否仍近似：
   body bridge << boundary-to-ring quad < ring-to-ring quad。
2. 重点查看 `overcover_rejected_boundary_to_ring.stl` 与 `overcover_rejected_ring_to_ring.stl`：
   如果 ring-to-ring 占主导，优先修 ring 对应 / miter 后顺序稳定性；
   如果 boundary-to-ring 占主导，优先修第一圈 offset / source-face 方向。
3. 用 `overcover_worst_points.obj` 定位 max normal leakage 局部区域，判断是否集中在少数 edge/source face。
4. 当前 artifact-only 验证显示 max normal leakage 出现在 edge 1576 / source face 1710；
   该 id 只能作为本轮真实样例线索，不能写进默认测试或硬编码修复。
5. 当前 5 * meanSpacing 阈值可能误杀正常高曲率/角点段；
   下一步应基于 CSV 的 `edge_length_ratio` 和 rejected STL 可视化决定是放宽阈值，还是改局部 bevel / fan 补洞。
6. 只有当 longTriangleCount 和 rejectedCount 收到可解释低值后，再继续定位 failed_patch_face_index=7 / original boundary edge 1585 的 open-wire closure。
```

每轮 Route 2 真实样例目录必须收敛到 `route2_artifacts/`：

```text
1. Geomagic 输入裁剪 STL：
   crop_stl_candidate_<id>.stl
2. Geomagic 原始未归一化导出 STP：
   raw_geomagic_crop_stp_candidate_<id>.stp
3. 归一化后的 patch STP：
   normalized_patch_mm.stp
4. base_removed_candidate：
   base_removed_candidate.stp
5. Geomagic 真实运行记录：
   GEOMAGIC_RUN.md
   geomagic_fit_region.log
   geomagic_generation.stdout.txt
   geomagic_generation.stderr.txt（仅非空时保留）
6. OCCT strict re-trim / PatchReplacementCommand 尝试合并后的 STEP：
   如果未生成，目录表必须标记 exists=false 并记录失败原因。
7. Creo API import-feature sewing 尝试合并后的 STEP：
   creo_api_merged.stp

对应说明文件：
  ROUTE2_SUMMARY.md

不保留低价值文件：
  run_exit_code.txt
  顶层 run_boundary_trim_fill_experiments.stdout.txt / stderr.txt
  run_invocation.txt
  artifact_inventory.json
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
