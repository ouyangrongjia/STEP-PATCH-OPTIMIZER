# 实现状态与待办清单

> 文档定位：本文件用于记录当前工程实现进度、已完成功能、部分完成模块、待办事项与验证方式。  
> 长期模块规划放在 `module_design.md`，本文件随开发进度持续更新。

---

## 1. 当前版本概览

当前项目已形成 MVP 工程闭环：

```text
STEP 读取
→ B-rep 拓扑索引构建
→ OCCT Viewer 显示
→ face/edge 选择与多选
→ 特征边检测
→ 用户锁边/解锁
→ same-domain 合并
→ undo/redo
→ 基础合法性检查
→ STEP 导出
```

当前重点已经从”搭建项目结构”和旧 OCCT 平面合并扩展转向：

```text
1. Geomagic AutoSurface patch generation / import / preview / apply 主线。
2. 当前默认 fitting input mode：STP Sampled Candidate Surface。
   - 直接从当前 STP candidate faces / boundary 采样生成 fitting STL。
   - 不要求先加载原始 STL。
   - 当前推荐作为默认模式，速度更快，效果与 STL 裁剪路线接近。
3. 备用 / 诊断路线：原始 STL crop。
   - Legacy centroid-only crop 保留为基线。
   - Conservative boundary-band crop 保留为 A/B 验证。
   - Global Cut Chain crop 已接入 GUI，作为 STL 全局切链裁剪器。
4. Stage 3A-Fix / A6 保留为旧 OCCT 近似平面路线的诊断与研究分支。
5. 暂停扩展球面/圆柱/圆锥/自由曲面真实合并。
6. 当前主线提交为 `a57695d`：以 `1f0c3d7` 已手动验证可缝合的几何处理为基准，只额外合入 GUI 后台任务、状态显示和 Gate 诊断优化。
7. 下一阶段重点转为 Geomagic 输出在 sharp corner / feature junction 附近圆角化所造成的商业 CAD 缝隙问题；该问题不能只靠 OCCT BRepCheck / free edge / multiple edge 判断。
```

### 1.1 当前关键诊断（2026-05-27）

```text
GUI 显示”看起来连续” ≠ STEP/B-rep 拓扑合法。

平面重建后，boundary wire / pcurve / orientation / 闭合关系不正确
→ 外部软件（非 OCCT）重新解析 STEP 时暴露为缺面、飞面、无限平面、开壳。

这不是渲染问题，而是 B-rep 拓扑 / 裁剪边界不稳定问题。
```

因此旧 OCCT 近似平面路线的安全收口项是 Stage 3A-Fix：
PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild。

---

### 1.2 Geomagic AutoSurface 当前关键诊断（2026-06-02）

```text
Geomagic AutoSurface “运行成功”不等于 patch 可接受。

已确认两个独立问题：

1. STL 局部裁剪结果若带内部孔洞 / 非流形顶点，会传递到 AutoSurface 输出。
   当前脚本默认执行 RepairMesh / RemoveNonManifoldVertices / FillSmallHoles。

2. AutoSurface.numPatches=1 只是 Geomagic 的近似 NURBS patch 目标，
   不保证导出的 STEP 只有 1 个 B-rep face。
   对当前 crop STL，Mechanical + autoMerge=true 明显优于 Organic。
```

真实样例验证结果：

```text
输入：data/crop_stl/<step文件stem>/<step文件stem>_candidate_0179.stl
输出：data/crop_stp/<step文件stem>/<step文件stem>_candidate_0179.stp
日志：data/crop_stp/<step文件stem>/<step文件stem>_candidate_0179_fit_region.log

修复前：boundaryCycles=11, nonManifoldVertices=2
修复后：boundaryCycles=1, nonManifoldVertices=0, FillSmallHoles numFilled=22

Mechanical + autoMerge=true + numPatches=1:
faces=12, edges=50, BRepCheck valid=true

对比：
Organic + autoMerge=true: faces≈273
Organic + autoMerge=false: faces≈494
Organic detail/tolerance 调参：face count 仍≈273
```

---

## 2. 最新进度同步

本次状态更新确认以下事项已经完成：

```text
1. UnlockEdgeCommand 已补充 document 校验。
2. 用户锁边进入 protectedEdges 的测试已补充。
3. GUI 手动流程验证已通过。
4. MergePatchCommand undo 时清空锁边状态是当前确认的正确语义。
5. Stage 2 Generic Merge Candidate Framework 已完成：MergeCandidate / MergePlanner / MergeRegionGrower 可生成 PlaneLike 候选区域。
6. Stage 2.5 Candidate GUI Preview 已完成：支持 Top N、显示全部非隐藏候选、按 ID 高亮和清除候选高亮。
7. Stage 2.6 Candidate Selection / Rejection 已完成：支持候选区域点击选择、接受、拒绝、隐藏、恢复和状态统计。
8. Stage 3-0 Analytic RegionMerger Framework Preparation 已完成：已预留统一结果、选项、失败原因和解析图元 merger stub。
9. Stage 3A PlaneRegionMerge 已完成基础可用版：支持 PlaneLike candidate 的真实 planar trimmed face 替换、Command 层执行、undo/redo、批量平面候选合并和 BRepCheck 报告。
10. PlaneRegionMerge 已修复合并后实体数丢失问题：当前通过原 shape 上的 face 级 reshape 保留 solid/shell 容器，并对候选外边界执行受限 edge-only same-domain 简化，减少共线边界分段。
11. Stage 2.7 Face / Candidate Inspect 已完成：点击 face 可查看 surface type、候选归属、candidate type/status/risk/metrics，并支持未命中原因提示。
12. Stage 2.8 Analytic Primitive Candidate Detection 已完成基础版：PlaneLike 保持原行为，CylinderLike/SphereLike/ConeLike 支持基础检测，TorusLike/Freeform 类型保留通道。
13. Stage 2.9 Multi-type Candidate Preview 已完成：报告、模型树、按类型筛选、Viewer 多类型配色和 Face Inspect 已支持 PlaneLike / CylinderLike / SphereLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 / Unknown 的显示通道。
14. Stage 2.8 Enhancement A 已完成：CylinderLike 支持对 B-spline / Bezier / SurfaceOfRevolution 近似圆柱 face 做保守采样拟合检测；CylinderLike 仍要求至少 2 个 face 形成可合并候选；本阶段只生成候选，不执行 CylinderRegionMerge。
15. Stage 2.8 Enhancement B 已完成：ConeLike 支持对 B-spline / Bezier / SurfaceOfRevolution 近似圆锥/圆台 face 做保守采样拟合检测；ConeLike 仍要求至少 2 个 face 形成可合并候选；本阶段只生成候选，不执行 ConeRegionMerge。
16. Stage 3-S Shared Primitive Result Fields 已完成：RegionMergeResult 已新增通用 primitive 参数字段（primitive_center_x/y/z、primitive_axis_x/y/z、primitive_radius、primitive_secondary_radius、primitive_angle_degrees、primitive_fit_error）；Stage 3A PlaneRegionMerge 仍保持原有行为，成功时填充 primitive_axis_* 和 primitive_fit_error；Cylinder / Sphere / Cone / Torus 真实合并仍未实现；后续 Stage 3D / Stage 3B 将复用这些字段。
17. Stage 3D SphereRegionMerge 已完成稳定版调整：SphereLike candidate 合并不再手工构造 spherical trimmed face，也不再使用 ReShape 删除 face；当前改为只放开候选内部边并保护其他所有边，然后调用 OCCT `ShapeUpgrade_UnifySameDomain` 合并同域球面片，避免手工球面边界导致缺面和飞线；支持所有已接受 SphereLike candidates 的批量合并；支持全部可合并 SphereLike candidates 的实验性批量合并；支持 Command 层执行和 undo/redo；支持 AppController 和 GUI 三个入口；写入 RegionMergeResult primitive_center / primitive_radius / primitive_fit_error；Cylinder / Cone / Torus 真实合并仍未实现。
18. SphereRegionMerge 一键批量合并已增加防护：一键全部可合并球面候选会跳过 High risk 和单 face 候选；批量合并保护候选外部边和其他所有边，只允许候选内部边被同域合并消除；若结果丢失拓扑、solid 数变化或 face 数未下降则失败回滚。
19. GUI 主工具栏已按功能收敛为下拉入口：选择、候选显示、候选状态、合并、检查/导出；候选显示下拉中补充了直接显示 PlaneLike / SphereLike 候选的入口，避免工具栏横向溢出。
20. GUI 平面候选入口已区分“PlaneLike 预览候选”和“可真实平面合并候选”：严格合并入口只显示原生 `GeomAbs_Plane`、边界有效、非隐藏/非拒绝的候选；B-spline backed planar-like 候选会明确报告为预览专用，不再显示 Unknown 失败原因。
21. Stage 3A-Fix T3 Unsafe Candidate Rejection Report 已完成：`RegionMergeFailureReason` 已有稳定字符串转换；平面/球面候选合并报告会输出 candidate、failure reason、message、统计、BRepCheck 和失败时 `document was not modified / rollback applied`。
22. Stage 3A-Fix T4 RegionBoundaryAnalyzer 已完成：新增独立边界分析器，在 PlaneRegionMerger 真实重建前检查单连通区域、单一闭合外环、无内环/洞、无 non-manifold 边；复杂 boundary 先明确拒绝，不执行修复。
23. Stage 3A-Approx A1 已完成：PlaneRegionMergeOptions 新增 `allow_approximate_planar_surfaces=false` 和 `approximate_plane_max_deviation=0.01`；默认继续保持 T2 strict mode，显式开启后 B-spline backed PlaneLike 可越过 strict native Plane 检查进入后续拟合/偏差检查，但本阶段未实现真实 B-spline 平面重建。
24. Stage 3A-Approx A2 已完成：`allow_approximate_planar_surfaces=true` 时低误差 B-spline backed PlaneLike 可复用拟合平面、T4 边界分析、planar trimmed face 构造、ShapeValidator/BRepCheck 和 STEP roundtrip gate 完成近似平面重建；高偏差候选返回 `DeviationTooLarge`，失败时 document/stats 保持不变；GUI 实验入口仍留给 A4。
25. Stage 3A-Approx A3 已完成：strict native Plane 与 approximate B-spline planar rebuild 统一使用 `RegionBoundaryAnalyzer`；构造 boundary wire 时只使用 `analysis.ordered_boundary_edges`，不依赖原始 `candidate.boundary_edges` 顺序；open/disconnected/multiple-loop/hole/non-manifold/branching boundary 仍按 strict 策略拒绝，不做 ShapeFix 或 pcurve 深度修复。
26. Stage 3A-Approx A4 已完成：GUI 新增“实验性合并当前近似平面候选”和“实验性合并全部近似平面候选”入口；原 strict 平面合并入口保持 `allow_approximate_planar_surfaces=false`，实验入口显式开启 `allow_approximate_planar_surfaces=true` 并在报告中输出 mode、approximate_plane_max_deviation、失败原因、文档回滚状态、统计和 BRepCheck。
27. Stage 3A-Approx A5 已完成：补齐 strict/approx 自动测试和阶段收口验证；覆盖 B-spline backed PlaneLike strict 拒绝、approx 低误差成功、高误差失败、RegionBoundaryAnalyzer invalid boundary failure、batch approx mixed valid/invalid 跳过语义、全部 invalid failure、STEP roundtrip failure、失败 rollback 和 command undo/redo 既有路径。
28. Geomagic AutoSurface T4 路径已改为真实 `wrapCore.exe --script` + `FIT_REGION_*` 环境变量；真实脚本不再要求 `config.json` 作为调用输入。
29. Geomagic Python 脚本已对齐标准 `fit_region.py` 风格：只输出一个 fit_region log，不再写大量 result JSON；中间 IGES keepTemp 时作为 `<output>_autosurface.igs` sidecar 保留。
30. Geomagic Python 脚本已加入默认网格修复：`RepairMesh`、`RemoveNonManifoldVertices`、`FillSmallHoles`；默认 `FIT_REGION_REPAIR_MESH=1`。
31. Geomagic AutoSurface 默认参数已改为 `geometry=Mechanical`、`autoMerge=true`、`adaptiveFit=false`、`numPatches=1`；真实 crop 样例 face count 从 Organic 的约 273 降到 Mechanical 的 12。
32. GeomagicAutoSurfaceBackend 已支持无 result JSON 兜底：真实脚本产出 STEP 即可判定成功，避免 wrapCore.exe 非零 exit code 误判。
33. PatchImportService 已完成：可导入 STEP/STP/IGS/IGES，返回 shape、face/edge/shell/solid 统计、bbox、BRepCheck；不接受或修改 ShapeDocument。
34. PatchImportService 真实接入测试已拆分：默认测试只导入已有 crop_stp / crop_igs 文件；设置 `SPO_ENABLE_REAL_GEOMAGIC_TESTS=1` 时跑通 crop STL → Geomagic → STEP → PatchImportService。
35. `tools/step_stats` 已可用于导入 STEP/IGS 并输出 face/edge/shell/solid/bbox/BRepCheck 统计，用于判断 Geomagic 输出是否适合继续 overlay / Apply。
36. Geomagic AutoSurface T5.2.0 PatchArtifactLocator 已完成：可从 local STL / GeomagicAutoSurfaceResult 动态定位 patch STEP、IGES sidecar 和 fit_region log；生产逻辑不写死当前真实样例文件名；Windows 中文目录 / 中文 stem 查找使用 native `std::filesystem::path` 拼接，避免 UTF-8 narrow string 重建文件名导致同名 patch 查找失败。
37. Geomagic AutoSurface T5.2 patch overlay 已完成基础版：GUI 支持“从 local STL 定位并导入 Patch”和手动 CAD patch 文件导入；前者选择 local STL 作为 artifact key 并自动定位同 stem patch，后者直接选择 STEP/STP/IGS/IGES。Viewer 以独立 AIS_Shape 叠加显示，清除或重复导入不会修改主 ShapeDocument。
38. Geomagic AutoSurface T5.3 PatchPreviewReport 已完成基础版：报告 candidate/source 统计、artifact 路径、patch 拓扑统计、bbox deviation、BRepCheck、warning 和 recommended action；multi-face patch 仅作为 warning，明显异常标记 HighRisk。
39. Geomagic AutoSurface T5.3.1 一键 Patch cutout overlay preview 已完成：GUI 可对当前 FeatureBoundedRefit candidate 自动裁剪 local STL、运行 Geomagic、导入 patch，并在 Viewer 中以 visual-only 方式隐藏 source faces 后叠加 patch；主 ShapeDocument 不修改。
40. 仓库脚本已补齐：`scripts/verify_spo.ps1` 作为本地统一验证入口，`scripts/run_geomagic_patch.ps1` 作为手动复现 Geomagic patch 生成入口；`verify_spo.ps1` 会在 Geomagic/Patch 相关代码或仓库脚本变更但进度文档未同步时失败，避免后续遗漏文档同步。
41. Geomagic AutoSurface T5.4 Patch Apply 状态机与按钮门控已完成：新增 `RegionPatchStatus` / `PatchApplyDecision`，GUI 新增“应用当前 Patch（T6 占位）”；只有 PreviewReady 且非 HighRisk、bbox/BRepCheck/face count 合法时允许请求 Apply。T5.4 当时点击后仅进入 ApplyPending 并提示 T6 未实现，不修改 ShapeDocument，不执行 replacement / sewing / ShapeFix / StrictTopologyGate；该占位入口已在 T6.5 被真实 Apply 流程替换。
42. Geomagic AutoSurface T5.4.1 patch preview 生命周期安全修正已完成：打开新 STEP、undo/redo 成功、旧 SameDomain/Plane/Sphere 合并成功修改主模型、GUI refreshDocumentViews 刷新主模型时会清空旧 patch preview 状态和 viewer overlay，避免旧 patch 被误用于新模型。
43. Geomagic AutoSurface T6.0 PatchReplacement 输入结构与 MultiFacePatchAnalyzer 已完成：新增 `PatchReplacementInput` / `PatchReplacementReport` / `PatchReplacementFailureReason` / `MultiFacePatchAnalyzer`，在真实 replacement 前统一校验 document、candidate、boundary、imported patch 和 preview report，并分析 TopoDS_Face / Shell / Solid / Compound 形态下的 imported patch 多面片拓扑；`patchFaceCount > 1` 不作为失败条件，multi-face patch 标记为主路径输入；本阶段不修改 ShapeDocument，不实现 Command / GUI / sewing / ShapeFix / StrictTopologyGate。
44. Geomagic AutoSurface T6.2 StrictTopologyGate 最小可用版已完成：新增 `StrictTopologyGateInput` / `StrictTopologyGateReport` / `StrictTopologyFailureReason` / `StrictTopologyGate`，在 PatchReplacementCommand 提交前评估 before/after ShapeDocument；Gate 检查 after BRepCheck、free edge / multiple edge 增量、solid/shell 一致性、bbox、STEP export 和 STEP roundtrip；`replacementFaceCount > 1` 不作为失败条件，multi-face replacement 只记录 warning；本阶段不修改 ShapeDocument，不实现 PatchReplacementCommand / GUI / Geomagic 调用 / STL 重新裁剪。
45. Geomagic AutoSurface T6.1 BoundaryConstrainedPatchBuilder 已完成：新增 `BoundaryConstrainedPatchBuilder` / `BoundaryConstrainedPatchBuildOptions` / `BoundaryConstrainedPatchBuildResult`，可基于 `PatchReplacementInput` 与 `MultiFacePatchAnalysis` 构造 one-face fragment 或 multi-face compound replacement fragment；multi-face patch 是主路径，不因 `patchFaceCount > 1` 返回 unsupported；内部 patch seam 会保留到 `internalPatchEdges`；boundary mismatch 只记录 warning，最终可提交性留给 StrictTopologyGate / 后续 sewing；本阶段不修改 ShapeDocument，不实现 Command / GUI / Geomagic 调用。
46. Geomagic AutoSurface T6.3 PatchReplacementCommand 最小可用版已完成：新增 `PatchReplacementCommand`，通过 Command 层执行 `PatchReplacementInput` 校验、`MultiFacePatchAnalyzer` 分析、`BoundaryConstrainedPatchBuilder` 构造 replacement fragment、`StrictTopologyGate` 验证、Gate 成功提交 afterDocument、Gate 失败 rollback，并支持 undo/redo；redo 只复用缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新读取固定 patch 路径；multi-face replacement fragment 是主路径，`patchFaceCount > 1` 不返回 unsupported。当前 T6.3 未真正删除 / 替换 candidate source faces，未执行 sewing / ShapeFix / SameParameter，未接入 GUI；最小 afterDocument 策略是将 imported patch top-level shape 作为 gated afterDocument candidate，普通局部 patch 若无法满足 StrictTopologyGate 会失败并保持主 ShapeDocument 不变。
47. Geomagic AutoSurface T6.4 Sewing / ShapeFix / SameParameter 最小可用版已完成：`PatchReplacementCommand` 现在使用 `BRepTools_ReShape` 对 candidate source faces 执行真实替换尝试，第一个 source face 替换为 `BoundaryConstrainedPatchBuilder` 生成的 replacementShape，其余 source faces 移除；replacementShape 可为 one-face 或 multi-face compound/shell fragment，`patchFaceCount > 1` 和 internal patch seams 不作为 unsupported。临时 after shape 会执行 `BRepLib::SameParameter`、`ShapeFix_Wire`、`ShapeFix_Face` 和 `BRepBuilderAPI_Sewing` 尝试，并把 repair 前后 face/edge/shell/solid、free edge、multiple edge 统计写入 `PatchReplacementReport`；若 sewing 结果为空或丢失 solid 拓扑，会记录 warning 并保留 pre-sewing shape。`StrictTopologyGate` 仍是最终提交门，Gate 成功才提交 afterDocument，Gate 失败 rollback 且主 ShapeDocument 不变；redo 只复用缓存 afterDocument，不重新运行 repair、不运行 Geomagic、不裁剪 STL、不读取固定 patch 路径。本阶段仍未接入 GUI，未绕过用户确认，也不信任 STL crop boundary 或 Geomagic patch outer boundary 作为最终 CAD boundary。
48. Geomagic AutoSurface T6.5 AppController / GUI 真实 Apply 接入已完成：新增 `AppController::applyCurrentPatchToCurrentCandidate`，GUI “应用当前 Patch 到候选区域”会获取当前 candidate 并通过 `CommandHistory` 执行 `PatchReplacementCommand`。Apply 前会验证 document、preview ready、preview/candidate 一致性、FeatureBoundedRefit 类型、candidate 状态和当前 document 上重新分析得到的 single closed outer boundary；成功后提交 afterDocument 到主 `ShapeDocument`、清除 patch overlay / cached artifact / imported patch / preview report、状态置为 `Applied`，失败后主 `ShapeDocument` 不变且保留 overlay / preview state 便于复盘，状态置为 `ApplyFailed`。GUI Apply 路径启用 `StrictTopologyGate` 严格水密选项：before 为 solid 时 after/STEP roundtrip solid count 必须保持，after 与 roundtrip 都必须 BRepCheck 通过、free edges=0、multiple edges=0，STEP export/readback 必须成功。`PatchReplacementCommand` 在 one-face path 中会用 imported patch face 的 surface 加原 STP candidate boundary wire 重建 trimmed face；multi-face replacement fragment 仍为主路径，不因 `patchFaceCount > 1` 或 internal seams 返回 unsupported，最终是否提交由 repair + StrictTopologyGate 决定。undo/redo 已接入，redo 只复用缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新导入 patch、不重新运行 repair。
49. Geomagic AutoSurface T6.5.1 Apply failure diagnostics 已完成：`PatchReplacementReport` 现在记录 `StrictTopologyGateReport` 的 before / after / STEP roundtrip face/edge/shell/solid、free edge、multiple edge、BRepCheck、STEP export、STEP roundtrip 和 watertight 选项；`PatchReplacementCommand` 在 Gate 成功或失败时都会把 gate failure reason、message、warning 和 stats 写入 report；GUI Patch Apply report 会显示 repair 前后 face/edge/shell/solid、repair free/multiple edge、Gate before/after/roundtrip stats 和 solid/watertight 判断。失败后仍保持主 `ShapeDocument` 不变且保留 overlay / preview state；本阶段未放宽 `StrictTopologyGate`，未重新运行 Geomagic，未重新裁剪 STL，未写死真实样例路径；可选 rejected after debug STEP artifact 未实现。
50. Geomagic AutoSurface T6.6 Industrial Adaptive Sewing 路线已根据 `scripts/industrial_sew.py` 确定：将三阶段策略迁移到 C++ repair pipeline，包括 `ShapeFix_Shape` / face orientation 修复、`ShapeUpgrade_UnifySameDomain`、多 tolerance `BRepBuilderAPI_Sewing` loop、`ShapeFix_Shell`、`BRepBuilderAPI_MakeSolid`、`ShapeFix_Solid`、collapsed guard 和 best-result selection。该路线不放宽 StrictTopologyGate，不绕过 rollback，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary，multi-face patch 仍是主路径，redo 仍只复用缓存 afterDocument。
51. Geomagic AutoSurface T6.6 Industrial Adaptive Sewing 已完成最小 C++ 集成：新增 `PatchReplacementRepair` 模块并由 `PatchReplacementCommand` 调用，repair 顺序覆盖 `ShapeFix_Shape`、`SameParameter`、`ShapeFix_Wire`、`ShapeFix_Face`、`ShapeUpgrade_UnifySameDomain`、adaptive `BRepBuilderAPI_Sewing` tolerance loop、`ShapeFix_Shell` / `BRepBuilderAPI_MakeSolid` shell-to-solid、`ShapeFix_Solid` 和 final unify。`PatchReplacementReport` 会记录 selected sewing tolerance、sewing attempt count、best sewing face/edge/shell/solid、best free/multiple edge、best BRepCheck、collapsed 状态、shapeFixShape/unify/shellToSolid/adaptiveSewing 是否执行；GUI Apply report 已展示这些字段。结果选择保持严格：preferred tolerance=0.007 的 valid non-collapsed solid 优先，否则选择 valid solid、未塌缩、free edge 最少的结果；没有 valid non-collapsed solid 时只保留 best attempt 诊断，不把塌缩或丢 solid 的 sewing result 当成功提交，最终仍由 `StrictTopologyGate` 决定提交或 rollback。redo 仍只复用缓存 afterDocument，不重新运行 repair / adaptive sewing / Geomagic / STL crop / patch import。
52. Geomagic AutoSurface T6.6.1 Crop Boundary Diagnostics 已完成第一版：新增 `CropBoundaryDiagnostics` / `CropBoundaryDiagnosticsReport`，基于 `RegionBoundaryAnalysis` 与 `BoundaryWireBuilder` 对原 STP candidate outer boundary loop 按 edge 采样，记录 edge id、sample count、3D point、edge length 和 single closed outer loop 状态；将原 boundary sample 分别与 local STL crop mesh、imported patch outerEdges 做最近距离统计，输出 min / max / average distance、missing point count、连续超限 segment、suspected gap edge ids、message / warning。GUI patch preview / import 成功后会自动显示 T6.6.1 overlay：黄色原 STP boundary loop、青色 imported patch outer boundary、红色 local STL crop coverage issue、洋红 patch boundary mismatch；Patch preview report 同步显示 T6.6.1 数值字段。本阶段不修改 `ShapeDocument`，不调用 Geomagic，不重新裁剪 STL，不改变 `PatchReplacementCommand` / `StrictTopologyGate` / redo 语义，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary。
53. Geomagic AutoSurface T6.6.2 Process Status Panel 已完成第一版：新增 `ProcessStatusSnapshot` / `ProcessStage` 和 GUI 底部“进程”页，显示当前阶段、candidate、local STL / patch STEP / IGS / fit_region log 路径、selected sewing tolerance、sewing attempt count、best sewing free/multiple edge、best face/edge/shell/solid、repair/adaptive sewing 状态、StrictTopologyGate evaluated/passed/failure、message 和 warning。`AppController` 会在 patch import、PreviewReady、Apply early failure、replacement build、PatchReplacementCommand 成功/失败、undo/redo 后同步快照；`MainWindow` 会在 crop / Geomagic preview pipeline / import / Apply / clear overlay / undo / redo 后刷新面板。ApplyFailed 后状态保留最后的 repair / adaptive sewing / Gate 参数；undo/redo 显示 CachedUndo / CachedRedo，不伪装成重新运行 Geomagic / crop / import / repair。第一版是阶段级状态；adaptive sewing 每个 tolerance attempt 的实时逐步刷新留给后续 job/progress callback。
54. Geomagic AutoSurface T6.6.3 STL Crop Boundary-Band Diagnostics 已完成第一版：`CropBoundaryDiagnostics` 现在在原 boundary sample 外增加 candidate 内侧 boundary-band sample，并统计 band sample 到 local STL crop mesh 的 min / max / average distance、missing point count、missing edge ids 和 suspected crop hole edge ids；同时对已打开的 source STL 做只读 triangle rejection audit，复现当前 `StlRegionExtractor` 的 bbox intersects + centroid-inside 判据，并记录 centroid outside 但 vertex / edge midpoint / near-boundary-band 命中的 conservative keep candidate、rejected near-boundary triangle count 和有限数量 overlay triangle。GUI Patch preview report 已显示 boundary-band 与 triangle audit 数值，overlay 新增橙色 boundary-band missing area 和紫色 near-boundary rejected triangles；Process Status Panel 同步显示 crop band/audit 摘要。裁剪 bbox 线框仍可手动打开，但默认隐藏，避免遮挡 STL、原 STP loop 和诊断 segment。本阶段不改变正式 crop 输出，不调用 Geomagic，不重新生成 patch，不修改 `ShapeDocument`，不放宽 `StrictTopologyGate`，不改变 redo 语义。
55. Geomagic AutoSurface T6.6.4 STL Region Extractor 保守裁剪修复已完成：`StlRegionExtractorOptions` 新增 vertex-inside、edge-midpoint-inside、boundary-band inclusion 与 leak guard 参数；在 `ConservativeBoundaryBand` 模式下，crop 对 triangle bbox 与 expanded candidate bbox 相交的三角片，除 centroid-inside 基线外，会保守保留 vertex inside、edge midpoint inside 或靠近原 STP candidate boundary band 的三角片。`StlCropReport` 记录 centroid / vertex / edge-midpoint / boundary-band keep count、conservative keep total、outside bbox / outside candidate reject count 和 warning；GUI “STL 裁剪完成”报告同步显示这些统计。实现保留 output bbox guard 与 output/centroid triangle ratio guard，避免无约束扩大 local STL。本阶段不调用 Geomagic，不修改 `ShapeDocument`，不绕过 `StrictTopologyGate`，不改变 redo 语义，不把 STL crop boundary 当最终 CAD boundary。
56. Geomagic AutoSurface T6.6.4.1 STL Crop Mode Switch 已完成：`StlRegionExtractorOptions::mode` 默认回到 `CentroidOnly`，等价旧 crop；T6.6.4 的 vertex / midpoint / boundary-band inclusion 保留在 `ConservativeBoundaryBand` 模式。GUI `STL -> 启用保守边界裁剪` 默认关闭，并同时控制“裁剪当前候选 STL”和“生成并预览当前 Patch”；裁剪报告和自动 Patch preview report 显示 crop mode。手动验证显示 conservative-boundary-band 能补齐 local STL，但可能让 AutoSurface 生成过多 patch faces 并扩大 patch boundary mismatch，因此当前生产默认不启用保守裁剪。本阶段不修改 `ShapeDocument`，不改变 Apply / `StrictTopologyGate` / redo 语义。
57. Geomagic AutoSurface T6.6.5 Regenerate Patch + Apply Verification 已完成手动路线验证：legacy centroid-only crop 仍能得到较简单的 AutoSurface patch，但 patch outer boundary 不能可靠闭合原 CAD candidate boundary；conservative-boundary-band crop 可改善 local STL 覆盖，但会把边缘褶皱 / 邻接特征带入 AutoSurface，导致 patch face count 和 patch boundary mismatch 恶化。Apply 失败形态仍集中在 replacement seam free edges 与 BRepCheck failure，说明问题不在 sewing tolerance 或 StrictTopologyGate 过严，而在“direct AutoSurface patch outer-boundary replacement”路线本身不成立。
58. Geomagic AutoSurface T6.7 Boundary-Constrained Surface Re-trim 第一版已完成：新增 `BoundaryConstrainedSurfaceRetrim`，`BoundaryConstrainedPatchBuilder` 默认从 imported Geomagic patch 选择可用 surface，并使用原 STP candidate outer boundary wire 构造 trimmed replacement face；Geomagic patch outer boundary 不再作为默认 replacement boundary，legacy direct patch fragment 只保留为显式测试/兼容路径。单一 surface 无法覆盖原 boundary 时先输出最佳单 surface 投影统计与 all-surface 最近投影 coverage，成功构造后的 replacement 仍必须经过 `PatchReplacementRepair` 与 `StrictTopologyGate`，redo 仍不得重跑 Geomagic / crop / import / repair。
59. Geomagic AutoSurface T6.6.4.2 STL Boundary Loop Repair Connectivity Guard 已完成：boundary-loop repair 只允许加入与当前已保留 crop mesh 顶点近似连通的三角片；仅满足边界距离但不连通的 repair candidate 会计入 `boundary_loop_orphan_repair_candidate_count` 并被拒绝，GUI “STL 裁剪完成”报告同步显示 orphan repair candidates。`missing after=0` 现在只代表边界采样距离满足阈值，不再被视为充分成功条件；如果缺口只能靠漂浮碎片补齐，则正确结果是拒绝该碎片并保留 missing/警告。
60. Geomagic AutoSurface stale output cleanup 已完成：`GeomagicAutoSurfaceBackend` 和 `scripts/geomagic_wrap/autosurface_pipeline.py` 在新运行前删除旧 STEP / IGS 输出，IGES→STEP 转换每次 WriteFile 前也会删除旧 STP，避免 AutoSurface 失败或未覆盖时把上一次结果误判为新 patch。backend 现在通过 `FIT_REGION_*` 环境变量传递 log、mesh repair、remesh、smooth/relax 与 AutoSurface 参数。
61. Geomagic AutoSurface Remesh opt-in 修正已完成：真实日志显示 Remesh 自动 target edge length 可低到不合理量级，并导致 AutoSurface `Initialization of surface data failed`。`GeomagicAutoSurfaceConfig::skipRemesh` 默认改回 `true`，脚本默认 `FIT_REGION_SKIP_REMESH=1`，示例配置同步为 `skip_remesh=true`；GUI Patch 菜单新增“启用 Geomagic Remesh”显式实验开关。脚本执行 Remesh 失败时只写 warning 并继续使用原 mesh；如果 Remesh 成功但随后 AutoSurface 全部失败，脚本会 retry pre-remesh mesh，避免实验 Remesh 阻断可生成的 no-remesh 路径。
62. Geomagic AutoSurface T6.7.4 Strict Multi-surface Boundary-Constrained Shell 已完成增强版：新增 `BoundaryConstrainedMultiSurfaceShellBuilder`，在单 surface retrim 失败但 all-surface coverage 无 uncovered sample 时，将原 STP candidate boundary edges 整段优先分配给最佳 Geomagic surface；若某条 edge 没有单一 surface 能覆盖整条 edge，但每个采样点均有 surface 覆盖，则只对该失败 edge 按 surface ownership 切分参数区间。T6.7.4 用原 STP edge 参数区间构造最终外边界段，并保留 imported patch 内部 seam edges 连接闭合 wire；若同一 patch face 形成多个 closed wire，现在全部构造成 replacement face，不再丢弃额外闭合环。`BoundaryConstrainedPatchBuilder` 默认按 single-surface retrim → multi-surface shell → legacy fallback-disabled 的顺序执行；T6.7.4 失败时返回 BuildFailed 并报告 boundary sample / assigned segment / split edge / built face / closed wire / open wire / failed patch face / failed edge diagnostics，不回退到 patch outer boundary。成功结果仍进入 `PatchReplacementRepair` 与 `StrictTopologyGate`；redo 语义不变。
63. T6.7.4 真实 Apply 诊断工具与阶段性结论已补齐：新增参数化命令行工具 `patch_apply_probe`，可加载 source STEP、按 candidate id 选择 FeatureBoundedRefit candidate、导入 patch STEP/IGES，并执行与 GUI Apply 对齐的 replacement / repair / StrictTopologyGate 路径；工具不运行 Geomagic、不裁剪 STL、不写死真实样例路径，并输出阶段进度用于定位真实大样例耗时。当前真实 GUI / Apply report 结论显示 multi-surface shell 已不再停在单 surface coverage 阶段，后续主要失败转为 split boundary 与邻接旧拓扑/桥接闭合收口；不应回退到 Geomagic patch outer boundary。
64. STP-sampled synthetic fitting STL input mode 已完成：新增 `StpSampledFittingMeshBuilder` / `StpSampledFittingOptions` / `StpSampledFittingReport`，可从当前 STP candidate faces、outer boundary 和 boundary band 采样生成 synthetic fitting STL，再交给 Geomagic AutoSurface。`GeomagicFittingInputMode::StpSampledCandidateSurface` 已接入 `AppController::generatePatchPreviewForCandidateData`，生成的 STL 仍写入 `data/crop_stl/<step-stem>/candidate_<id>.stl`，后续 patch artifact 定位、Geomagic 输出、PatchImportService 和 Apply 流程保持一致。该路径不需要源 STL，适合作为当前默认生产输入。
65. GUI 默认 fitting input mode 已切到 STP sampled：`MainWindow::fittingInputMode_` 默认值和对应 QAction 都是 `StpSampledCandidateSurface`；“生成并预览当前 Patch”在该模式下不会要求先打开原始 STL。Legacy STL crop 与 Conservative Boundary Band STL crop 仍保留在 Patch 菜单用于对照验证，报告会输出 `fitting input mode` 字符串，避免把不同输入路线混在一起。
66. STL Global Cut Chain crop 已完成第一版并接入 GUI：新增 `StlCropMode::GlobalCutChain` 与 `StlCutChainCutter`，从原 STP candidate ordered boundary edges 采样 boundary loop，在源 STL 上投影、跟踪 cut chain、按约束重三角化并 flood-fill 选出 patch mesh；GUI `STL -> 启用全局切链裁剪 (Global Cut Chain)` 可显式开启，单独裁剪当前候选 STL 时会走该路线。它是 STL 全局切链裁剪器，不是当前默认 Geomagic fitting input mode。
67. Global Cut Chain boundary snap 已接入：`StlCutChainOptions` 默认 `snapBoundaryToRed=true`、`snapMaxDist=0.2`，切链后的 patch outer boundary 可向原 STP red boundary loop 回贴，降低投影到 STL 后的 green boundary 漂移。该 snap 只改善 local STL / fitting STL 输入质量，不改变最终 CAD boundary；Apply 仍必须使用原 STP candidate outer boundary wire，并通过 PatchReplacementRepair 与 StrictTopologyGate。
68. GUI 后台任务与状态显示优化已按 `1f0c3d7` 可缝合基准完成移植：`openStepFile()`、`previewMergeCandidates()` 和 Patch Apply 的计算 / 读取进入 worker，viewer / model tree / report / Process Status 更新回 GUI 线程；长任务期间禁用会修改 document / controller 的入口；Process Status 与 Patch Apply report 展示 Gate before / after / STEP roundtrip 的 BRepCheck、free edge、multiple edge 和 face / edge / shell / solid 计数。`d1c00e4` Improve boundary constrained Geomagic patch flow 与 Sharp Contours 实验不进入本轮主线。
69. 文档入口与下一阶段方向已同步到 `a57695d` 主线：仓库根目录恢复 `AGENTS.md`，保证新对话会先读全局 Workspace、项目记忆和仓库文档；当前后续研究问题明确为 Geomagic 在拐角 / 特征交汇处圆角化导致商业 CAD 出现缝隙。下一轮应优先评估 corner-aware / curvature-aware 采样、STP boundary guard-band 外扩采样、feature/corner anchors、原 STP boundary 重裁剪和 Creo-like 高密度偏差门控，而不是继续 Sharp Contours 或重新合入 `d1c00e4`。
70. corner preservation 最终方案已确定为“双层闭环”：前端通过 corner-aware / curvature-aware sampling、feature/corner anchors 和 STP boundary 外 guard-band 采样改善 Geomagic fitting input；后端在 `StrictTopologyGate` 之外新增 commercial-CAD-like 高密度几何门控，测量 boundary deviation、corner anchor drift、feature edge drift、sharpness preservation、surface COPS-like deviation 和 STEP roundtrip 后 geometry drift。第一轮实验分支为 `experiment/corner-preservation-ab`，A/B 矩阵为 baseline、corner 加密、guard-band、corner+guard-band；成功不能只看 GUI 或 OCCT BRepCheck。
71. A0 baseline 自动化入口已落地：新增 `corner_baseline_probe` CLI，流程为 source STEP + candidate id → STP-sampled fitting STL → Geomagic AutoSurface 或 `--patch` 复用已有 patch → Patch Apply + `StrictTopologyGate` → `CommercialCadLikeQualityGate` → JSON 报告。新增 `CommercialCadQualityGate` 独立模块，第一版测量原 STP boundary、corner anchor 和 feature boundary samples 到 imported patch 的 max/mean/RMS/p95 drift；它不替代 `StrictTopologyGate`，只补足 OCCT 拓扑 gate 看不到的 commercial-CAD-like 几何偏差。
72. `corner_baseline_probe` 的 Geomagic staging 已对齐 GUI 路径：输入 STL / 输出 STP / fit log 写入仓库 `data\crop_stl`、`data\crop_stp`、`data\crop_igs`，报告写入 `data\baseline_runs`。真实样例 `03_配件_Clay.stp` candidate 179 的 A0 脚本已复现 GUI 后端：Geomagic STP 生成成功，Patch Apply 与 `StrictTopologyGate` 通过，但 `CommercialCadLikeQualityGate` 失败，boundary/feature max drift 0.123574、p95 0.044766，corner p95 0.086309。
```

其中，`MergePatchCommand` 的撤销语义当前定义为：

```text
撤销合并曲面片时，取消合并后产生或保留的锁边状态。
```

这是一个明确设计决策，不再作为待修复问题记录。

---

## 3. 已实现功能清单

### 3.1 GUI 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| 主窗口布局 | 已完成 | 包含模型树、3D Viewer、参数面板、日志/检查/验证/报告区域 |
| STEP 模型显示 | 已完成 | 基于 OCCT Viewer |
| 鼠标中键旋转视角 | 已完成 | 支持基础视角浏览 |
| 鼠标滚轮缩放 | 已完成 | 支持放大 / 缩小 |
| face 选择 | 已完成 | 支持面选择模式 |
| edge 选择 | 已完成 | 支持边选择模式 |
| Shift 多选 face/edge | 已完成 | 支持多选与 toggle |
| Ctrl 点击移除选择 | 已完成 | 支持从选择集中移除 |
| 右键菜单 | 部分完成 | edge 模式已接入锁边/解锁；face/candidate 菜单多为预留 |
| 特征线显示 | 已完成基础版 | 显示自动检测出的特征边 |
| 锁定边显示 | 已完成 | 锁定边以高亮方式显示 |
| 合并候选预览 | 已完成基础版 | 后端 MergePlanner 已接入，可高亮候选区域 |
| 候选区域点击选择 | 已完成基础版 | 选择候选区域模式下点击面片可选中所属候选 |
| 候选区域接受/拒绝/隐藏/恢复 | 已完成基础版 | 管理运行时候选状态，不修改 B-rep |
| Crop Boundary Diagnostics overlay | 已完成基础版 | Patch preview/import 成功后显示原 STP boundary、patch outer boundary、STL coverage issue、boundary-band missing area、patch mismatch 和 near-boundary rejected triangles 诊断线；crop bbox 默认隐藏 |
| undo/redo 按钮 | 已完成 | 已接入 Ctrl+Z / Ctrl+Y |
| GUI 手动验证 | 已完成 | 当前主流程手动验证通过 |

### 3.2 AppController 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| 打开 STEP/STP | 已完成 | `openStepFile` |
| 导出 STEP | 已完成 | `exportStepFile` |
| 导出后二次读取校验 | 已完成 | `verifyStepFileReadable` |
| 特征边检测 | 已完成基础版 | `detectFeatureEdges` |
| same-domain 合并 | 已完成 | `unifySameDomain` |
| 合法性检查 | 已完成基础版 | `validateShape` |
| 锁边 / 解锁边 | 已完成 | `lockEdges` / `unlockEdges` |
| undo / redo | 已完成 | `undo` / `redo` |
| undo/redo 状态查询 | 已完成 | `canUndo` / `canRedo` |
| Crop Boundary Diagnostics | 已完成基础版 | `diagnoseCropBoundaryForCurrentPatch` / `diagnoseCropBoundaryData` 输出 T6.6.1 + T6.6.3 report，包含 boundary-band coverage 与 source triangle audit，不修改文档 |

### 3.3 Command 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `Command` 基类 | 已完成 | 支持 `execute`、`undoable`、`undo`、`redo` |
| `CommandContext` | 已完成基础版 | 保存当前文档、检测结果、验证报告、锁边状态等 |
| `CommandHistory` | 已完成 | 维护 undoStack / redoStack / executedCommands |
| `LoadStepCommand` | 已完成 | 加载 STEP；不可撤销 |
| `DetectFeatureCommand` | 已完成基础版 | 检测特征边；不可撤销 |
| `ValidateShapeCommand` | 已完成基础版 | 生成验证报告；不可撤销 |
| `ExportStepCommand` | 已完成 | 导出 STEP；不可撤销 |
| `MergePatchCommand` | 已完成基础版 | 支持 same-domain 合并和 undo/redo |
| `LockEdgeCommand` | 已完成 | 支持多边锁定和 undo/redo |
| `UnlockEdgeCommand` | 已完成 | 支持多边解锁、document 校验和 undo/redo |
| `LockedEdgeRef` | 已完成 | 保存锁边几何签名，用于合并后边 ID 重映射 |

### 3.4 IO 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| STEP 读取 | 已完成 | 支持读取 `.step` / `.stp` |
| STEP 写出 | 已完成 | 支持导出 STEP |
| STEP 二次读取验证 | 已完成 | 导出后可重新读取验证 |
| 项目文件保存 | 未完成 | `ProjectSerializer` 仍待实现 |
| 项目文件恢复 | 未完成 | 需要保存锁边、参数、操作日志等 |

### 3.5 B-rep 拓扑模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `ShapeDocument` | 已完成基础版 | 保存 shape、source path、stats、topology |
| `FaceIndex` | 已完成基础版 | face 编号与查询 |
| `EdgeIndex` | 已完成基础版 | edge 编号与查询 |
| `TopologyGraph` | 已完成基础版 | 支持 face/edge 查询、邻接关系、面边关系 |
| face/edge GUI 命中查询 | 已完成 | 支持从 TopoDS_Shape 反查 ID |
| 面属性统计 | 待增强 | 面积、中心点、平均法向、曲面类型等仍可扩展 |
| 边属性统计 | 待增强 | 长度、曲线类型、曲率等仍可扩展 |

### 3.6 特征线模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| sharp edge 检测 | 已完成基础版 | 基于二面角阈值 |
| free edge 检测 | 已完成基础版 | 用于识别开口边 |
| multiple edge 检测 | 已完成基础版 | 用于识别异常拓扑边 |
| min edge length 过滤 | 已完成基础版 | 可过滤过短边 |
| 用户锁边 | 已完成 | 通过 `CommandContext.lockedEdges` 维护 |
| 用户锁边参与合并保护 | 已完成 | 合并时加入 protectedEdges，并已有测试覆盖 |
| 曲率特征线 | 未完成 | ridge / valley / weak feature 仍待实现 |
| 圆角起止线识别 | 未完成 | 后续由 BoundaryClassifier 增强 |
| 曲面类型变化识别 | 未完成 | 后续可结合 B-rep surface type |

### 3.7 合并模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `SameDomainUnifier` | 已完成基础版 | 当前主合并能力 |
| 自动特征边保护 | 已完成 | sharp/free/multiple edges 进入保护边 |
| 用户锁边保护 | 已完成 | locked edges 进入 protectedEdges |
| 用户锁边保护测试 | 已完成 | 已补充 protectedEdges 测试 |
| 合并前后统计 | 已完成基础版 | before/after stats |
| 合并撤销/重做 | 已完成 | 通过 `MergePatchCommand` 快照实现 |
| 合并撤销时清空锁边 | 已确认 | 当前定义为正确交互语义 |
| 锁边重映射 | 已完成基础版 | 通过 `LockedEdgeRef` 几何签名尝试映射 |
| `MergeCandidate` | 已完成基础版 | 支持候选类型、风险、face/edge 集合、统计指标和运行时状态 |
| `MergePlanner` | 已完成基础版 | 基于特征边和锁边生成 PlaneLike 候选区域 |
| `MergeRegionGrower` | 已完成基础版 | 支持平面近似区域生长，protectedEdges 可阻断扩张 |
| `MergeCandidate` 预览 | 已完成基础版 | GUI 可预览 Top N、全部非隐藏候选和指定候选 |
| 候选状态管理 | 已完成基础版 | Pending / Accepted / Rejected / Hidden，仅运行时保存 |
| Face / Candidate Inspect | 已完成基础版 | 点击 face 可查看 surface type、候选归属、candidate type/status/risk/fit_error 等信息 |
| Analytic primitive candidate detection | 已完成增强 B | 支持 PlaneLike、CylinderLike 原生与 B-spline/Bezier/SurfaceOfRevolution 近似检测、SphereLike、ConeLike 原生与 B-spline/Bezier/SurfaceOfRevolution 近似检测；TorusLike/Freeform 类型预留 |
| Multi-type Candidate Preview | 已完成基础版 | 支持多类型统计、按类型筛选、Viewer 类型配色、模型树同步和点击查看 |
| RegionMergeResult / RegionMergeOptions | 已完成基础版 | 统一解析图元区域合并返回值、失败原因和基础选项 |
| Plane/Cylinder/Cone/Sphere/Torus RegionMerger stub | 已完成基础版 | 仅返回 NotImplemented / UnsupportedCandidateType，不修改 B-rep |
| `PlaneRegionMerger` | 已完成基础可用版 | 支持 PlaneLike 候选区域真实替换为 planar trimmed face，保留 solid/shell 容器 |
| 平面候选批量合并 | 已完成基础版 | 支持合并当前候选、合并所有已接受平面候选、一键合并全部可合并平面候选 |
| 平面合并边界简化 | 已完成基础版 | 对候选外边界执行受限 edge-only same-domain 简化，减少共线边界分段 |
| `PlaneRegionMergeCommand` | 已完成基础版 | 平面候选合并通过 Command 层执行，支持 undo/redo |
| `PlaneRegionBatchMergeCommand` | 已完成基础版 | 批量平面候选合并通过 Command 层执行，支持 undo/redo |
| Stage 3-S Shared Primitive Fields | 已完成 | RegionMergeResult 已新增 9 个通用 primitive 参数字段，供后续 Sphere / Cylinder / Cone / Torus 复用 |
| `SphereRegionMerger` | 已完成稳定版调整 | 支持 SphereLike 候选区域通过 OCCT same-domain unifier 消除内部边，不再手工构造 spherical trimmed face；批量路径保护候选外边界和其他所有边，避免缺面和飞线 |
| `SphereRegionMergeCommand` | 已完成基础版 | 球面候选合并通过 Command 层执行，支持 undo/redo |
| `SphereRegionBatchMergeCommand` | 已完成基础版 | 批量球面候选合并通过 Command 层执行，支持 undo/redo |
| `CylinderRegionMerger` | 未完成 | 当前仍为 stub |
| `ConeRegionMerger` | 未完成 | 当前仍为 stub，后置 |
| `TorusRegionMerger` | 未完成 | 当前仍为 stub，可选 |
| `SurfaceRefitter` | 未完成 | 当前为后续研究增强方向 |

### 3.8 验证模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| shape 是否存在 | 已完成 |
| BRepCheck | 已完成基础版 |
| stats 统计 | 已完成 |
| free edge 数量 | 已完成 |
| multiple edge 数量 | 已完成 |
| 导出后二次读取验证 | 已完成 |
| max deviation | 未完成 |
| mean / RMS deviation | 未完成 |
| feature preserve rate | 未完成 |
| locked edge preserve rate | 未完成 |
| 实验报告生成 | 未完成 |

### 3.9 测试模块

| 测试内容 | 状态 | 说明 |
|---|---:|---|
| STEP IO 测试 | 已完成基础版 |
| TopologyGraph 测试 | 已完成基础版 |
| FeatureEdge 测试 | 已完成基础版 |
| SameDomainMerge 测试 | 已完成基础版 |
| Validation 测试 | 已完成基础版 |
| CommandHistory 测试 | 已完成 |
| LockEdgeCommand 测试 | 已完成 |
| UnlockEdgeCommand 测试 | 已完成 |
| UnlockEdgeCommand 无文档校验测试 | 已完成 |
| MergePatchCommand undo/redo 测试 | 已完成 |
| 用户锁边进入 protectedEdges 测试 | 已完成 |
| MergePlanner / MergeRegionGrower 测试 | 已完成基础版 | 覆盖候选生成、protectedEdges 阻断、min_region_faces 和预览不改模型 |
| MergeCandidate 状态测试 | 已完成基础版 | 覆盖 Pending 默认状态、状态切换、Hidden 过滤和 stats 不变 |
| RegionMerger stub 测试 | 已完成基础版 | 覆盖 NotImplemented、UnsupportedCandidateType、Rejected/Hidden 和 stats 不变 |
| PlaneRegionMerger 测试 | 已完成基础版 | 覆盖非法候选、边界无效、NURBS-backed 平面区域、solid 容器保留和边界分段简化 |
| PlaneRegionMergeCommand 测试 | 已完成基础版 | 覆盖失败不污染文档、成功后 undo/redo |
| PlaneRegionBatchMergeCommand 测试 | 已完成基础版 | 覆盖批量平面合并成功后的 undo/redo |
| Face / Candidate Inspect 测试 | 已完成基础版 | 覆盖 surface type、候选归属、NotInCandidate 和 stats 不变 |
| Analytic Candidate Detection 测试 | 已完成增强 B | 覆盖 CylinderLike 原生与 NURBS-backed 近似检测、SphereLike/ConeLike 基础检测、NURBS-backed ConeLike 近似检测、近似圆柱不误判为 ConeLike、protected edge 阻断和 ID 唯一 |
| Candidate Type Statistics 测试 | 已完成基础版 | 覆盖多类型统计、Hidden 过滤、按类型筛选和 PlaneLike 合并过滤 |
| SphereLike 一键合并过滤测试 | 已完成基础版 | 覆盖一键球面候选过滤会跳过 High risk 和单 face 候选；球面合并不再依赖 boundary wire 闭合，而是基于候选内部边做同域合并 |
| Geomagic backend mock 测试 | 已完成 | 覆盖 mock success/failure/timeout、缺输入、缺脚本、缺输出、result JSON 缺失但 STEP 存在的真实脚本兜底 |
| Geomagic pipeline script 静态契约测试 | 已完成 | 覆盖 FIT_REGION 输入输出、RepairMesh、RemoveNonManifoldVertices、FillSmallHoles、Mechanical 默认、无 result JSON 输出契约 |
| PatchImportService 测试 | 已完成 | 覆盖 STEP/STP/IGS/IGES 导入、bbox/face/edge/BRepCheck 统计、从 Geomagic result 导入、失败不修改 ShapeDocument |
| PatchImportService 真实文件测试 | 已完成 | 默认导入 data/crop_stp/data/crop_igs 中已有文件；`SPO_ENABLE_REAL_GEOMAGIC_TESTS=1` 时跑通真实 wrapCore.exe 链路 |
| Patch Apply / Controller Apply 测试 | 已完成 | 覆盖 NotGenerated、ApplyBlocked、PreviewHighRisk、PreviewReady、multi-face 不阻塞、clear 重置、无 preview Apply 失败、preview/candidate mismatch 阻断、真实 Apply 通过 CommandHistory 成功提交、失败保留 document 与 preview state、成功清 preview state、undo/redo、multi-face 进入 Apply path、open STEP / undo / redo / SameDomain 合并后的旧 preview 清理；T6.5.1 增加 PatchReplacementReport gate diagnostics、GUI report 诊断标签、STEP roundtrip stats 和 watertight stats 断言；T6.6 增加 selected sewing tolerance、adaptive sewing attempt count、best sewing stats、GUI report 标签和 redo 不重跑 repair 断言 |
| MultiFacePatchAnalyzer / PatchReplacement input validation 测试 | 已完成 | 覆盖 empty patch、one-face patch、multi-face box patch、compound patch、缺失输入、invalid boundary、multi-face accepted 和 highRisk rejected |
| StrictTopologyGate 测试 | 已完成 | 覆盖 identical valid box、missing after shape、free edge increase、multi-face replacement accepted / disallowed、face count non-reduction warning、STEP export + roundtrip、严格 watertight solid gate、roundtrip 后 BRepCheck / solid count / zero free edge / zero multiple edge 统计 |
| BoundaryConstrainedPatchBuilder 测试 | 已完成 | 覆盖 one-face fragment、multi-face box fragment、empty analysis、invalid boundary、synthetic patchFaceCount=12、internal seam retention 和 ShapeDocument 不变 |
| PatchReplacementRepair / T6.6 测试 | 已完成 | 覆盖 adaptive sewing 多 tolerance 尝试、preferred tolerance 优先、collapsed result 只作为诊断、closed shell 的 shell-to-solid path、Command report 字段、multi-face 不 unsupported、GateFailed rollback、redo 不重跑 repair；T6.5.1 的 rejected after debug artifact 仍未实现，若后续需要可补为可选诊断 artifact |
| T6.6.1 Crop Boundary Diagnostics 测试 | 已完成 | 覆盖原 STP boundary loop 采样、STL crop 最近距离覆盖检查、imported patch outer boundary 覆盖检查、suspected gap segment 报告、无 local STL 的 patch-only 诊断、GUI overlay 标签和禁止真实样例路径硬编码 |
| T6.6.2 Process Status Panel 测试 | 已完成 | 覆盖状态阶段字符串、PreviewReady、Apply success/failure 后参数保留、GateFailed 后 repair/Gate 诊断保留、undo/redo 显示 CachedUndo / CachedRedo 且不显示 Geomagic / crop / import / repair 重跑 |
| T6.6.3 STL Crop Boundary-Band Diagnostics 测试 | 已完成 | 覆盖 boundary point coverage 正常但 boundary-band coverage 报警、centroid-only 拒绝近边界三角片但 conservative criteria 命中、report/GUI 字段存在性和禁止真实样例路径硬编码 |
| T6.6.4 STL Region Extractor conservative crop / mode switch 测试 | 已完成 | 覆盖默认 centroid-only、显式 ConservativeBoundaryBand 下 centroid outside but vertex inside、edge midpoint inside、boundary-band inclusion、crop report keep/reject 字段和正式测试不调用 Geomagic |
| T6.6.5 GUI A/B 验证 | 已完成 | 验证 centroid-only 与 conservative-boundary-band 两条 crop mode 都不能让 Geomagic patch outer boundary 成为可靠 CAD replacement boundary，后续转向 T6.7 surface re-trim |
| T6.6.4.2 boundary-loop connectivity guard 测试 | 已完成 | 覆盖连通 boundary-loop 修补可加入、不连通漂浮修补候选会被拒绝并计入 orphan repair candidates |
| T6.7 Boundary-Constrained Surface Re-trim / T6.7.4 Multi-surface Shell 测试 | 已完成 | 覆盖 original-boundary surface re-trim、multi-face patch 默认不再直接信任 patch outer boundary、无覆盖 surface 时 BuildFailed、单 surface 与 all-surface coverage report 字段、strict multi-surface shell 成功、缺少内部 seam 时 BuildFailed、attempted / split edge / closed wire / Command report 字段和 Gate/rollback 语义；真实大样例继续以 GUI Apply report 作为手动验证主证据，`patch_apply_probe` 作为辅助定位工具 |
| Geomagic stale output / Remesh env 测试 | 已完成 | 覆盖 backend 删除 stale STEP/IGS、脚本删除旧输出、Remesh 默认跳过、显式 Remesh 环境变量传递、Remesh 失败 warning fallback 和 AutoSurface 失败后的 pre-remesh retry |
| STP-sampled fitting mesh 测试 | 已完成 | 覆盖 planar face / multi-face candidate 采样、boundary samples、boundary band 开关、STL roundtrip、bbox、非退化三角片、report 字段和 `GeomagicFittingInputMode` 字符串 |
| STL Global Cut Chain cutter 测试 | 已完成基础版 | 覆盖空 mesh / 短 loop 失败、拓扑构建、三角形面积、polyline 长度和点到线段距离；真实复杂 STL 切链仍以 GUI 手动验证为主 |
| PatchArtifactLocator 中文路径测试 | 已完成 | 覆盖中文 relative dir / 中文 stem 下 local STL 到同名 STEP、autosurface IGS sidecar 和 fit_region log 的定位 |
| AppController 打开新文档清历史测试 | 已完成 |
| GUI 自动化测试 | 未完成 | 当前主要依赖手动验证 |
| GUI 手动验证 | 已完成 | 当前主流程手动验证通过 |

---

## 4. 当前已完成的关键工程闭环

### 4.1 锁边保护闭环

```text
边选择模式
→ Shift 多选边
→ 右键锁定选中边
→ 锁边高亮显示
→ 执行 same-domain 合并
→ 用户锁定边进入 protectedEdges
→ 合并尽量不跨越锁定边
```

### 4.2 undo/redo 闭环

```text
执行可撤销命令
→ 命令进入 undoStack
→ Ctrl+Z 撤销
→ 命令进入 redoStack
→ Ctrl+Y 重做
```

当前可撤销命令：

```text
1. MergePatchCommand
2. LockEdgeCommand
3. UnlockEdgeCommand
4. PlaneRegionMergeCommand
5. PlaneRegionBatchMergeCommand
```

当前不可撤销命令：

```text
1. LoadStepCommand
2. DetectFeatureCommand
3. ValidateShapeCommand
4. ExportStepCommand
```

### 4.3 STEP 处理闭环

```text
打开 STEP
→ 显示模型
→ 检测特征边
→ 锁定关键边
→ 执行合并
→ 合法性检查
→ 导出 STEP
→ 二次读取校验
```

### 4.4 Geomagic patch 生成 / 导入闭环

```text
默认路线：
当前 STP candidate faces / boundary
→ STP Sampled Candidate Surface
→ data/crop_stl/<step文件stem>/<step文件stem>_candidate_<id>.stl
→ wrapCore.exe --script scripts/geomagic_wrap/autosurface_pipeline.py
→ RepairMesh / RemoveNonManifoldVertices / FillSmallHoles
→ AutoSurface geometry=Mechanical, autoMerge=true, numPatches=1
→ data/crop_stp/<step文件stem>/<step文件stem>_candidate_<id>.stp
→ PatchImportService 导入
→ step_stats 输出 faces=12, edges=50, BRepCheck valid=true

备用 / 诊断路线：
原始 STL
→ Legacy centroid-only crop / Conservative boundary-band crop（自动 Patch preview fitting input modes）
→ Global Cut Chain crop（独立 STL 裁剪路线，当前不是 GeomagicFittingInputMode）
→ data/crop_stl/<step文件stem>/<step文件stem>_candidate_<id>.stl
→ Legacy / Conservative 可进入同一 Geomagic / import / preview / Apply 后续流程
→ Global Cut Chain 输出可通过手动 / 脚本 Geomagic 路线继续验证
```

当前验证命令：

```powershell
.\scripts\verify_spo.ps1
.\scripts\verify_spo.ps1 -Gui
.\scripts\verify_spo.ps1 -RealGeomagic
.\scripts\verify_spo.ps1 -StepStats -StepStatsPath "<patch_step_path>"
.\scripts\run_geomagic_patch.ps1 -InputStl "<local_stl_path>"
```

---

## 5. 已确认设计决策

### 5.1 MergePatchCommand undo 清空锁边状态

当前语义：

```text
用户先锁边
→ 执行合并
→ 撤销合并
→ 模型回到合并前
→ 锁边状态清空
```

该语义已确认是当前正确设计。

理由：

```text
1. 合并前后拓扑 edge ID 可能变化。
2. 合并后的锁边状态不一定能安全映射回合并前模型。
3. 撤销合并时清空锁边，可以避免锁边引用错误拓扑。
4. 用户可以在回退后的模型上重新选择并锁定边。
```

### 5.2 UnlockEdgeCommand 必须要求已有文档

当前语义：

```text
没有已加载模型时，LockEdgeCommand 和 UnlockEdgeCommand 都应返回错误。
```

这保证锁边/解锁边行为一致。

### 5.3 用户锁边是 protectedEdges 的一部分

当前语义：

```text
protectedEdges = 自动检测特征边 + 用户锁定边
```

目的：

```text
1. 自动特征边保护棱边、free edge、multiple edge。
2. 用户锁边保护人工认为重要但算法未识别的边。
3. same-domain 合并不应跨越 protectedEdges。
```

---

## 6. 待办清单

## P0：稳定性收口

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 手动完整验证 GUI 主流程 | 已完成 | 打开、选择、多选、锁边、合并、撤销、重做、验证、导出 |
| 确认 MergePatchCommand undo 锁边语义 | 已完成 | 已确认撤销合并时清空锁边是正确语义 |
| 补 UnlockEdgeCommand document 校验 | 已完成 | 无模型时解锁返回错误 |
| 补充用户锁边进入 protectedEdges 的测试 | 已完成 | 已有测试验证锁边会贡献 protectedEdges |
| 复杂 STEP 样例回归测试 | 待做 | 选择 3-5 个潮玩件或碎片 STP 样例 |

## P1：合并候选规划

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现 MergeCandidate 数据结构 | 已完成基础版 | 能表达候选区域 face 集合、边界、风险说明和运行时状态 |
| 实现 MergePlanner 基础候选生成 | 已完成基础版 | 能从 TopologyGraph 生成 PlaneLike 候选区域 |
| 实现 MergeRegionGrower 基础区域生长 | 已完成基础版 | 能根据特征边/锁边阻断区域扩张 |
| GUI 显示候选区域 | 已完成基础版 | 可高亮 Top N、全部非隐藏候选和指定候选 |
| 用户接受/拒绝候选区域 | 已完成基础版 | 支持选择、接受、拒绝、隐藏、恢复候选；本阶段不应用到 B-rep |

## P2：项目保存与报告

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现 ProjectSerializer | 待做 | 保存 `.spo.json` |
| 保存用户锁边状态 | 待做 | 重开项目后锁边可恢复 |
| 保存参数配置 | 待做 | 重开项目后参数可恢复 |
| 保存操作日志 | 待做 | 项目文件中记录核心操作 |
| 实现 ReportGenerator 基础报告 | 待做 | 输出 face/edge 变化、free/multiple edge、BRepCheck |
| 实现导出批量实验报告 | 待做 | 可用于论文/组会实验记录 |

## P3：高级特征线与重拟合

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| CurvatureEstimator 实用化 | 待做 | 能估计局部曲率变化 |
| BoundaryClassifier 实用化 | 待做 | 能识别圆角起止线、凸凹分界 |
| ridge / valley 检测 | 待做 | 能在潮玩件凸起/凹陷处生成候选特征线 |
| SurfaceRefitter 局部重拟合 | 待做 | 能对局部碎片区域拟合新 B-spline |
| 局部 patch layout 重构 | 待做 | 能减少 tiny/slender patch |
| 学习辅助候选区域推荐 | 待做 | 作为研究增强方向，不进入 MVP |

---

## 7. 当前建议验证命令

Windows 本地构建与测试：

```powershell
cd D:\pyProject\step-patch-optimizer
$env:Path='C:\Program Files\CMake\bin;C:\Users\27836\vcpkg;' + $env:Path

cmake --build --preset windows-msvc-debug --target step-patch-optimizer
ctest --preset windows-msvc-debug --output-on-failure --timeout 30
```

手动 GUI 验证流程：

```text
1. 打开一个 STEP/STP。
2. 切换到边选择模式。
3. Shift + 左键多选几条边。
4. 右键锁定选中边，确认锁边高亮。
5. Ctrl+Z，确认锁边撤销。
6. Ctrl+Y，确认锁边恢复。
7. 执行特征边检测。
8. 执行 same-domain 合并。
9. Ctrl+Z，确认模型回退并清空合并相关锁边状态。
10. Ctrl+Y，确认模型恢复。
11. 点击“预览合并”，选择或接受 PlaneLike 候选区域。
12. 通过“平面合并”下拉菜单执行当前候选、全部已接受候选或全部可合并候选。
13. 确认平面合并后 face/edge 数下降，solid 数不丢失，候选外边界共线分段尽量被简化。
14. 执行合法性检查。
15. 导出 STEP。
16. 确认导出后二次读取校验通过。
```

---

## 8. 近期推荐开发顺序（2026-06-11 修订）

**当前默认路线：STP-sampled fitting input**

```text
1. 继续把 STP Sampled Candidate Surface 作为默认 Geomagic fitting input mode。
   - 不要求用户先加载原始 STL。
   - 使用当前 STP candidate faces / boundary 生成 fitting STL。
   - 重点验证速度、AutoSurface patch face count、Apply report 和失败形态。

2. 保留 STL 全局切链裁剪器作为备用 / 诊断路线。
   - Global Cut Chain 用于需要从真实源 STL 取局部三角片的场景。
   - 它可以改善 local STL 边界跟随，但不能改变最终 CAD boundary。
   - 不应在没有更多真实样例证据前替代 STP-sampled 默认模式。

3. Apply 收口仍围绕 T6.7.4 后的问题：
   - split boundary 与邻接旧拓扑 / bridge closure。
   - PatchReplacementRepair 后 free edge / multiple edge 归零。
   - BRepCheck、solid/watertight 和 STEP roundtrip 全部通过。

4. 文档、报告和 GUI 文案必须明确区分：
   - fitting input mode：STP sampled / legacy STL crop / conservative STL crop。
   - STL crop mode：centroid-only / conservative boundary-band / global cut chain。
   - final CAD boundary：只能来自原 STP candidate outer boundary wire。

5. 当前下一步执行 corner preservation A/B 实验：
   - A0：用 `corner_baseline_probe` 跑当前 STP sampled baseline，并保存 JSON 报告。
   - B1：corner / feature edge 加密采样。
   - B2：原 STP boundary 外 guard-band 采样。
   - B3：corner anchors + guard-band。

6. `CommercialCadLikeQualityGate` 第一版已新增：
   - 不替代 StrictTopologyGate。
   - 当前输出 boundary max/p95/RMS deviation、corner anchor drift、feature edge drift 和 sharp-corner-preservation pass/fail。
   - 后续仍需补 STEP roundtrip 后几何重复测量、surface COPS-like deviation 和 Creo / commercial CAD 阈值标定。
```

**冻结范围：**

```text
- 不继续把 OCCT PlaneRegionMerge / A6 作为当前主线增强。
- 不把 Geomagic patch outer boundary 或 STL crop boundary 当最终 CAD boundary。
- 不绕过 StrictTopologyGate。
- 不让 redo 重新运行 Geomagic / crop / import / repair。
- 不在默认单元测试里依赖真实 Geomagic 或真实大样例路径。
```

---

## 9. 版本进度判断

当前阶段判断：

```text
MVP 基础闭环：已完成
锁边交互闭环：已完成
最小 undo/redo：已完成
same-domain 合并闭环：已完成
P0 稳定性收口：基本完成
复杂 STEP 样例回归测试：待做
合并候选规划：已完成基础版
候选区域 GUI 预览：已完成基础版
候选区域选择/接受/拒绝/隐藏：已完成基础版
Face / Candidate Inspect：已完成基础版
Analytic primitive candidate detection：已完成增强 B
Stage 2.8 Enhancement A B-spline CylinderLike approximate detection：已完成
Stage 2.8 Enhancement B B-spline ConeLike / FrustumLike approximate detection：已完成
Multi-type candidate preview：已完成基础版
RegionMerger 框架准备：已完成基础版
PlaneRegionMerge：已完成基础可用版（导出稳定性未验证）
平面候选批量合并：已完成基础版
平面合并边界简化：已完成基础版
Stage 3-S Shared Primitive Fields：已完成
SphereRegionMerge：已完成稳定版调整（标记为实验性）
Geomagic patch preview / Apply 主线：以 1f0c3d7 文档同步提交为当前可缝合基准
STP Sampled Candidate Surface：已完成并作为当前默认 fitting input mode
STL Global Cut Chain crop：已完成第一版并作为可选裁剪器接入 GUI
GUI 后台任务 / 状态显示优化：已合入 1f0c3d7 基准
d1c00e4 Improve boundary constrained Geomagic patch flow：因真实样例缝合回归，暂不进入当前主线
Sharp Contours 实验：无收益，暂不进入当前主线
下一阶段硬问题：Geomagic 圆角化 sharp corner 后，OCCT 门控可能通过但商业 CAD 仍出现缝隙；需要新增 corner-aware fitting input 与商业 CAD 近似门控实验
```

**旧 OCCT 近似平面路线定位：**

```text
Stage 3A-Fix / A6 只保留为历史诊断与研究分支。
它不再是当前推荐的主推进路线。
除非明确回到 OCCT 近似平面合并问题，否则不应抢占 Geomagic patch input / Apply 收口任务。
```

**冻结区域：**

```text
CylinderRegionMerge：冻结
ConeRegionMerge：冻结
TorusRegionMerge：冻结
SphereRegionMerge 进一步扩展：冻结
Freeform Candidate Detection：冻结
Freeform B-spline / Plate Refit：冻结
项目保存恢复：冻结
完整误差评估：冻结
高级特征线：冻结
局部重拟合：冻结
```

总体评价：

```text
当前项目已经从候选生成 / PlaneRegionMerge 实验阶段推进到 Geomagic patch preview / Apply 阶段。
T6.7.4 已经把 replacement boundary 从 Geomagic patch outer boundary 收回到原 STP candidate outer boundary wire。

当前输入路线有两条：
1. STP Sampled Candidate Surface：当前默认。速度更快，不依赖源 STL，效果与 STL crop 方案接近。
2. 原始 STL crop：保留 legacy / conservative 两种 automatic fitting input mode；Global Cut Chain 是独立 STL 裁剪路线，用于对照、诊断或必须使用源 STL 几何采样的场景。

下一步的硬问题不是继续调 STL crop tolerance，也不是回退 patch outer-boundary replacement；
而是让 T6.7.4 后的 replacement shell / repair / StrictTopologyGate 在真实样例上稳定闭合。
```

---

## 10. Stage 3A-Approx / A6 历史诊断分支（2026-05-27）

```text
Stage 3A-Approx / A6：历史诊断分支，非当前默认推进路线

已完成：
- A6.1：PlaneRegionMerger 在 approximate planar rebuild 进入 MakeFace 前检查 ordered boundary edges。
- A6.1：boundary 3D curve 采样点若明显偏离拟合平面，提前返回 DeviationTooLarge。
- A6.1：失败时 result.document 保持原 document，Command 层仍不会污染当前模型。
- A6.1：新增测试覆盖“face 采样误差低，但 boundary 曲线偏离拟合平面”的失败路径。
- A6.2：RegionMergeResult 新增 diagnostic_report，用于承载近似平面重建失败诊断。
- A6.2：PlaneRegionMerger approximate mode 失败路径会输出 candidate id/type/status、face ids、boundary/internal/ordered edge ids、拟合平面、face/boundary deviation、RegionBoundaryAnalyzer 结果、每条 boundary edge 的 3D curve type、start/mid/end 距离和 pcurve 信息。
- A6.2：data/samples/3#_底部.stp 已纳入自动测试；样例存在时会读取真实 STP、生成候选并验证失败诊断报告，同时确认 document/stats 不被污染。

仍未完成：
- A6.3：ShapeFix_Wire / ShapeFix_Face / SameParameter 最小修复路径评估。
- A6.4：projected boundary / pcurve rebuild 设计与验证。

当前结论：
A6.1 不是提升合并力度，而是防止不安全的近似平面候选进入 MakeFace 后才在 BRepCheck 阶段失败。
A6.2 让真实失败样例具备可定位诊断。若后续明确回到 OCCT 近似平面合并，再基于 diagnostic_report 做 A6.3；当前不应让 A6 抢占 Geomagic patch input / Apply 收口。
```
