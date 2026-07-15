# 当前 TODO

> 文档定位：只记录当前待办、下一步实验和验收边界。已完成或已退役的历史计划放在 `docs/completed_features.md`。

更新时间：2026-07-15

---

## 1. 当前判断

当前开发分支为 `native-surface-reduction`。SR-0～SR-6 的第一版代码和验证均已落地；提交前仍必须完成变更范围检查。GUI 已通过启动冒烟，预览不改文档、Apply、撤销和重做的缓存语义由命令测试覆盖。

当前待办：

```text
1. [x] 最终代码的属性发现工具在四个真实 STEP 上运行 12 个选区；报告位于 `data/runs/20260715_surface_reduction_benchmark_final12b/`。
2. [x] 其中 3 个真实选区由 2 面减为 1 面并达到 50% 目标，参数化、BRepCheck、STEP 回读和商业 CAD 类边界门均通过；另 9 个选区无合法减少。
3. [x] 合成测试覆盖“达到目标”“部分减少”“无合法减少”三种结果。
4. [x] 合成测试覆盖孔洞、高曲率 B-spline、锁边和尖锐边；candidate 179 未进入生产逻辑或默认测试。
5. [x] GUI 通过隐藏启动冒烟；预览不改文档，Apply、撤销和重做不重新拟合由 `ReduceSelectedFacesCommand` 测试验证。
6. [x] SR-6 STEP/STL 混合拟合完成第一版：STEP 修剪面域限定 STL 裁剪并建立单图调和参数化，但当前拟合 UV 随后被局部 PCA 覆盖，混合路径每个节点布局只求解一次；STL 以弱位置项参与，原 STEP 边界在拟合阶段是软约束，拟合后的重裁剪、独立边界终验和全部质量门执行硬拒绝。
```

candidate 179 的 SR-6 手动长期回归位于 `data/runs/20260715_surface_reduction_candidate_0179_hybrid/`：选区仍为 FaceId 1702～1750，共 49 面。动态裁剪原始扫描 STL、目标比例 `0.98` 的独立运行得到 `49→48`；当前目录保留的 `49→46` 使用缓存 STEP 派生 STL，默认 50% 目标下尝试 212 次并接受 3 次。后者最大距离 `0.0286183 mm`、最大法向差 `0.699484°`，BRepCheck、STEP 回读、零自由边和零多重边全部通过，已生成可 Apply STEP。50% 目标仍未达到，因此结果为 `Reduced` 而非 `TargetAchieved`。

缓存 `candidate_0179_reference_crop.stl` 不是 NSR-1 输出；它来自 `20260713_155502_505__boundary-trim-fill` 的 STEP 采样网格，与共享 staging STL 的 SHA-256 同为 `7D094762B7B7FFE31244B65B7066DD5176B6DB85B21A8A1E360C01DF60EF516C`。该 STL 与 STEP 同源，不能作为独立几何真值。candidate 编号只存在于手动运行记录和文档，不进入生产选择逻辑或默认测试。

完整合同见 `docs/surface_reduction_guide.md`。以下 NSR 内容是冻结实验记录，不再代表当前产品主线。

当前开发分支为 `native-surface-refit`。原生自由曲面拟合作为独立分支推进，`feature-bounded-refit` 保留现有 Geomagic patch preview / Apply 稳定基线。

`NSR-1.3` 局部切平面约束 0 / 1 / 4 / 8 有限周期已经完成，四档均未通过全部质量门，NSR-1 尚未结束：

```text
STP
→ 选取 FeatureBoundedRefit 候选区域
→ 提取原始 B-rep 外边界与候选面内部样本
→ BRepFill_Filling 原生单曲面拟合
→ 内存态 / 方向反转后 / STEP roundtrip BRep 状态诊断
→ 独立边界 / 角点 / 内部 / 法向误差与样本覆盖验证
→ 固定总点数的采样分配 A/B
→ 输出单张 patch STEP、JSON 和样本级诊断产物
→ 通过后再复用现有 Apply / repair / StrictTopologyGate / STEP roundtrip
```

切换依据：

```text
1. 最新 crop STL 与拆除候选区域后的 STEP 叠加完整，角点没有缺口。
2. raw Geomagic STEP 的底层曲面在相同角点已经圆角化并发生欠覆盖。
3. normalized patch 保留相同缺口，问题不是单位归一化、pcurve 或 Sewing 单独造成。
4. 最新样例的 multi-surface max projection distance 与 corner max distance 都约为 0.105。
5. 提高 Sewing 容差可以让拓扑 Gate 通过，但不能让商业 CAD 类质量门通过。
```

因此当前决策为：

```text
保留原生单曲面拟合路线；NSR-1.3 已确认 4 / 8 个局部 G1 能降低内部距离，但法向仍超限，并重新引入 face `UnorientableShape` + wire `SelfIntersectingWire`。
停止继续增加局部 G1 或混调 Filling 参数；下一证据任务是在至少三个受支持真实候选上复核同类失败。NSR-2、NSR-1.4 和 NSR-4 继续冻结。
Geomagic 保留为独立后备路线，不再是 native-surface-refit 分支的默认拟合入口。
完整方案以 docs/native_surface_refitting_guide.md 为准。
```

---

## 2. Geomagic 后备路线已落地能力

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
    - diagnostics/candidate_surface_over_cover 新增 overcover_experiment_summary.json/csv。
    - corner_baseline_probe 使用 artifacts/ 保存本轮 fitting STL、raw patch STP/IGES、fit_region log 的 run-local copy。
    - run_boundary_trim_fill_experiments.ps1 会把 over-cover 诊断复制进本轮 diagnostics/。
14. 2026-07-10 测试耗时去重已落地：
    - Route 2 的 Geomagic 生成阶段只生成并预览补片；单位归一化后只执行一次 Apply。
    - 已有补片的 Apply 不再重复生成 fitting STL 和 over-cover 诊断产物。
    - 单位归一化、坐标对齐输入、Creo ModelCHECK 诊断和 Creo Toolkit sewing 对照路线全部保留。
    - StrictTopologyGate 的 after_stats 复用于指标汇报，不再用 step_stats.exe 二次读取同一 STEP。
    - 默认 spo_tests 只跑单元测试；真实文件与真实 Geomagic 改为显式测试目标。
15. 2026-07-10 测试与实验运行目录已统一：
    - 新默认目录为 data/runs/<时间>__<流程>__<模型>__cand-<四位编号>__<变体>。
    - 每轮固定生成 run.json、RUN.md、artifacts/、diagnostics/、logs/。
    - test.ps1 保存 build.log / ctest.log；baseline、Route 2、Creo 和 Toolkit runner 记录 Git 状态、源文件 SHA-256、参数和阶段结果。
    - list_runs.ps1 支持按 workflow/model/candidate/status/latest 查询。
    - data/crop_* 继续作为可覆盖 staging；旧 data/baseline_runs 只读保留。
```

已验证：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
cmake --build --preset windows-msvc-debug --target spo_real_file_tests -- /m:1
cmake --build --preset windows-msvc-debug --target spo_real_geomagic_tests -- /m:1
```

---

## 3. Geomagic 后备基线最新真实样例结果

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

上述目录是历史基线。后续每次新测试必须写入独立 `data/runs/<run-id>/`，并在 `artifacts/` 中保留同类角色文件：

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
该轮 Geomagic 基线的 blocker 已不是“STL 拓扑脏”，而是 over-cover 采样点对应/桥接策略过保守或错配，导致写入的有效外扩带不足，输出又碎化。
raw patch UnifySameDomain 可把 Geomagic patch 从 68 面降到 59 面、326 边降到 306 边，但 free_edges 仍为 26；内存态 BRepCheck 还会变为 false，写出 STEP 再读回才恢复 true。
因此 UnifySameDomain 不能作为该轮 Apply 的直接修复步骤，只能作为“Geomagic 输出确实存在同域碎片”的诊断证据。
该轮后续应先降低 longTriangle rejection 的误伤，或改为局部 bevel/重采样补洞，而不是进入 Apply / sewing 调参。
```

---

## 4. NSR-1 / NSR-1.1 / NSR-1.2 / NSR-1.3 已完成证据与当前待办

本轮已完成 `docs/native_surface_refitting_guide.md` 定义的 NSR-1 探针范围：

```text
1. [x] 实现 SurfaceRefitter::fitSingleFace。
2. [x] 使用原始有序边界作为 G0 约束，不给全部边界施加 G1。
3. [x] 使用原始 STEP 候选面内部点作为拟合约束。
4. [x] 将训练样本与独立验证样本分开。
5. [x] 报告边界、角点、内部距离误差、法向误差、Jacobian/近奇异、曲面复杂度和 BRepCheck。
6. [x] 新增 native_surface_fit_probe，输出单张 patch STEP 和 JSON 报告。
7. [x] 增加合成单元测试，覆盖共面裁剪、人工切分自由曲面、尖角裁剪、孔洞、开放和非流形边界；默认测试不硬编码真实路径或候选序号。
8. [x] 使用 candidate 179 完成中等档手动探针，产物位于 data/runs/20260713_200400_711__native-surface-fit__03_配件_Clay__cand-0179__nsr-1-medium；拟合面和报告成功写出，但因 BoundaryErrorExceeded 未通过 0.03 几何容差门。
```

candidate 179 当前不能只归类为边界失败：

```text
boundary：12 / 416 超限，max=0.0381795，worst edge=1540
corner：2 / 26 超限，max=0.0381795
interior：6 / 96 超限，max=0.0933887，p95=0.0356314，worst face=1709
normal：max=14.1263°，未超过 15°
Jacobian / BRepCheck：通过
```

`BoundaryErrorExceeded` 只是质量门检查顺序中的首个失败项。`NSR-1.1` 已完成以下失败诊断与固定三档实验，仍属于 NSR-1，未启动 NSR-2：

```text
1. [x] 保留现有 failure_reason，并新增 failed_gates，报告全部失败质量门。
2. [x] 报告 candidate_face_count、boundary_valid、完整 solver 配置和 diagnostic patch 语义。
3. [x] 输出 diagnostics/native_surface_fit/surface_error_samples.csv。
4. [x] 输出 diagnostics/native_surface_fit/surface_error_vectors.obj，只标记超限原始点、投影点和误差向量。
5. [x] 增加合成测试，覆盖“边界先失败但内部也失败”的多失败门报告和诊断文件字段合同。
6. [x] 对 candidate 179 完成固定三档受控实验：training=24/48/96；validation=192、boundary samples=32、Jacobian grid=16 固定。
7. [x] 三档保持 tolerance3d=0.03、normal tolerance=15° 和全部 solver 参数不变。
8. [x] 输出三档比较表并按误差趋势完成判读；本轮未混调边界参数，未实现 NSR-4。
```

固定三档结果：

| 档位 | failed_gates | boundary max / p95 / RMS / 超限 | corner max / p95 / RMS / 超限 | interior max / p95 / RMS / 超限 | normal max / p95 | degree / poles / segments | Jacobian / BRepCheck |
|---|---|---|---|---|---|---|---|
| sparse | InteriorErrorExceeded, NormalErrorExceeded | 0.014135 / 0.008378 / 0.003548 / 0 | 0.012527 / 0.011608 / 0.004509 / 0 | 0.154917 / 0.114527 / 0.053045 / 89 | 58.4834° / 14.7188° | 8x8 / 529 / 9 | 通过 / 通过 |
| medium | BoundaryErrorExceeded, CornerErrorExceeded, InteriorErrorExceeded, NormalErrorExceeded | 0.038180 / 0.025657 / 0.011631 / 24 | 0.038180 / 0.034959 / 0.014290 / 2 | 0.093389 / 0.042576 / 0.019742 / 18 | 47.9057° / 9.3050° | 8x8 / 256 / 4 | 通过 / 通过 |
| dense | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.015591 / 0.009086 / 0.004321 / 0 | 0.015591 / 0.013647 / 0.005295 / 0 | 0.041323 / 0.025610 / 0.010416 / 4 | 45.4750° / 9.7698° | 8x8 / 529 / 9 | 通过 / 失败 |

判读：内部 max / p95 从 sparse 的 `0.154917 / 0.114527` 降至 dense 的 `0.041323 / 0.025610`，说明增加训练样本有明显收益，不能据此判定 Plate 路线能力不足。该结论已进入 NSR-1.2 的结构诊断和固定 96 点分配 A/B；medium 的 edge 1540 边界超限未在 dense 档持续超限，因此没有启动边界约束混调。三档 patch 均为 `diagnostic_patch=true`、`patch_acceptable=false`。

`NSR-1.2` 已完成：

```text
1. [x] 记录 Filling 输出、方向反转后和 STEP 二次读取后的 BRepCheck。
2. [x] 输出 face / wire / edge / vertex 的具体 BRepCheck_Status，并定位失败子形状。
3. [x] 输出 edge tolerance、pcurve / SameRange / SameParameter、3D curve 与 curve-on-surface 最大偏差。
4. [x] 为每张源面记录 training / validation 样本数和验证离群点最近训练距离。
5. [x] 为法向样本记录源法向、拟合法向、角度、UV、face id 和投影点，并输出独立 CSV。
6. [x] 固定 training=96、validation=192 和全部 solver 参数，对比纯面积加权与“每张源面至少一点 + 剩余点面积加权”。
7. [ ] 只有某一受控策略同时通过边界、角点、内部、法向、Jacobian、复杂度和 BRepCheck，才允许结束 NSR-1 并进入 NSR-2。
```

固定 96 点 A/B 结果：

| 策略 | failed_gates | interior max / p95 / RMS / 超限 | normal max / p95 / RMS / 超限 | 每面 training min / max / zero | BRep Filling / oriented / roundtrip | roundtrip face / edge |
|---|---|---|---|---|---|---|
| area_weighted | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.041323 / 0.025610 / 0.010416 / 4 | 45.4750° / 9.7698° / 5.3601° / 1 | 0 / 4 / 2 | false / false / true | 1 / 50 |
| face_stratified_area_weighted | InteriorErrorExceeded, NormalErrorExceeded | 0.068507 / 0.024468 / 0.012112 / 8 | 40.7441° / 8.2060° / 4.7287° / 1 | 1 / 3 / 0 | true / true / true | 1 / 43 |

正式运行：

```text
area_weighted:
  data/runs/20260714_014513_419__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.2-area-weighted
face_stratified_area_weighted:
  data/runs/20260714_014616_181__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.2-face-stratified-area-weighted
comparison:
  data/runs/20260714_014717_549__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.2-sampling-ab
```

`NSR-1.3` 已完成：

```text
1. [x] 0 个 G1 完整数值复现 face_stratified_area_weighted 基线；原选择集的 boundary / corner / interior / normal、Filling 误差、复杂度和 BRep 状态均一致。
2. [x] 内部训练样本在采样时保留稳定支撑面、源 face id 和 UV；96 个真实样本的支撑面重算点最大偏差为 1.59e-14。
3. [x] 训练约束总数始终为 96；0 / 1 / 4 / 8 档实际 G1 分别为 0 / 1 / 4 / 8，其余为 G0，原始边界仍只施加 G0。
4. [x] 选择集按 max(distance / 0.03, normal_angle / 15°) 排序并执行确定性空间非极大值抑制，再映射到不同训练样本；1 ⊂ 4 ⊂ 8。
5. [x] 新增独立 192 点终验集；training-selection、training-holdout、selection-holdout 交集四档均为 0，最终质量门只读取 holdout。
6. [x] 完成 0 / 1 / 4 / 8 四档受控运行；除 G1 数量外，训练点、选择集、终验集、边界和全部 solver 参数逐字段一致。
7. [x] 每档均保留 run.json、RUN.md、patch STEP、JSON、CSV、OBJ、日志和 step_stats；比较目录同时输出 CSV / Markdown。
8. [x] 四档均为 patch_acceptable=false；按停止规则不再增加 G1 或混调 Filling 参数，不进入 NSR-2。
```

| G1 | failed_gates | selection interior max/p95/RMS/over | selection normal max/p95/RMS/over | holdout interior max/p95/RMS/over | holdout normal max/p95/RMS/over | BRep Filling/oriented/roundtrip | Jacobian near/flip | degree/poles/segments |
|---:|---|---|---|---|---|---|---|---|
| 0 | InteriorErrorExceeded, NormalErrorExceeded | 0.068507/0.024468/0.012112/8 | 40.7441°/8.2060°/4.7287°/1 | 0.054879/0.026982/0.010159/8 | 23.5646°/7.2512°/3.9602°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 1 | InteriorErrorExceeded, NormalErrorExceeded | 0.068275/0.024458/0.012137/8 | 38.9583°/8.2059°/4.6291°/1 | 0.054914/0.026859/0.010164/8 | 23.1334°/7.2574°/3.9184°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 4 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.044250/0.022206/0.009061/4 | 38.7154°/7.6144°/4.3457°/1 | 0.038039/0.017075/0.007938/2 | 22.9609°/7.2526°/3.5619°/1 | false/false/true | 0/0 | 8x8/529/9 |
| 8 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.044311/0.022247/0.008834/4 | 38.6458°/6.4799°/4.2270°/1 | 0.038155/0.017076/0.007676/2 | 22.8859°/6.7965°/3.4800°/1 | false/false/true | 0/0 | 8x8/529/9 |

4 / 8 个 G1 将 holdout interior 超限数从 8 降到 2，但 normal max 仍为约 23°，且内存态 face=`UnorientableShape`、wire=`SelfIntersectingWire`；STEP roundtrip 虽为一张有效面，也不能绕过内存态失败。candidate 179 单例只形成能力边界证据，不满足 NSR-4 解冻条件。

正式运行：

```text
data/runs/20260714_142505_621__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.3-local-g1-0
data/runs/20260714_142629_618__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.3-local-g1-1
data/runs/20260714_142756_421__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.3-local-g1-4
data/runs/20260714_142922_182__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.3-local-g1-8
data/runs/20260714_143049_479__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.3-local-g1-tiers
```

当前待办：在至少三个满足单外轮廓、无孔、连通、光顺支持范围的真实候选上复核同类失败；完成前不解冻 NSR-4，也不启动 NSR-1.4 或 NSR-2。

`NSR-4` 解冻条件：

```text
以下条件必须同时满足：
1. candidate 179 的 1 / 4 / 8 个局部 G1 周期均无法同时通过 0.03 距离门、15° 法向门和内存态 BRepCheck。
2. 同一失败模式在至少三个满足“单外轮廓、无孔、连通、光顺”支持范围的真实候选中重复出现。
3. 多个候选均表现为距离与法向之间搬移误差、重新引入 wire 自交 / Jacobian 问题，或者指标已无稳定改善趋势。

仅 candidate 179 单例失败时，不足以直接启动自研求解器；它也可能只是单曲面路线的困难样例，应保留 Geomagic 后备分流。

NSR-4 不是重写完整 NURBS 内核。第一版只实现受限 B-spline 控制点求解：固定参数化和节点布局，组合边界位置、内部位置、局部切平面和公平性正则项，结果仍交给 OCCT Geom_BSplineSurface / B-rep / STEP 导出。
```

`NSR-1.4` 后续冻结计划：

```text
目标：把固定 96 点从实验常量改为按区域几何复杂度计算的训练预算，但不在 NSR-1.3 中实现。

1. [ ] 训练预算主要由连续目标区域的面积、边界长度、角点复杂度、曲率 / 法向变化和目标容差决定，不由原始 STEP 面片数量决定。
2. [ ] 以 N_base ≈ area / target_spacing² 形成基础预算，再按曲率和法向变化提高局部密度，并设置明确的最小值、最大值与复杂度上限。
3. [ ] 在目标区域的三角化表示上使用确定性的空间均匀采样，并向高曲率区域加权；源面 id 只用于回查支撑面与 UV，不作为“一面一个点”的生产分配依据。
4. [ ] 局部 G1 数量随自适应训练预算按比例形成有限档位，不继续固定为 1 / 4 / 8。
5. [ ] 若估算预算超过单曲面复杂度上限，或区域内存在单张光顺曲面无法表达的多个曲率分区，应明确返回 unsupported，转入多曲面或 Geomagic 后备，而不是无限增加控制点。

解冻条件：NSR-1.3 完成并形成局部 G1 的有效性证据之后，再单独设计、实现和验证；不得与本轮局部 G1 实验混在一起。
```

一步到位的边界：用户可以只触发一次“原生重拟合”，但内部允许执行少量固定策略并由质量门选择结果；不把“单次操作”误解为“一组固定 Filling 参数保证任意自由曲面成功”。

NSR-1.1 验收：

```text
1. JSON 能同时表达 boundary / corner / interior 等全部失败门。
2. CSV/OBJ 能定位 edge 1540 和 face 1709 附近的超限样本，不依赖截图猜测。
3. 三档只改变训练样本数量，每档都有独立 run.json、RUN.md、patch、JSON、CSV、OBJ 和日志；已验证每档 JSON/CSV 均有 1050 条样本，OBJ 只含超限误差向量。
4. 若内部误差随训练密度明显下降，继续 OCCT Filling；若基本不变且 degree 持续达到 8，只记录 NSR-4 证据；若只剩边界超限，另立边界约束实验。
```

NSR-1 暂不做：

```text
- 不改 GUI。
- 不做原生多曲面布局。
- 不接自动 Geomagic 回退。
- NSR-1.3 证据形成前不解冻 NSR-4；即使解冻，也不自研通用 NURBS 内核。
- 不放宽 StrictTopologyGate、Sewing 或商业 CAD 类质量门。
- 不在 NSR-1.1 同时调整训练密度和 Plate / curve / iteration / degree / segment 参数。
```

Geomagic 旧基线保留为后备诊断证据：

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

只有原生路线明确失败并进入 Geomagic 后备时，才继续检查：

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

每轮新的 Geomagic 后备真实样例必须收敛到统一运行目录：

```text
data/runs/<timestamp>__boundary-trim-fill__<model>__cand-<id>__route1-route2/
  run.json
  RUN.md
  artifacts/
  diagnostics/
  logs/
```

`artifacts/` 保留：

```text
1. Geomagic 输入裁剪 STL：
   crop_stl_candidate_<id>.stl
2. Geomagic 原始未归一化导出 STP：
   raw_geomagic_crop_stp_candidate_<id>.stp
3. 归一化后的 patch STP：
   normalized_patch_mm.stp
4. base_removed_candidate：
   base_removed_candidate.stp
5. OCCT strict re-trim / PatchReplacementCommand 尝试合并后的 STEP：
   如果未生成，目录表必须标记 exists=false 并记录失败原因。
6. Creo API import-feature sewing 尝试合并后的 STEP：
   creo_api_merged.stp

对应说明文件：
  ROUTE2_SUMMARY.md

logs/ 保留：
  GEOMAGIC_RUN.md
  geomagic_fit_region.log
  geomagic_generation.stdout.txt
  geomagic_generation.stderr.txt（仅非空时保留）

diagnostics/ 保留：
  candidate_surface_over_cover/

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
1. native_surface_fit_probe 输出一张 TopoDS_Face 和完整 JSON 报告。
2. 原始边界 G0 误差、角点误差和内部独立验证误差满足配置容差。
3. 角点不被圆角化或抹掉。
4. BRepCheck 有效，无 Jacobian 翻转或近奇异区域。
5. degree / poles / knots / segments 没有达到不合理规模。
6. 合成单元测试通过，真实样例成功或失败都有可定位报告。
7. 进入 Apply 后仍必须通过 repair、StrictTopologyGate、STEP roundtrip 和商业 CAD 类质量门。
```

如果仍失败，按下面分流：

```text
边界 Wire 无法构造：
  修边界顺序、方向或输入分析，不进入拟合调参。

G0 边界误差超限：
  检查边约束和曲线兼容性，不提高 Sewing 容差。

边界合格但内部误差大：
  只比较固定三档样本/复杂度，禁止无限调参。

距离合格但法向误差大：
  记录为后续切平面约束需求，NSR-1 返回失败。

Jacobian 翻转、近奇异或曲面复杂度失控：
  拒绝单曲面结果。

OCCT Plate 路线在受控参数内失败：
  记录失败证据，再决定启动自定义 B-spline 求解或 Geomagic 后备。

Geomagic 后备仍圆角化或底层曲面欠覆盖：
  覆盖 Gate 失败并拒绝，不进入 Apply / Sewing。
```

---

## 6. 冻结范围

不要做：

- 不恢复 BestFitFreeform。
- 不恢复旧 guard-band / 旧 over-cover strip 的历史实现作为主路线。
- 不把 Geomagic patch outer boundary、STL crop boundary、support collar 外环当最终 CAD boundary。
- 不放宽 `StrictTopologyGate`。
- 不通过继续加大 adjacent-face support collar 主宽度来掩盖方向错误。
- 不把局部原生单曲面拟合扩张为整个潮玩件的全曲面重建。
- NSR-1 不做多曲面布局、GUI 接入、自动 Geomagic 回退或通用 B-spline 求解器。
- NSR-1 原始边界只做 G0，不恢复“全部边界 G1”的失败实验。
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
