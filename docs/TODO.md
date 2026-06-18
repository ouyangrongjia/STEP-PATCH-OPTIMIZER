# STEP-PATCH-OPTIMIZER 当前 TODO：Geomagic AutoSurface → T6 replacement / fitting input 路线

> 文档定位：这是当前执行 TODO 文档，用于随开发进度持续更新、替换和勾选。  
> 长期模块边界维护在 `docs/module_design.md`；Geomagic patch preview / Apply 流程维护在 `docs/geomagic_patch_workflow.md`。
> 当前阶段：以 `1f0c3d7`（文档同步）为已手动验证可缝合的几何基准；`d1c00e4` Improve boundary constrained Geomagic patch flow 与 Sharp Contours 实验不进入本轮主线。
> 更新时间：2026-06-17
> 当前判断：默认路线是 `STP Sampled Candidate Surface`，速度更快且效果与 STL crop 接近；`Global Cut Chain` 是可选 STL 全局切链裁剪器，不是默认模式。旧 Stage 3A-Approx / A6 保留为 OCCT 近似平面诊断分支，不再抢占当前主线。当前主线提交为 `a57695d`，即 `1f0c3d7` 可缝合几何基准 + GUI 后台任务、状态显示和门控诊断优化。

---

## 当前执行判断（2026-06-15）

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
   openStepFile、exportStepFile、previewMergeCandidates 和 Patch Apply 长任务转入后台线程。
   viewer / model tree / report / Process Status 更新必须回 GUI 线程执行。
   长任务期间禁用会修改 document / controller 的入口。
   STEP 导出与二次读取校验使用 ExportingStep 状态在后台执行。
   Process Status / report 展示 Gate before / after / STEP roundtrip 的 BRepCheck、free edge、multiple edge 和拓扑计数。

6. Patch preview 运行观测：
   每次 GUI 一键 Patch preview 都会在仓库根目录 log/ 下生成
   patch_preview_<timestamp>_candidate_<id>.log。
   状态栏显示总 elapsed；root run log 记录 output path 解析、fitting STL 生成、RunningGeomagic、ImportingPatch、
   viewer overlay 和 crop diagnostics 的 elapsed_ms / duration_ms。
   fit_region.log 只代表 Geomagic Wrap 脚本内部耗时，不能单独解释 GUI 是否卡住。

7. 下一阶段硬问题：
   Geomagic 导出的 patch 会在 sharp corner / feature junction 附近圆角化，
   原 STP 的棱角可能变成圆边；OCCT BRepCheck、free edge、multiple edge 甚至 STEP roundtrip 通过，
   仍可能在 Creo 等商业 CAD 中出现可见缝隙。
   下一步不应把 Sharp Contours 当作默认主线或单独收口方案，也不应重新合入 d1c00e4。
   应在 a57695d 基准上设计 corner-aware / curvature-aware fitting input 与商业 CAD 近似门控实验。
```

---

## 0.6 旧无用代码清理执行记录（2026-06-16）

```text
已执行 Cleanup-0 / Cleanup-1 / Cleanup-2 / Cleanup-3 / Cleanup-4 / Cleanup-5 / Cleanup-6：
1. 删除未接入 GUI / AppController 的 CylinderRegionMerger / ConeRegionMerger / TorusRegionMerger stub 后端。
2. 删除 RegionMergeStub 和 tests/test_region_merge_stubs.cpp。
3. 从 CMake 移除对应源文件和 stub 测试。
4. Cleanup-6 已删除 PlaneLike / SphereLike / CylinderLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 旧候选类型及相关检测。
5. Cleanup-2 已下线 MainWindow 旧 Plane / Sphere 真实合并菜单、action 和 handler。
6. Cleanup-3 已删除 Plane / Sphere 的 AppController API、Command 类和对应 command 测试。
7. Cleanup-4 已删除 PlaneRegionMerger / SphereRegionMerger 后端和对应后端测试。
8. 依赖 PlaneRegionMerger 的候选检测、候选统计、boundary analyzer 测试已改为各自模块自身断言。
9. Cleanup-5 已删除 RegionMergeResult / RegionMergeOptions；RegionBoundaryAnalyzer 已改用 BoundaryAnalysisFailureReason。
10. 修正 CTest 超时到 300 秒，并让 scripts/test.ps1 传播 native command 失败码。
11. Cleanup-6 已删除 MergeRegionGrower 与 tests/test_analytic_candidate_detection.cpp；MergePlanner 只生成 FeatureBoundedRefit 候选。
12. GUI / Viewer / ModelTree / Inspect / CandidateFilters / 候选统计测试已同步到 FeatureBoundedRefit / Unknown 当前主线。
13. 整体旧无用代码清理完成；后续不应恢复旧 analytic candidate 检测或旧 Plane / Sphere 真实合并链路。
```

验证要求：

```text
.\scripts\build_debug.ps1
.\scripts\test.ps1
git diff --check
rg -n "CylinderRegionMerger|ConeRegionMerger|TorusRegionMerger|RegionMergeStub" src tests CMakeLists.txt
rg -n "mergePlaneCandidateAction_|mergeSphereCandidateAction_|mergeCurrentPlaneCandidate|mergeCurrentSphereCandidate|mergeAllApproximatePlaneCandidates" src/app
rg -n "PlaneRegionMergeCommand|SphereRegionMergeCommand|mergePlaneCandidate\\(|mergeSphereCandidate\\(" src tests CMakeLists.txt
rg -n "PlaneRegionMerger|SphereRegionMerger" src tests CMakeLists.txt
rg -n "RegionMergeResult|RegionMergeOptions|RegionMergeFailureReason|regionMergeFailureReasonToString" src tests CMakeLists.txt
rg -n "BoundaryAnalysisFailureReason" src tests CMakeLists.txt
rg -n "PlaneLike|SphereLike|CylinderLike|ConeLike|TorusLike|FreeformG1|FreeformG2|MergeRegionGrower|test_analytic_candidate_detection|enable_plane_candidates|enable_cylinder_candidates|enable_sphere_candidates|enable_cone_candidates|enable_torus_candidates" src tests CMakeLists.txt
cmake --build --preset windows-msvc-debug --target patch_apply_probe
cmake --build --preset windows-msvc-debug --target corner_baseline_probe
```

---

## 0. 当前最终方案：corner preservation / commercial-CAD-like gate

当前结论不是继续扩大裁剪范围、继续调 sewing tolerance，或重新尝试 Sharp Contours。

必须建立“双层闭环”：

```text
1. 前端输入约束：
   让 Geomagic AutoSurface 在拟合时看到 sharp corner / feature junction 的真实上下文。

2. 后端商业 CAD 近似门控：
   即使 OCCT BRepCheck / free edge / multiple edge / STEP roundtrip 通过，也必须用高密度几何偏差、
   corner drift、edge drift 和 sharpness preservation 判断是否可交付。
```

### 0.1 为什么 OCCT gate 不够

```text
OCCT gate 证明的是 OCCT 自己认为 B-rep 拓扑合法。
它不能证明：
1. Geomagic 拟合曲面在 sharp corner 附近仍贴合原 CAD 几何。
2. 原 sharp edge 没有被拟合成隐性圆角或 G1 平滑过渡。
3. 原 STP boundary wire 附着到 replacement surface 后，商业 CAD 仍会按相同容差闭合。
4. GUI tessellation 中视觉连续的区域，在 Creo / SolidWorks / CATIA 类软件中也无缝。
```

因此，`StrictTopologyGate` 继续负责拓扑水密；新增质量门控负责商业 CAD 近似几何质量。

### 0.2 必须实现的策略组合

```text
1. corner-aware / curvature-aware sampling：
   - 沿 sharp edge、corner vertex、feature junction 增加高密度采样。
   - 曲率突变和二面角突变附近使用更小 sampling spacing。

2. boundary 外扩采样：
   - B2.0 已实现的是原 STP candidate boundary 外侧沿邻接 STP faces 的 guard-band 采样。
   - B2.1 目标路线改为围绕当前 fitting STL patch boundary 生成连续窄 over-cover strip，平行 / 跟随当前面片边缘，宁愿略微覆盖原 STP candidate boundary 外侧。
   - B2.2 改为邻接面约束 support collar：从当前 fitting STL mesh boundary 接出一圈窄 collar，外侧 rail 来自原 STP 非候选邻接 face 的 pcurve 面内采样，用于给 Geomagic 明确 seam 高度上下文。
   - B2.3 改为角点安全 support collar：保留 B2.2 的邻接面 support rail，但对当前 fitting STL mesh boundary 的角点 / offset 跳变点执行局部平滑和最大 offset clamp，避免角点处生成异常凸起；Geomagic `SharpenContours` 只作为显式 A/B 开关，默认关闭。
   - B2.4 改为 Apply 侧 Boundary Edge Rebuild + Local Closure Probe：不再继续改 fitting STL 外扩带，而是在 multi-surface boundary shell 中把原 STP boundary 3D curve 投影到对应 Geomagic fitted surface，显式重建 pcurve / SameParameter，并报告失败 edge id、最大同参偏差和局部闭合风险。
   - 外扩带只用于 Geomagic fitting input，不作为最终 CAD boundary。
   - 拟合后必须继续使用原 STP candidate outer boundary wire / pcurve 在 fitted surface 上重裁剪。

3. feature edge / corner anchor 约束：
   - 将 sharp edge polyline、corner vertices、junction 邻域点作为 anchor set。
   - 如果 Geomagic 不能接受硬约束，则至少作为输入加密点和输出漂移指标。

4. 原 STP boundary re-trim：
   - Geomagic patch outer boundary 仍不能作为最终 CAD boundary。
   - `allowPatchOuterBoundaryFallback=false` 不得放开。

5. CommercialCadLikeQualityGate：
   - 不替代 `StrictTopologyGate`。
   - 在拓扑 gate 之外新增高密度几何门控。
```

### 0.3 新增质量指标

```text
boundary_deviation:
  原 STP boundary dense samples 到 replacement surface / shell 的 max / p95 / RMS。

corner_anchor_drift:
  原 corner vertices / feature junction anchors 到 replacement 的 max / p95。

feature_edge_drift:
  原 sharp edge dense samples 到 replacement feature / seam / surface 的 max / p95。

seam_continuity:
  原 STP boundary dense samples 到 replacement 最近点的 signed normal offset。
  normal 取自原 STP 非候选邻接 face，用于暴露长边 / 角点处的边界高低差。

sharpness_preservation:
  原 sharp edge 两侧二面角或法向突变不能被 replacement 变成近似 G1 圆滑过渡。

surface_cops_like_deviation:
  原 candidate dense samples 到 after shape 的 max / p95 / RMS。

roundtrip_geometry_drift:
  STEP roundtrip 后重复计算上述指标，避免只验证写出前的 OCCT 内存形体。
```

阈值第一版不要写死成“最终工业标准”。先以 `0.01 mm` 作为 warning / report 参考线，真实 fail 阈值通过 Creo A/B 实验标定。

### 0.4 最小 A/B 实验

实验分支：

```text
experiment/corner-preservation-ab
```

实验矩阵：

| 组 | 策略 | 目的 |
|---|---|---|
| A0 | 当前 STP sampled baseline | 建立 corner rounding / commercial CAD gap 数值基线 |
| B1 | corner / feature edge 加密采样 | 已接入脚本开关；验证点集增强是否降低 sharp edge drift |
| B2.0 | 原 STP boundary 外邻接面 guard-band 采样 | 已接入脚本开关；真实样例 drift 下降但 Gate 未通过，不是最终扩宽方向 |
| B2.1 | 当前 fitting STL 外围 over-cover strip + 原 STP boundary re-trim | 已接入脚本；默认 `OverCoverWidth=0.05` 真实样例可进入 Apply / StrictTopologyGate / applied STEP export，但 CommercialCadLikeQualityGate 仍未通过 |
| B2.2 | adjacent-face support collar + seam continuity report | 已接入脚本；真实样例输入 STL 干净且 p95 有改善，但 StrictTopologyGate / CommercialCadLikeQualityGate 仍未通过 |
| B2.3 | corner-safe support collar + optional Geomagic SharpenContours A/B | 已接入脚本；真实样例 `SharpenContours` 可降低 drift，但 StrictTopologyGate 仍因 `FreeEdgeIncreased` 失败 |
| B2.4 | Boundary Edge Rebuild + Local Closure Probe | 已完成最小版；Apply multi-surface shell 会重建原 STP boundary edge 在 fitted surface 上的 pcurve / SameParameter，并输出 pcurve rebuild diagnostics；真实样例 27/27 pcurve rebuild 成功但 StrictTopologyGate 仍因 `FreeEdgeIncreased` 失败 |
| B2.5 | Pre-repair Closure Probe | 已完成最小版，真实复用 patch 验证已跑；PatchReplacementReport / baseline JSON / patch_apply_probe / GUI Apply report 输出 pre/post repair closure stats 与 free edge 定位。真实样例显示 pre-repair 已有 66 条 free edge，repair 后剩 1 条 free edge 且 `appeared_after_repair=true` |
| B2.6 | Local Free-edge Closure Fix | 已完成最小诊断/repair selection 版，并修正 `ShapeValidator` 对 closed seam / degenerated edge 的 free-edge 误计数；真实样例 `StrictTopologyGate` 与 applied STEP readback 已通过，`CommercialCadLikeQualityGate` 仍因 drift 超限失败 |
| B2.7 | Trim / over-cover / seam failure localization | 已完成最小诊断版；PatchReplacementReport / baseline JSON / patch_apply_probe / GUI Apply report 输出 `trim_diagnostics`。真实样例显示 StrictTopologyGate 与 applied STEP readback 仍通过，CommercialCadLikeQualityGate 仍失败；当前主信号是 under-cover 与局部 boundary gap，不是 STEP roundtrip 破坏，也不是 replacement face 明显 over-cover 未裁掉 |
| B3 | corner anchors + B2.3 seam-aware fitting input | 暂缓到 B2.7 结论之后；只有后续修复证明主因确实需要 Geomagic fitting input 约束增强，才继续做 anchor / collar 组合 |

每组必须输出同一份实验报告：

```text
1. Geomagic patch face / edge / shell / solid / BRepCheck。
2. Apply / repair / StrictTopologyGate before-after-roundtrip stats。
3. boundary max / p95 / RMS deviation。
4. corner anchor drift。
5. feature edge drift。
6. sharpness preservation score。
7. STEP roundtrip 后重复测量。
8. Creo 或商业 CAD 打开结果作为最终外部确认。
```

判定标准：

```text
B 组必须同时降低 corner anchor drift、feature edge drift p95/max、boundary projection p95/max，
并且不能让 patch face count、repair failure、StrictTopologyGate 结果明显恶化。

GUI 看起来连续不是成功证据。
OCCT BRepCheck 通过也不是商业 CAD 无缝证据。
```

### 0.5 已落地的 A0 / B1 / B2 自动化入口

当前已新增脚本化 A0 baseline gate：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

复用已有 patch、跳过 Geomagic 时：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId 7 `
  -Patch "D:\path\to\patch.stp" `
  -AllowQualityGateFailure
```

B1 corner / feature edge 加密采样入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2 原 STP boundary 外 guard-band 采样入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2.0 默认同时启用 B1 corner / feature edge 加密，并在原 STP candidate outer boundary 外侧沿相邻 STP faces 采样保守窄带。默认 guard-band 参数为 `-GuardBandSamples 16 -GuardBandRings 1 -GuardBandSpacing 0.10`，用于避免 Geomagic AutoSurface 因过密外扩带输出高碎片 patch。JSON 的 `stp_sampled_fitting` 节会输出：

```text
boundary_guard_band_sampling_enabled
boundary_guard_band_edge_count
boundary_guard_band_sample_count
boundary_guard_band_triangle_count
boundary_guard_band_ring_count
boundary_guard_band_spacing
boundary_guard_band_adjacent_face_sample_count
boundary_guard_band_fallback_sample_count
```

B2.1 基础实现：

```text
目标：
  在当前 STP-sampled fitting STL patch 外围生成一圈连续、小幅、连通的 over-cover strip。
  strip 应跟随当前 patch boundary，而不是沿邻接 STP face 生成尖刺状外带。
  允许拟合输入略微超过原 STP candidate boundary，让 Geomagic surface 覆盖边界外侧。

裁剪：
  Apply 阶段仍丢弃 Geomagic patch outer boundary。
  首选使用原 STP candidate outer boundary wire / pcurve 在 fitted surface 上 re-trim。
  如果探索 surface 与原 STP 边界/邻接面相交，只能作为构造 re-trim 曲线的辅助手段，不能替代原 STP boundary。

验收：
  fitting STL 必须保持单连通，outer strip 不得产生漂浮碎片、锯齿尖刺或大面积邻接特征污染。
  JSON/report 需输出 over-cover width、ring count、sample count、triangle count、fallback/rejected count、boundary coverage。
  成功判据仍是 StrictTopologyGate + CommercialCadLikeQualityGate，而不是视觉 overlay 对齐。
```

B2.1 脚本入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2.1 默认在 B1 加密采样基础上启用 `-OverCoverWidth 0.05 -OverCoverRings 1`，不会启用 B2.0 的 `--b2-boundary-guard-band`。JSON 的 `stp_sampled_fitting` 节新增：

```text
boundary_over_cover_strip_enabled
boundary_over_cover_width
boundary_over_cover_ring_count
boundary_over_cover_sample_count
boundary_over_cover_triangle_count
boundary_over_cover_fallback_count
boundary_over_cover_rejected_count
boundary_over_cover_boundary_coverage
```

B2.2 adjacent-face support collar 入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.2 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2.2 默认在 B1 加密采样基础上启用 `-SupportCollarWidth 0.05 -SupportCollarSamples 16 -SupportCollarRings 1`，不会启用 B2.0 guard-band 或 B2.1 over-cover。JSON 的 `stp_sampled_fitting` 节新增：

```text
adjacent_face_support_collar_enabled
adjacent_face_support_collar_width
adjacent_face_support_collar_ring_count
adjacent_face_support_collar_edge_count
adjacent_face_support_collar_sample_count
adjacent_face_support_collar_triangle_count
adjacent_face_support_collar_adjacent_face_sample_count
adjacent_face_support_collar_fallback_count
adjacent_face_support_collar_rejected_count
adjacent_face_support_collar_boundary_coverage
```

CommercialCadLikeQualityGate 现在同时输出 `seam_continuity`，包含 `max_abs_signed_normal_offset`、`p95_abs_signed_normal_offset`、RMS 和超阈值样本数。该指标专门用于暴露原 STP 邻接面法向上的边界高低差。

2026-06-17 真实样例 `03_配件_Clay.stp` auto-selected candidate 179 的 B2.2 初版参数扫：

```text
共同输入事实：
  fitting STL output_triangle_count=78288
  adjacent_face_support_collar_sample_count=728
  adjacent_face_support_collar_triangle_count=1456
  fallback=0, rejected=0, boundary_coverage=1
  Geomagic input: components=1, boundaryCycles=1, nonManifoldVertices=0, degenerateTriangles=0
  Patch preview: patch_face_count=5, patch_edge_count=20, high_risk=false

SupportCollarWidth=0.03:
  StrictTopologyGate failed: FreeEdgeIncreased
  boundary max/p95=0.106121/0.034150
  seam max_abs/p95_abs=0.068317/0.032183
  corner max/p95=0.106121/0.063592

SupportCollarWidth=0.04:
  StrictTopologyGate failed: FreeEdgeIncreased
  boundary max/p95=0.098052/0.036913
  seam max_abs/p95_abs=0.068727/0.035968
  corner max/p95=0.098052/0.059117

SupportCollarWidth=0.05:
  StrictTopologyGate failed: StepRoundtripFailed, roundtrip free_edges=1
  boundary max/p95=0.090959/0.041232
  seam max_abs/p95_abs=0.069443/0.040150
  corner max/p95=0.090959/0.057764
```

结论：B2.2 初版证明 adjacent-face support collar 生成质量和报告链路可验证，但它不是最终几何收口。它降低了部分 p95 指标并新增了 seam 高低差观测，但仍会引入 Apply / STEP roundtrip 水密风险；下一步不能标记 B2.2 完成，只能继续做 seam-aware surface owner / boundary shell 稳定性或更强的边界约束拟合。

B2.3 corner-safe support collar 入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.3 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

可选 SharpenContours A/B：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.3 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic `
  -SharpenContours
```

B2.3 在 B2.2 adjacent-face support collar 基础上增加角点安全处理：

```text
1. 使用当前 fitting STL mesh boundary 作为内环，保留邻接 STP face pcurve rail。
2. 对 boundary turn、support offset direction jump 和超出目标宽度的 offset 执行局部平滑。
3. 默认 SupportCollarMaxOffsetScale=1.25，即 SupportCollarWidth=0.05 时最大 support offset clamp 到 0.0625。
4. JSON 的 stp_sampled_fitting 节新增：
   - adjacent_face_support_collar_corner_clamp_enabled
   - adjacent_face_support_collar_corner_clamp_count
   - adjacent_face_support_collar_max_offset
5. 根节点新增 geomagic_sharpen_contours，用于区分 SharpenContours A/B。
```

2026-06-17 使用真实样例 `03_配件_Clay.stp` auto-selected candidate 179 跑 B2.3：

```text
B2.3, SupportCollarWidth=0.05, SharpenContours=false:
  adjacent_face_support_collar_corner_clamp_count=16
  adjacent_face_support_collar_max_offset=0.0625
  output_triangle_count=78288
  StrictTopologyGate failed: FreeEdgeIncreased
  Patch Apply success=false
  applied_step_export.success=false
  boundary / feature / corner max drift=0.095701
  boundary p95=0.042605

B2.3, SupportCollarWidth=0.05, SharpenContours=true:
  adjacent_face_support_collar_corner_clamp_count=16
  adjacent_face_support_collar_max_offset=0.0625
  output_triangle_count=78288
  StrictTopologyGate failed: FreeEdgeIncreased
  Patch Apply success=false
  applied_step_export.success=false
  boundary / feature / corner max drift=0.070672
  boundary p95=0.038436
```

结论：B2.3 解决的是角点 support collar offset 失控的输入质量问题，SharpenContours 对真实样例 drift 有正向作用，但它没有解决 replacement shell / repair 后的 free edge 增量。由于 StrictTopologyGate 未通过，本轮没有 applied STEP；这不是脚本漏导出，而是严格门控阻止了提交。

2026-06-16 真实样例 `03_配件_Clay.stp` candidate 179 的 B2.1 默认参数重新生成验证：

```text
命令：
  .\scripts\run_corner_baseline_gate.ps1 -Experiment B2.1
    -SourceStep data\stp\03_配件_Clay.stp
    -CandidateId 179
    -OutputDir data\baseline_runs\scripted_b2_1_gate_candidate_0179_real_regen
    -RealGeomagic

fitting STL:
  output_triangle_count=78288
  boundary_over_cover_sample_count=728
  boundary_over_cover_triangle_count=1456
  boundary_over_cover_boundary_coverage=1
  Geomagic log: boundaryCycles=1, components=1, nonManifoldVertices=0

Geomagic / preview:
  AutoSurface 成功写出 STEP，导入 BRepCheck valid=true。
  Patch preview: patch_face_count=5, patch_edge_count=20, high_risk=false。

Apply / export:
  Patch Apply / StrictTopologyGate: gate_passed=true, STEP roundtrip ok。
  applied_step_export: write_success=true, readback_success=true。

Commercial:
  CommercialCadLikeQualityGate: failed。
  boundary / feature / corner max drift=0.088487。

结论：
  B2.1 已能覆盖 GUI 同源 Apply + StrictTopologyGate + applied STEP export/readback 验收链路。
  它仍不是几何质量收口方案，因为 CommercialCadLikeQualityGate 仍失败。
```

复用已有 patch 只验证 B1/B2 fitting input / report 链路，不证明 Geomagic 重新拟合后的 drift 改善：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId 7 `
  -Patch "D:\path\to\patch.stp" `
  -AllowQualityGateFailure
```

该脚本会构建并调用 `corner_baseline_probe`，默认 candidate id 可用 `auto`，选择最大且 boundary analysis 有效的 `FeatureBoundedRefit` candidate。默认不传 `-RealGeomagic` 且找不到已有 patch 时会跳过，避免普通测试依赖真实 Geomagic。`-Experiment B1` 会提高 STP-sampled fitting STL 的 surface grid 密度，并报告原 STP candidate boundary / feature-boundary edge dense samples 与 corner endpoints；当前 `-Experiment B2` 是 B2.0 alias，会在 B1 基础上生成连接到采样面的 STP boundary guard-band 三角带，并优先投到相邻非候选 STP face；`-Experiment B2.1` 会在 B1 基础上围绕当前 fitting STL patch boundary 生成 over-cover strip；`-Experiment B2.2` 会在 B1 基础上从当前 fitting STL mesh boundary 接出 adjacent-face support collar，并输出 seam_continuity；`-Experiment B2.3` 会在 B2.2 collar 上启用 corner-safe clamp，并可通过 `-SharpenContours` 做显式 Geomagic A/B。默认 `A0` 路径保持不变。B1 不生成孤立 anchor micro facets；B2.0/B2.1/B2.2/B2.3 都不把 guard-band、over-cover strip、support collar 或 Geomagic patch outer boundary 当最终 CAD boundary，Apply 仍必须使用原 STP boundary wire re-trim / multi-surface shell。

底层命令行入口：

```powershell
cmake --build --preset windows-msvc-debug --target corner_baseline_probe
.\build\windows-msvc-debug\Debug\corner_baseline_probe.exe `
  --source-step "D:\path\to\model.stp" `
  --candidate-id auto
```

也可跳过 Geomagic，复用已有 patch：

```powershell
.\build\windows-msvc-debug\Debug\corner_baseline_probe.exe `
  --source-step "D:\path\to\model.stp" `
  --candidate-id 7 `
  --patch "D:\path\to\patch.stp"
```

第一版 `CommercialCadLikeQualityGate` 已实现为独立代码模块：

```text
输入：
  原 STEP document / candidate / original boundary / imported patch。

当前测量：
  1. 原 STP boundary dense samples 到 imported patch 的 max / mean / RMS / p95。
  2. 原 boundary endpoints 去重后作为 corner anchors 到 imported patch 的 drift。
  3. 候选 boundary 上 sharp/free/multiple feature edges 的 drift。

输出：
  baseline_report.json 中的 commercial_cad_like_quality_gate 节。
  该节现在包含 sampling_report，可重复说明：
    - boundary_samples_per_edge / feature_edge_samples_per_edge。
    - corner_anchor_source=original_boundary_edge_endpoints。
    - boundary edge / boundary sample / corner anchor / feature boundary edge / feature edge sample count。
    - feature edge detection result 是否参与本次质量评估。
```

Geomagic staging 必须保持 GUI 兼容：

```text
脚本报告可以写入 data/baseline_runs，
但 Geomagic 输入 STL、输出 STP 和 fit_region.log 必须写入 data/crop_stl / data/crop_stp / data/crop_igs。
否则 Geomagic WriteFile 可能受工作目录 / 路径长度 / 路径形态影响，导致 CLI 与 GUI 结论不一致。
```

明确限制：

```text
1. 这不是 Creo 内核替代品，只是比 OCCT BRepCheck 更接近商业 CAD 风险的自动化近似门控。
2. 它当前测的是 imported patch 几何相对原 CAD boundary 的漂移，后续还要补 STEP roundtrip 后 geometry drift。
3. B1 corner / feature edge 加密采样已完成第一版；B2.0 邻接 STP face guard-band 已完成第一版；B2.1 over-cover strip 已完成基础版且默认真实样例已进入 Apply / StrictTopologyGate / applied STEP export，但 CommercialCadLikeQualityGate 仍失败；B2.2 adjacent-face support collar 与 seam_continuity 已完成最小版，但真实样例仍未通过 StrictTopologyGate / CommercialCadLikeQualityGate；B3 组合策略尚未实现。
4. sampling_report 说明 quality gate 测量点集来源与采样规模；B1/B2 的 fitting STL 增强字段写在 `stp_sampled_fitting` 节，不代表 Geomagic 已获得硬约束 anchor。
5. 真实样例 `03_配件_Clay.stp` candidate 179 的 B2.0 保守参数验证显示：输入 STL 仍为单连通 Geomagic mesh，Patch preview 可通过 high-risk gate，但 Apply 后 `StrictTopologyGate` 因 after BRepCheck/free edges 失败，`CommercialCadLikeQualityGate` 仍未通过。B2.0 已降低 max drift，但不是可交付成功状态。
6. B2.1 不等于继续扩大邻接 STP face guard-band；它要求围绕当前 fitting STL patch 做小幅连续覆盖，再用原 STP boundary 重裁剪。
7. 真实样例 `03_配件_Clay.stp` candidate 179 的 B2.1 默认 over-cover 验证显示 fitting STL 单连通且单 boundary cycle，AutoSurface 输出 5-face patch，可按 GUI 同源流程进入 Apply；StrictTopologyGate 与 applied STEP readback 通过，但 CommercialCadLikeQualityGate 因 max drift 0.088487 失败。
8. `run_corner_baseline_gate.ps1` 是 GUI 同源 pipeline 的脚本化 gate，不是窗口点击级自动化；窗口事件自动化仍未完成。
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
3. 不恢复已删除的 SphereRegionMerger / CylinderRegionMerger / ConeRegionMerger / TorusRegionMerger。
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

当前下一步不是 A6.3，也不是继续扩大 STL crop tolerance；B2.3 corner-safe support collar 与 SharpenContours A/B 已跑真实 Geomagic，结论是输入侧角点 offset 可被约束、SharpenContours 能降低 drift，但 StrictTopologyGate 仍因 `FreeEdgeIncreased` 失败，CommercialCadLikeQualityGate 仍未达标，因此不能直接进入“成功收口”叙事：

```text
corner preservation A/B 后续：
1. A0/B1/B2.0 baseline 已有脚本入口；B2.0 的真实样例结果显示 drift 下降但 Gate 未通过。
2. B2.1 基础版已实现：围绕当前 fitting STL patch boundary 生成连续 over-cover strip，避免继续扩大邻接 STP face guard-band。
3. B2.1 Apply 仍必须使用原 STP candidate outer boundary wire / pcurve re-trim，不能信任 Geomagic patch outer boundary。
4. B2.1 report 已新增 over-cover width、ring count、sample count、triangle count、fallback/rejected count 和 boundary coverage。
5. B2.1 默认参数真实结果已进入 Apply，StrictTopologyGate 与 applied STEP export/readback 通过；CommercialCadLikeQualityGate 仍失败，不能标记为几何收口完成。
6. B2.2 基础版已实现：从当前 fitting STL mesh boundary 接出 adjacent-face support collar，外侧 rail 来自原 STP 非候选邻接 face pcurve 面内采样，并新增 seam_continuity 指标。
7. B2.2 真实参数扫显示输入 STL components=1、boundaryCycles=1、fallback=0、rejected=0、coverage=1，AutoSurface 输出 5-face patch 且非 high-risk；但 SupportCollarWidth=0.03/0.04/0.05 均未通过 StrictTopologyGate / CommercialCadLikeQualityGate。
8. B2.3 基础版已实现：在 B2.2 collar 上增加角点 / offset 跳变局部平滑与最大 offset clamp，报告 corner clamp count / max offset，并新增 `-SharpenContours` 显式 A/B。
9. B2.3 真实样例显示 `SharpenContours=true` 可把 max drift 从 0.095701 降到 0.070672，但 StrictTopologyGate 仍因 FreeEdgeIncreased 失败，Patch Apply / applied STEP export 不提交。
10. B2.4 最小版已实现：T6.7.4 multi-surface boundary shell 会把原 STP boundary 3D curve 显式投影到对应 Geomagic fitted surface，更新 edge pcurve、edge range 和 SameParameter 状态，并输出 pcurve rebuild attempt/success/failure、SameParameter failure、max deviation、failed edge ids。
11. B2.4 还补充了 split boundary segment 在旧邻接面重建时的 pcurve rebuild，避免 split segment edge 只在 replacement face 一侧有合法 pcurve。
12. 真实样例 `03_配件_Clay.stp` auto-selected candidate 179 复用 B2.3 + `SharpenContours` patch 后，B2.4 报告显示 `pcurve_rebuild_attempt=27`、`success=27`、`failure=0`、`same_parameter_failure=0`，但 `max_same_parameter_deviation=0.093401`，StrictTopologyGate 仍因 `FreeEdgeIncreased` 失败，after free edges=1，未导出 applied STEP。
13. B2.4 不移动原 STP candidate boundary，不信任 STL crop boundary / support collar outer rail / Geomagic patch outer boundary，也不通过扩大 sewing tolerance 或放宽 StrictTopologyGate 假装闭合。
14. B2.5 已完成最小版：`PatchReplacementCommand` 在 `assemble_replacement_shape` 成功后、`PatchReplacementRepair` 运行前记录 pre-repair closure snapshot；repair 返回后记录 post-repair snapshot；`PatchReplacementReport`、`corner_baseline_probe` JSON、`patch_apply_probe` 和 GUI Apply report 均输出 pre/post BRepCheck、face/edge/shell/solid、free edge、multiple edge 与 free edge midpoint / endpoints / adjacent face count / nearest original boundary edge / split segment / patch face owner / appeared_after_repair。
15. B2.5 真实样例 `03_配件_Clay.stp` auto-selected candidate 179 复用 B2.3 + SharpenContours patch 后，报告路径为 `data\baseline_runs\scripted_b2_5_apply_reuse_b2_3_sharpen_w005_auto_pre_repair_probe\baseline_report.json`；`pre_repair_brep_check_valid=true`，`pre_repair_free_edge_count=66`，`post_repair_brep_check_valid=true`，`post_repair_free_edge_count=1`，`gate_after_free_edges=1`，StrictTopologyGate 仍因 `FreeEdgeIncreased` 失败。
16. B2.5 的 post-repair free edge 定位为 `appeared_after_repair=true`，midpoint / endpoints 均为 `(-25.0904068, 42.5760562, -9.2595695)`，nearest original boundary edge id 为 `1600`，distance 约 `0.0639331`；这说明 pre-repair assembled replacement 本身已经不闭合，repair 又把剩余风险压缩成一个新出现的点状/退化 free edge。
17. 下一步不应放宽 StrictTopologyGate，也不应删除 PatchReplacementRepair；应先修 replacement shell 构造、surface owner / split 策略和 fitted surface 到原 boundary 的局部偏差，同时审视 repair best-result selection 对退化 free edge 的处理。在这之前仍不应把 B3 corner anchors 当作收口方案。
```

### B2.6 Local Free-edge Closure Fix 具体任务

B2.6 不是新的 Geomagic fitting input 实验，也不是 Blender / quad-remesh 主线。B2.6 的起点是 B2.5 已输出的 free edge 诊断：pre-repair assembled replacement 已有 66 条 free edge，repair 后剩 1 条 post-repair 新出现的点状 / 退化 free edge；在当前真实样例中，该 free edge 的 nearest original boundary edge id 为 1600，距离约 0.0639331。这里的主对象是 free edge，不是 edge 1600 本身。

2026-06-17 最小实现结果：

```text
已完成：
  - PatchReplacementReport 增加 pre/post degenerated free edge count。
  - 每条 free edge diagnostic 输出 length、tolerance、degenerated。
  - matched / nearest original boundary edge 输出 length、tolerance、parameter range、endpoints/midpoint、adjacent old face ids、surface type 和 pcurve availability。
  - split diagnostics 输出 same original edge 上的 split segment count、owner switch count、degenerated split count，以及 nearest split segment id/range/owner/distance。
  - fitted patch projection diagnostics 输出 free edge endpoints/midpoint 到 replacement patch faces 的 projection distance 分布。
  - PatchReplacementRepair best-result selection 在同等 free edge 数下优先选择 degenerated free edge 更少的 sewing result。
  - ShapeValidator 的 free-edge 统计排除 closed seam edge 与 OCCT degenerated edge；普通 open face 仍计为 free edge。
  - corner_baseline_probe JSON、patch_apply_probe 和 GUI Apply report 已同步输出关键字段。

真实样例 `03_配件_Clay.stp` auto-selected candidate 179 复用 B2.3 + SharpenContours patch 后：
  report=data\baseline_runs\scripted_b2_6_apply_reuse_b2_3_sharpen_w005_auto_free_edge_gate_fix\baseline_report.json
  StrictTopologyGate passed
  gate_after_free_edges=0
  gate_roundtrip_free_edges=0
  gate_step_roundtrip_ok=true
  applied_step_export.write_success=true
  applied_step_export.readback_success=true
  pre_repair_free_edge_count=66
  pre_repair_degenerated_free_edge_count=0
  post_repair_free_edge_count=0
  post_repair_degenerated_free_edge_count=1
  appeared_after_repair_degenerated_free_edge_count=1
  remaining post-repair diagnostic edge length=0, tolerance=1e-7
  nearest_original_boundary_edge_id=1600, distance=0.0639331
  same_original_boundary_edge_split_segment_count=0
  nearest_split_boundary_original_edge_id=1582, distance=4.22411
  CommercialCadLikeQualityGate passed=false
  boundary max/p95/rms=0.070672/0.038436/0.019153
  corner max/p95=0.070672/0.058351
  feature max/p95=0.070672/0.038436
  seam max_abs/p95_abs=0.055789/0.037368
```

结论：B2.6 当前已解决这条真实样例上的 StrictTopologyGate free-edge 误报；问题不是单独面片天然有 open edge，而是最终 afterDocument 的闭合 seam / degenerated edge 被旧 validator 当成真实 open boundary。拓扑链路已通过且 applied STEP 可二次读取；几何质量仍未收口，下一步应围绕 boundary / corner / feature drift、局部 fitted surface 到原 boundary 的偏差和 owner/split 稳定性继续压低 `CommercialCadLikeQualityGate` 指标。不能放宽 StrictTopologyGate，也不能把退化边诊断当作可忽略的几何偏差。

目标：让 replacement shell / repair / StrictTopologyGate 在真实样例上靠正确拓扑闭合，而不是靠放宽门控或提交 repair 前 shape。

任务拆解：

```text
1. Free edge local context report：
   - 输出 detected free edge 的 midpoint / endpoints / length / tolerance / adjacent face count。
   - 输出 nearest / matched original boundary edge id；当前真实样例该字段为 1600，但实现不得硬编码。
   - 对 matched original boundary edge 输出 3D curve endpoints / midpoint / range、两侧旧 STP face、surface type、pcurve 是否存在、local normal / tangent。
   - 输出所有 candidate fitted patch face 到 free edge 端点 / 中点 / matched boundary samples 的投影距离分布。
   - 输出 matched split boundary segment 的参数范围、长度、owner patch face、owner 切换点。
   - 输出到 PatchReplacementReport、corner_baseline_probe JSON、patch_apply_probe；GUI 可后置。

2. Surface owner / split stabilization：
   - owner 选择不能只看最近距离；需要加入连续性、相邻 segment owner 稳定性和 edge 端点一致性。
   - 对极短 split segment 设置最小长度 / 合并策略，避免 repair 后形成点状退化 free edge。
   - 如果同一 original boundary edge 上 owner 高频切换，优先报告并拒绝不稳定 split，而不是强行 sewing。

3. Local free-edge-neighborhood input：
   - 只在失败 edge / 邻域做局部 collar 或加密采样，不全局扩大 B2.2/B2.3，也不默认接入 Blender 四边形化。
   - collar / support rail 仍只是 Geomagic fitting input；最终 CAD boundary 仍来自原 STP candidate outer boundary。
   - 验收关注 detected free edge 邻域及其 matched original boundary edge 的 local drift max / p95、owner 稳定性、pre-repair free edge 数量，而不是 STL 网格是否更好看。

4. Repair best-result degeneracy handling：
   - PatchReplacementRepair 的 best-result selection 需要显式报告 zero-length / degenerated free edge。
   - 对“free edge 数量减少但生成点状退化 free edge”的结果加惩罚，避免把局部退化误判为更优 sewing 结果。
   - 仍由 StrictTopologyGate 决定是否提交，不因退化边很短而放行。

5. Verification：
   - 默认测试使用 synthetic shape，不依赖真实 Geomagic 或 `data\stp\03_配件_Clay.stp`。
   - 不把 candidate ordinal 写死进默认测试；真实样例验证继续使用 `--candidate-id auto` 或脚本 `-CandidateId auto`。
   - redo 仍只能复用缓存 afterDocument，不能重新运行 Geomagic / crop / import / repair。
   - 必跑 `.\scripts\build_debug.ps1`、`.\scripts\test.ps1`、`.\scripts\verify_spo.ps1`。
   - 真实样例存在时，复用 B2.3 + SharpenContours patch 跑 B2.6 报告；必须输出 detected edge context、nearest original boundary edge、owner/split diagnostics、pre/post repair 对照、退化 edge 判定、StrictTopologyGate / STEP readback 和 CommercialCadLikeQualityGate 指标。
```

验收判断：

```text
如果 B2.6 使 pre-repair free edge 明显下降，但 post-repair 仍出现点状 free edge：
  下一步继续修 PatchReplacementRepair best-result selection / sewing degeneracy handling。

如果 B2.6 使 detected free edge 邻域 / matched original boundary edge 的局部 drift 降低，但 pre-repair closure 仍差：
  说明 replacement shell 的 owner/split 或 boundary face assembly 仍错误。

如果 gate free edge 归零且 StrictTopologyGate 通过：
  再进入 CommercialCadLikeQualityGate、STEP roundtrip geometry drift 和外部 CAD 验证。

任何分支都不能放宽 StrictTopologyGate，不能删除 PatchReplacementRepair，不能提交 pre-repair shape。
```

### B2.7 Trim / Over-cover / Seam Failure Localization 具体路线

B2.7 是结果诊断关卡，不是几何修复关卡。它的输入应优先复用 B2.6 真实样例已经导出的 applied STEP、同一次 Apply 的 afterDocument 与 baseline JSON；不得因为诊断而重新运行 Geomagic / crop / import / repair，也不得把 `CommercialCadLikeQualityGate` 失败改写成成功。当前要回答的是截图里三类可见失败到底来自哪里：

```text
1. 面片内部 seam 出现缝隙。
2. replacement 与原 STP 邻接面边界没有贴合 / 没有实际缝合。
3. Geomagic patch 或 fitted surface 较大，超出原 STP candidate boundary 的部分没有被裁掉。
```

任务拆解：

```text
1. 结果样本冻结：
   - 使用 B2.6 applied STEP 与对应 baseline_report 作为主样本。
   - CLI 使用 candidate-id auto；默认测试不得写死真实样例路径或 candidate ordinal。
   - B2.7 报告写入 PatchReplacementReport、corner_baseline_probe JSON、patch_apply_probe；GUI Apply report 同步显示摘要。

2. replacement face trim envelope 诊断：
   - 对每个 replacement face 输出 face id、patch face owner、surface type、outer/inner wire count、wire closed、UV bounds、pcurve range。
   - 沿原 STP boundary 和 replacement face 边界采样，输出 boundary sample 到原 candidate boundary 的 max / p95 / RMS、surface projection residual。
   - 用 replacement face 的采样点判断是否落在原 candidate boundary 外，统计 over-cover sample count / ratio / max distance。
   - 如果超出原边界的面片仍可见，优先判为 trim wire / UV loop / face construction 问题，而不是继续扩大 fitting STL。

3. over-cover / under-cover 空间采样：
   - 从原 candidate 区域采样到 replacement faces，统计 under-cover holes / gap samples。
   - 从 replacement faces 采样回原 candidate region，统计 over-cover samples。
   - 输出 worst point、nearest boundary edge、patch face owner、old adjacent face、距离和法向偏移。

4. 边界 seam 诊断：
   - 沿原 STP candidate outer boundary dense sample。
   - 对每个 original boundary edge 输出 replacement surface 与旧邻接 face 的距离、signed normal offset、切向偏移、max / p95 / RMS。
   - 把截图中的长边 / 角点可见缝定位到 original boundary edge id、old face id、patch face owner；该 id 只能来自诊断输出，不能硬编码为任务目标。

5. patch 内部 seam 诊断：
   - 对 imported patch internal seams / replacement internal edges 采样。
   - 输出两侧 replacement faces 在同一 seam 上的 3D 点差、法向夹角、pcurve 是否双侧存在、SameParameter 偏差。
   - 统计 internal_seam_gap max / p95 / RMS 与 worst internal seam id。

6. 阶段对照：
   - 对 imported patch、assembled replacement、post-repair afterDocument、applied STEP roundtrip 四个阶段尽量复用同一组诊断。
   - 输出 roundtrip_changed，判断 STEP 写出 / 读回是否放大裁剪或 seam 问题。
   - 可选导出 overlay 点线，例如 trim / over-cover / seam diagnostic OBJ；CLI / JSON 优先，GUI 后置。
```

最小 JSON 字段：

```text
trim_diagnostics.replacement_face_count
trim_diagnostics.trim_wire_invalid_count
trim_diagnostics.trim_uv_loop_self_intersection_count
trim_diagnostics.over_cover_sample_count
trim_diagnostics.over_cover_ratio
trim_diagnostics.over_cover_max_distance
trim_diagnostics.under_cover_sample_count
trim_diagnostics.under_cover_max_distance
trim_diagnostics.boundary_gap_max / p95 / rms
trim_diagnostics.internal_seam_gap_max / p95 / rms
trim_diagnostics.worst_boundary_edge_id
trim_diagnostics.worst_internal_edge_id
trim_diagnostics.roundtrip_changed
```

判定矩阵：

```text
如果 imported patch 内部 seam 已经有 gap：
  问题在 Geomagic output / patch surface network，下一步修 fitting input 或拒绝该 patch。

如果 imported patch 无内部 gap，但 replacement internal seam 有 gap：
  问题在 BoundaryConstrainedMultiSurfaceShellBuilder 的 internal seam / owner / split / wire construction。

如果原 STP boundary 投影到 fitted surface 的 residual 大：
  问题是 fitted surface 没有覆盖真实 CAD 边界，下一步修局部输入或拒绝该 surface owner。

如果 over-cover samples 大量落在原 candidate boundary 外且导出可见：
  问题是 trim wire / UV loop 没有限制 face，下一步修 face construction 和 re-trim。

如果 under-cover / gap samples 落在原 candidate 区域内：
  问题是 replacement face 没覆盖候选区域，下一步修 surface coverage 或 patch generation。

如果内存 afterDocument 诊断通过但 STEP roundtrip 后失败：
  问题在 STEP writer/readback、pcurve 或 SameParameter 稳定性。
```

验收规则：

```text
默认测试只用 synthetic fixture：
  构造大面 + 原边界裁剪、有意 over-cover / under-cover / internal seam 的形状，验证诊断能识别。

真实样例只作为存在时的辅助验证：
  使用 candidate-id auto 或发现式选择；
  不把真实路径、edge id、candidate ordinal 写入默认测试。

B2.7 完成的标准：
  报告能回答“缝隙和未裁剪来自哪个阶段”，不是要求本阶段修到 Gate / commercial quality 通过。

禁止项：
  不放宽 StrictTopologyGate。
  不删除 PatchReplacementRepair。
  不提交 pre-repair shape。
  不把 STL / Geomagic patch outer boundary 当最终 CAD boundary。
  不把 Blender 四边形化作为默认主线或绕过原 STP boundary re-trim。
```

2026-06-18 最小实现结果：

```text
实现：
  新增 PatchTrimDiagnostics，接入 PatchReplacementCommand。
  PatchReplacementReport / corner_baseline_probe JSON / patch_apply_probe / GUI Apply report 均输出 trim_diagnostics。
  默认诊断使用采样上限，避免真实样例全量 BRepExtrema 距离计算超时。
  默认测试使用 synthetic fixture，覆盖 matching replacement、over-cover、under-cover、internal seam 和无真实路径硬编码。

真实样例：
  source-step=data\stp\03_配件_Clay.stp
  candidate-id=auto，实际选择 candidate 179
  patch=data\crop_stp\03_配件_Clay\03_配件_Clay_candidate_0179.stp
  report=data\baseline_runs\scripted_b2_7_apply_reuse_b2_3_sharpen_w005_auto_trim_diagnostics\baseline_report.json
  applied STEP=data\baseline_runs\scripted_b2_7_apply_reuse_b2_3_sharpen_w005_auto_trim_diagnostics\03_配件_Clay_candidate_0179_applied.stp

结果：
  StrictTopologyGate passed=true
  applied STEP readback=true
  CommercialCadLikeQualityGate passed=false
  boundary/corner/feature max distance=0.0706716
  trim_diagnostics.captured=true
  replacement_face_count=5
  over_cover_sample_count/total/ratio/max=0/29/0/0
  under_cover_sample_count/total/max=81/256/2.03433
  boundary_gap max/p95/rms=0.0706854/2.38984e-14/0.0118242
  worst_boundary_edge_id=1540（诊断输出，不作为硬编码任务目标）
  internal_seam_gap max/p95/rms=0/0/0
  roundtrip_compared=true
  roundtrip_changed=false

判读：
  1. 结果没有显示 replacement face 明显超出原 candidate boundary 后未裁掉；over-cover 为 0。
  2. 原 candidate 区域内仍有 replacement 未覆盖信号；under-cover 为 81/256，最大约 2.03433。
  3. 边界贴合存在局部峰值；boundary_gap_max 约 0.0706854，但 p95 近 0，说明不是整圈漂移，而是局部边界/角点问题。
  4. replacement 内部 seam 诊断未发现 open internal seam gap；截图里看到的“缝”更可能是多面片边界/显示网格或局部贴合误差，而不是 STEP roundtrip 后新产生的拓扑 seam。
  5. STEP 写出/读回没有改变诊断结论；roundtrip_changed=false。

下一步：
  不放宽 StrictTopologyGate。
  不继续围绕某个 edge id 硬修。
  优先修 replacement surface coverage / local boundary projection / owner split 稳定性，目标是降低 under-cover 和局部 boundary gap。
  如果后续需要改 Geomagic fitting input，也应围绕诊断出来的局部 under-cover / boundary-gap 区域做局部约束，而不是全局扩大 STL 或用 Geomagic patch outer boundary 替代原 STP boundary。
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
