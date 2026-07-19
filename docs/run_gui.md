# GUI 启动、操作与手动验证

本文档说明如何在 VS Code 或命令行中启动 `step-patch-optimizer` GUI，并给出当前 GUI 的主要交互方式和手动验证流程。

> 当前状态：`OccViewWidget` 已接入真实 OCCT Viewer。产品主 GUI 流程是任意连通面选区的原生 B-rep 曲面减面，支持后台预览、质量报告、事务 Apply、undo/redo 和 STEP 导出。Geomagic 保留在“高级”菜单作为显式旧后备，不会由原生流程自动回退。

---

## 1. 前置环境

当前工程默认使用：

```powershell
CMake
Visual Studio 2022 Build Tools
vcpkg
Qt6
OpenCASCADE / OCCT
```

默认 vcpkg 路径：

```powershell
%USERPROFILE%\vcpkg
```

CMake preset 已写在项目根目录：

```text
CMakePresets.json
```

推荐直接使用 preset 或仓库中的脚本，不手写复杂 CMake 参数。

---

## 2. 推荐脚本启动方式

首次配置、依赖安装、构建和测试：

```powershell
cd D:\pyProject\step-patch-optimizer
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\bootstrap_windows.ps1
```

初始化完成后直接启动 GUI：

```powershell
.\scripts\bootstrap_windows.ps1 -RunGui
```

日常构建与启动：

```powershell
cd D:\pyProject\step-patch-optimizer
.\scripts\build_debug.ps1
.\scripts\run_gui.ps1
```

运行测试：

```powershell
.\scripts\test.ps1
```

完成后终端会打印本轮独立目录，例如：

```text
data/runs/20260710_184429_299__unit-tests__repository__cand-na__windows-msvc-debug/
```

目录内的 `RUN.md` 用于人工查看，`run.json` 用于脚本查询，`logs/build.log` 与 `logs/ctest.log` 保存本轮真实输出。查询入口：

```powershell
.\scripts\list_runs.ps1 -Latest 10
.\scripts\list_runs.ps1 -Workflow unit-tests -Status passed -Latest 5
```

该命令只运行不依赖仓库真实大文件和外部 GUI 程序的默认单元测试。需要显式运行较慢测试时：

```powershell
cmake --build --preset windows-msvc-debug --target spo_real_file_tests -- /m:1
cmake --build --preset windows-msvc-debug --target spo_real_geomagic_tests -- /m:1
```

前者读取仓库中的真实 STL/STEP 样例；后者会实际启动 Geomagic。`verify_spo.ps1 -RealGeomagic` 只执行一次完整 scripted baseline，不会再由默认 `spo_tests` 预先重复启动 Geomagic。

---

## 3. VS Code 启动方式

1. 用 VS Code 打开项目根目录：

```powershell
code D:\pyProject\step-patch-optimizer
```

2. 推荐安装扩展：

```text
C/C++
CMake Tools
```

仓库已提供 `.vscode/extensions.json`，VS Code 会提示安装推荐扩展。

3. 在 VS Code 中选择 preset：

```text
Configure Preset: windows-msvc-debug
Build Preset: windows-msvc-debug
```

4. 构建：

```text
Ctrl + Shift + P
→ CMake: Build
```

5. 启动 GUI：

```text
F5
→ Launch step-patch-optimizer
```

也可以通过：

```text
Ctrl + Shift + P
→ Debug: Select and Start Debugging
→ Launch step-patch-optimizer
```

---

## 4. 命令行启动方式

在 PowerShell 中进入项目目录：

```powershell
cd D:\pyProject\step-patch-optimizer
```

配置工程：

```powershell
cmake --preset windows-msvc-debug
```

构建 GUI：

```powershell
cmake --build --preset windows-msvc-debug --target step-patch-optimizer
```

启动 GUI：

```powershell
.\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

构建并运行测试：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests
ctest --preset windows-msvc-debug --output-on-failure --timeout 120
```

---

## 4.1 命令行 baseline / A0-B2 自动验证

不需要人工打开 GUI 的 A0 baseline 脚本入口：

```powershell
cd D:\pyProject\step-patch-optimizer
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

B1 corner / feature edge 加密采样：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B1 复用已有 patch 时：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B1 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId 7 `
  -Patch "D:\path\to\patch.stp" `
  -AllowQualityGateFailure
```

B2 / B2.2 candidate/source-face parallel over-cover：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2 和 B2.2 现在都表示候选源面平行外扩。默认参数：

```text
CandidateOverCoverWidth = 0.25
CandidateOverCoverRings = 3
CandidateOverCoverSamples = 64
CandidateOverCoverMiterMaxScale = 1.25
```

底层 probe 参数为：

```text
--candidate-surface-over-cover
--candidate-over-cover-width
--candidate-over-cover-rings
--candidate-over-cover-samples
--candidate-over-cover-miter-max-scale
```

JSON 的 `stp_sampled_fitting` 节输出 `candidate_surface_over_cover_*` 字段。该路线沿原候选 STP face 边界生成外扩带，外扩点优先使用 source face 切平面 3D offset；`normal_leakage`、`direction_fallback`、`long_triangle`、`max_triangle_edge_length`、`source_face_count` 和 `direction_flip` 字段用于解释方向回退和异常三角保护。最终 Apply 仍使用原 STP boundary strict re-trim 裁回。

B2.2 等价入口：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.2 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

B2.3 candidate over-cover + corner-safe auxiliary support collar：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.3 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic
```

显式 SharpenContours A/B：

```powershell
.\scripts\run_corner_baseline_gate.ps1 `
  -Experiment B2.3 `
  -SourceStep "D:\path\to\model.stp" `
  -CandidateId auto `
  -RealGeomagic `
  -SharpenContours
```

B2.3 在 candidate over-cover 基础上额外启用窄 adjacent-face support collar 和 corner-safe clamp。默认辅助参数：

```text
SupportCollarWidth = 0.05
SupportCollarRings = 1
SupportCollarSamples = 64
SupportCollarMaxOffsetScale = 1.25
```

这条路线用于 A/B 验证相邻面上下文是否仍有帮助，不再让 adjacent-face support collar 作为主扩宽方向。`-SharpenContours` 仍是 Geomagic A/B 开关，默认关闭。

B2.4 是 Apply 侧 Boundary Edge Rebuild + Local Closure Probe，不是新的 `-Experiment B2.4` fitting input。验证 B2.4 时仍可使用 B2.3 / `-SharpenContours` 生成或复用 patch；区别在于 Patch Apply report / baseline JSON 会新增 multi-surface boundary edge pcurve rebuild 与 SameParameter diagnostics：

```text
multi_surface_boundary_edge_pcurve_rebuild_attempt_count
multi_surface_boundary_edge_pcurve_rebuild_success_count
multi_surface_boundary_edge_pcurve_rebuild_failure_count
multi_surface_boundary_edge_same_parameter_check_count
multi_surface_boundary_edge_same_parameter_failure_count
multi_surface_boundary_edge_max_same_parameter_deviation
multi_surface_boundary_edge_pcurve_rebuild_failed_edge_ids
multi_surface_boundary_edge_same_parameter_failed_edge_ids
```

这些字段用于判断真实缝合失败到底是原 STP boundary 3D curve 到 fitted surface 的 pcurve 投影失败、SameParameter 偏差超限，还是后续 repair / sewing / StrictTopologyGate 仍无法闭合。B2.4 不改变 redo 语义，不重跑 Geomagic，不把 STL crop boundary、support collar 外环或 Geomagic patch outer boundary 当最终 CAD boundary。

2026-06-17 的真实样例复用 B2.3 + `SharpenContours` patch 后，B2.4 Apply report 显示 `pcurve_rebuild_attempt=27`、`success=27`、`failure=0`、`same_parameter_failure=0`，但 `StrictTopologyGate` 仍因 `FreeEdgeIncreased` 失败，`gate_after_free_edges=1`，因此仍没有 applied STEP 导出。这个结果说明 B2.4 已经把边界 pcurve 表征问题单独排除，但没有解决最终水密闭合。

B2.5 pre-repair / post-repair closure probe 已接入。它不会删除 `PatchReplacementRepair`，也不会把 repair 前的 shape 当作可提交结果；它只是在 Apply report / baseline JSON 中新增 repair 前后的对照诊断，用于回答“free edge 是 shell 构造阶段已经存在，还是 repair/sewing 后才出现”。字段包括 pre/post repair free edge、multiple edge、BRepCheck、拓扑计数，以及 free edge midpoint / endpoints / matched original boundary edge / split segment / patch face owner / appeared_after_repair。2026-06-17 复用真实 B2.3 + SharpenContours patch 的 B2.5 报告显示：pre-repair free edges=66，post-repair free edges=1，post free edge `appeared_after_repair=true`，StrictTopologyGate 仍因 `FreeEdgeIncreased` 正确阻止提交。

B2.6 是 Apply 侧 Local Free-edge Closure Fix 的最小诊断 / repair selection / validator 分类修正版，不是新的 `-Experiment B2.6` fitting input。验证入口仍复用 `corner_baseline_probe` / `patch_apply_probe` / GUI Apply report。当前已输出 detected edge 的 midpoint / endpoints / length / tolerance / degenerated、nearest original boundary edge context、split owner diagnostics、fitted patch projection-distance 和 repair degenerated-free-edge stats；repair best-result 在同等 free edge 数下惩罚 degenerated free edge。`ShapeValidator` 现在会把 closed seam / OCCT degenerated edge 从 StrictTopologyGate 的真实 open-boundary 计数中排除，普通 open face 仍会被计为 free edge。真实样例 `03_配件_Clay.stp` 复用 B2.3 + SharpenContours patch 后，`StrictTopologyGate` 与 applied STEP readback 已通过：gate after/roundtrip free edges=0，pre-repair free-edge-like diagnostics=66，post-repair free edge=0，post-repair degenerated diagnostic edge=1；`CommercialCadLikeQualityGate` 仍因 boundary/corner/feature drift 约 0.070672 失败。当前真实样例 nearest original boundary edge 为 1600，但 B2.6 不把该 id 写死。B2.6 不改变 redo 语义，不重跑 Geomagic，不把 Blender 四边形化或 STL/Geomagic patch outer boundary 当成最终 CAD boundary，也不放宽 StrictTopologyGate。

B2.7 是结果定位诊断，不是新的 `-Experiment B2.7` fitting input，也不是 GUI 中新增的修复按钮。当前已接入 `trim_diagnostics`：replacement face count、trim wire invalid count、over-cover / under-cover samples、boundary seam gap、internal seam gap、worst boundary/internal edge、roundtrip_changed。输出路径包括 PatchReplacementReport、`corner_baseline_probe` baseline JSON、`patch_apply_probe` 和 GUI Apply report 摘要。2026-06-18 复用真实 B2.3 + SharpenContours patch 后，`StrictTopologyGate` 与 applied STEP readback 通过，`CommercialCadLikeQualityGate` 仍失败；B2.7 指标为 over-cover `0/29`、under-cover `81/256` 且 max `2.03433`、boundary gap max `0.0706854`、internal seam gap `0`、roundtrip changed `false`。这说明当前不是 STEP roundtrip 把面片弄坏，也不是 replacement face 大面积超出原 STP boundary 后没有裁掉；更直接的问题是局部 coverage 不足和边界贴合峰值误差。GUI 观察到的缝隙仍需要用该诊断定位到局部 boundary / owner / split，而不能用放宽 Gate 或替换最终边界来源处理。

B2.8 是外部 CAD 诊断路由，不是新的 `-Experiment B2.8` fitting input，也不是 GUI 中新增的修复按钮。当前已接入 `external_cad_diagnostics`：原始 Geomagic 补片 STEP 的预诊断角色、最终已合并 STEP 诊断资格、最终诊断输入路径、执行状态和跳过原因。输出路径包括 PatchReplacementReport、`corner_baseline_probe` baseline JSON、`patch_apply_probe` 和 GUI Apply report 摘要。规则很简单：原始补片只能做补片自身预诊断；只有 Patch Apply、StrictTopologyGate、已合并 STEP 导出和二次读取全部成功后，最终外部 CAD 诊断才 eligible，并且输入文件必须是已合并 STEP。

2026-06-18 复用真实 B2.3 + SharpenContours 补片后，B2.8 报告为 `data\baseline_runs\scripted_b2_8_apply_reuse_b2_3_sharpen_w005_auto_external_cad_route\baseline_report.json`，已合并 STEP 为 `data\baseline_runs\scripted_b2_8_apply_reuse_b2_3_sharpen_w005_auto_external_cad_route\03_配件_Clay_candidate_0179_applied.stp`；`StrictTopologyGate` 与已合并 STEP 二次读取通过，`CommercialCadLikeQualityGate` 仍失败，最终外部 CAD 诊断状态为 `PendingExternalRunner`。

如需在后台调用 Creo 检查 B2.8 标记的已合并 STEP，使用显式脚本，不通过 GUI redo 或默认测试触发：

```powershell
cd D:\pyProject\step-patch-optimizer
.\scripts\run_creo_step_diagnostic.ps1 `
  -StepPath "<applied STEP>" `
  -CreoRoot "E:\Proe\Creo 11.0.0.0"
```

该脚本的真实链路是先用 Creo Distributed Batch 无图形模式执行 `step_3d_import.ttd`，把 STEP 导入成 `.prt`；只有 `.prt` 生成后，才继续执行 `modelcheck.ttd`。结果写入 `creo_step_diagnostic_result.json`。当前已确认后台链路能跑通：`.dxc` 使用 `DSQM="_LOCAL"` 后，B2.8 已合并 STEP 可生成 `input.prt.1`，并生成 `input.p.html` / `input.p.xml`。脚本会解析 ModelCHECK XML，输出 `modelcheck.summary` 和 B2.9 的 `creo_diagnostic_correlation`。真实样例结果为 `diagnostic_passed=false`，统计 PASS=26、INFO=33、WARNING=3、ERROR=2；失败项为 `GEOM_CHECKS=1` 与 `SHORT_EDGES=1491`，导入校验为 `PTC_VAL_IMP_PART_STATUS=SOLID_FAILED`、`PTC_VAL_IMP_SCORE=FAIL`。因此 GUI/CLI 路由已经能接到 Creo 真实诊断，但当前已合并 STEP 没有通过 Creo 检查。

如果只需要重新解析已有 ModelCHECK XML 并关联已有 baseline JSON，不启动 Creo：

```powershell
.\scripts\run_creo_step_diagnostic.ps1 `
  -ParseModelCheckOnly `
  -ModelCheckXmlPath "<modelcheck xml>" `
  -BaselineReportPath "<baseline_report.json>"
```

未显式传 `-OutputDir` 时，脚本会创建新的 canonical run 目录并在终端打印路径；不要复用上一轮目录。

该 parse-only 路径会保留 `GEOM_CHECKS` / `SHORT_EDGES` 的 item 明细，输出短边 Creo edge id 摘要和导入 feature id；但当前 ModelCHECK XML 不含空间坐标或 OCCT edge id，所以只能做阶段级关联，不能直接定位到项目内部 edge。

脚本会构建 `corner_baseline_probe`、运行与 GUI 同源的核心 pipeline、写出 JSON 报告。默认不传 `-RealGeomagic` 且找不到已有 patch 时会跳过，避免普通验证依赖真实 Geomagic。`-Experiment B1` 会提高 STP-sampled fitting STL 的连接 surface grid 密度，并在 `stp_sampled_fitting` 节输出 dense sample / corner anchor / surface division 统计；`-Experiment B2` / `-Experiment B2.2` 会启用 candidate/source-face parallel over-cover，并显式传递 candidate over-cover 宽度、ring、samples 和 miter 参数；`-Experiment B2.3` 会在 candidate over-cover 上额外启用窄 adjacent-face support collar 与 corner-safe clamp，并可用 `-SharpenContours` 做 Geomagic A/B。旧 B2.0 guard-band 和 B2.1 over-cover strip 已移除，不再是可选实验入口。它不是窗口点击级 GUI 自动化，但覆盖的是 GUI Patch preview / Apply 使用的核心后端链路。

底层 probe 也可直接运行：

```powershell
cd D:\pyProject\step-patch-optimizer
cmake --build --preset windows-msvc-debug --target corner_baseline_probe
.\build\windows-msvc-debug\Debug\corner_baseline_probe.exe `
  --source-step "D:\path\to\model.stp" `
  --candidate-id auto
```

默认流程：

```text
read STEP
→ detect feature edges
→ plan feature-bounded candidates
→ analyze original STP boundary
→ generate STP-sampled fitting STL
→ run Geomagic AutoSurface
→ import generated patch
→ Patch Apply with StrictTopologyGate
→ CommercialCadLikeQualityGate
→ write JSON report
```

无 Geomagic 或复用已有 patch 时：

```powershell
.\build\windows-msvc-debug\Debug\corner_baseline_probe.exe `
  --source-step "D:\path\to\model.stp" `
  --candidate-id 7 `
  --patch "D:\path\to\patch.stp"
```

默认输出：

```text
D:\pyProject\step-patch-optimizer\data\runs\<timestamp>__corner-baseline__<model>__cand-<id>__<variant>\baseline_report.json
```

同一运行目录还固定包含：

```text
run.json
RUN.md
artifacts/
diagnostics/
logs/
```

Geomagic staging 使用与 GUI 一致的路径和文件名：

```text
data\crop_stl\<model>\<model>_candidate_<id>.stl
data\crop_stp\<model>\<model>_candidate_<id>.stp
data\crop_stp\<model>\<model>_candidate_<id>_fit_region.log
```

这些 `data/crop_*` 路径是共享暂存区，会被后续同模型、同候选运行覆盖。需要复核某一轮时，只使用该轮 `data/runs/<run-id>/artifacts`、`diagnostics`、`logs` 和 `run.json` 中登记的路径。

GUI 一键 Patch preview 还会为每次点击生成独立 root run log：

```text
log\patch_preview_<timestamp>_candidate_<id>.log
```

状态栏会显示当前阶段和总 elapsed。若 `fit_region.log` 显示 Geomagic Wrap 内部很快结束，但 GUI 仍在运行，优先查看 root run log 中 `RunningGeomagic`、`ImportingPatch`、`PreviewReady` 的 `duration_ms`。

报告包含 STP-sampled fitting STL 统计、Geomagic 输出路径、Patch preview/import 统计、Patch Apply / StrictTopologyGate 统计、B2.8 外部 CAD 诊断路由，以及 `CommercialCadLikeQualityGate` 的 boundary drift、seam_continuity、corner anchor drift 和 feature edge drift。

该脚本是 A/B 实验入口，不等同于 Creo 最终验收。它的作用是把 GUI 手工流程中的核心后端路径变成可重复的 baseline 数据生成器。

---

## 5. 当前 GUI 主流程

当前 GUI 产品主流程：

```text
打开 STEP/STP
→ 显示 B-rep 模型
→ 检测基础特征边
→ 选择候选区域
→ 原生 B-rep 重拟合
→ 显示预览效果
→ 应用通过全部质量门的结果
→ undo / redo
→ 导出新的 STEP/STP 文件
→ 导出后二次读取校验
```

face / edge 手动选择、锁边、same-domain 合并和 Geomagic 仍保留，但不再替代上述产品主流程。

### 5.1 打开模型

菜单或工具栏：

```text
文件 → 打开 STEP/STP
```

支持格式：

```text
.step
.stp
.STEP
.STP
```

成功打开后，左侧模型树会显示实体、壳、面、边等统计信息，中央 OCCT Viewer 会显示真实 B-rep 模型。

### 5.2 特征边检测

菜单或工具栏：

```text
检测 → 检测特征边
```

当前基础特征边包括：

```text
1. sharp edge：基于二面角阈值。
2. free edge：只有一个邻接面的边。
3. multiple edge：邻接面数量大于 2 的异常边。
```

检测结果会以高亮线条显示，并在报告面板输出特征边数量。
注意：FeatureEdgeDetector 的可视化 free edge 用于提示拓扑边界；Validate / Patch Apply / StrictTopologyGate 使用 `ShapeValidator`，会排除 closed seam / OCCT degenerated edge 的误报，但仍会拦截普通 open edge。

### 5.3 用户锁边 / 解锁边

操作流程：

```text
1. 切换到边选择模式。
2. 左键选择边。
3. Shift + 左键多选边。
4. Ctrl + 左键从选择集中移除边。
5. 右键打开上下文菜单。
6. 选择“锁定选中边”或“解锁选中边”。
```

锁定边会以高亮方式显示，并在执行合并时加入 protectedEdges，防止合并跨越用户认为重要的结构边。

### 5.4 候选区域选择与原生 B-rep 重拟合

主菜单或工具栏：

```text
检测特征边
→ 选择候选区域
→ 拟合设置
   → 仅 STEP B-rep
   → STEP B-rep + 扫描 STL 软参考
→ 原生重拟合（预览/应用）
→ 一键预览并应用
```

操作流程：

```text
1. 打开 STEP/STP 后点击“检测特征边”。
2. 点击“选择候选区域”。如果候选尚未生成，GUI 会先在后台生成候选并自动进入候选选择模式。
3. 在模型上单击一个候选区域；原生重拟合直接使用该候选的 FaceId 集合，不再读取候选模式下为空的手工面选区。
4. 默认使用“仅 STEP B-rep”：原始 STEP 曲面、参数域和边界是拟合与终验真值。
5. 如需独立扫描辅助，先从“高级 → STL 裁剪 → 打开原始 STL”加载扫描 STL，再显式切换到“STEP B-rep + 扫描 STL 软参考”。
6. 点击“原生重拟合（预览）”。GUI 使用 4 个固定工作线程运行确定性拟合；预览不会修改文档。报告页每 2 秒刷新心跳，并实时追加拓扑准备、束搜索层、唯一分组拟合计数、接缝候选终验、商业 CAD 类门、整件组装、`StrictTopologyGate` 和 STEP 回读等阶段。预览通过后，同一按钮变为“原生重拟合（应用）”。
7. 也可点击“一键预览并应用”：预览通过全部质量门后自动提交；任一质量门失败时只保留报告和预览信息，不修改文档。
8. Apply 在后台提交已经验证的缓存结果，不会重新拟合；撤销和重做同样交换缓存文档。
```

加载扫描 STL 不会再静默改变拟合输入；是否使用扫描软参考由上述模式显式决定。扫描 STL 只提供内部位置软参考，不替代 STEP 边界、参数域或最终质量门。

当前主线中，STEP 导入、特征边检测、候选生成、原生重拟合预览和 Apply 都在后台执行；报告页和底部状态栏持续显示心跳，窗口事件循环不会因这些长计算被占用。候选点击和预览结果显示只更新 GUI 状态，本身不是长计算。

GUI 默认重拟合合同是 `StepBrepOnly`、`targetOutputRatio=0.5`、提案分组大小不限、禁用持久拟合缓存、4 个工作线程。因此，49 面候选在 GUI 默认设置下会尝试搜索到不超过 24 面。`data/runs/20260715_surface_reduction_candidate_0179_hybrid/` 中锁定的 `49→46` 回归使用原始扫描 STL 软参考、`targetOutputRatio=0.95`、最多二面提案和分层脚本缓存策略。两条路径使用同一个 `SurfaceReductionEngine`、重裁剪和最终质量门，但参数和输入证据不同，GUI 默认运行不是锁定回归的直接回放。

重新检测特征边、切换到另一个候选、锁边/解锁边、Apply、撤销或重做都会使旧原生重拟合预览失效，防止把旧候选的缓存结果应用到新候选。

### 5.5 旧 same-domain 合并实验

菜单或工具栏：

```text
合并 → 执行合并
```

当前执行的是基础版 same-domain 合并：

```text
自动特征边 + 用户锁定边 → protectedEdges
protectedEdges 传入 SameDomainUnifier
OCCT same-domain unify 尝试合并同一几何域上的相邻 faces
```

注意：该入口是旧的同域合并实验，不是当前产品主拟合流程。当前主流程是上一节的 `SurfaceReductionEngine` 自制受限 B-spline B-rep 减面；Geomagic 候选、Patch preview 和 Apply 只保留在“高级 → Geomagic 后备（旧流程）”中。

### 5.6 合法性检查

菜单或工具栏：

```text
验证 → 合法性检查
```

当前基础检查包括：

```text
1. 是否存在 shape。
2. OCCT BRepCheck 是否通过。
3. 实体 / 壳 / 面 / 边数量统计。
4. free edge 数量（排除 closed seam / OCCT degenerated edge，普通 open edge 仍计入）。
5. multiple edge 数量。
```

### 5.7 导出 STEP/STP

菜单或工具栏：

```text
文件 → 导出 STEP
```

导出与二次读取校验会在后台执行，期间 GUI 不应卡死或进入未响应状态。状态栏 / 报告面板会显示 STEP 正在后台导出；完成后报告面板会显示二次读取校验结果。如果二次读取失败，主模型仍保持当前内存状态，报告面板会显示错误信息。

---

## 6. 鼠标与快捷键

| 操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标右键拖拽 | 平移视图 |
| Alt + 鼠标左键拖拽 | 平移视图 |
| 鼠标滚轮 | 放大 / 缩小 |
| 鼠标左键 | 选择当前模式下的 face 或 edge |
| Shift + 鼠标左键 | 多选 face 或 edge；已选中时再次点击可切换选择状态 |
| Ctrl + 鼠标左键 | 从当前选择集中移除 face 或 edge |
| 鼠标右键 | 打开上下文菜单 |
| F | 切换到面选择模式 |
| E | 切换到边选择模式 |
| H | 显示 / 隐藏特征线 |
| M | 预览合并候选入口 |
| R | 重置视角 |
| V | 合法性检查 |
| Ctrl + Z | 撤销最近一次可撤销编辑 |
| Ctrl + Y | 重做最近一次撤销编辑 |

---

## 7. 手动 GUI 验证流程

每次修改 GUI、Command、合并或验证相关代码后，建议执行以下手动验证：

```text
1. 启动 GUI。
2. 打开一个 STEP/STP 文件。
3. 确认模型在中央 Viewer 中正确显示。
4. 切换到面选择模式，点击若干 face，确认检查面板显示 Face ID 和邻接信息。
5. 使用 Shift + 左键多选 face，确认选择高亮正常。
6. 切换到边选择模式，点击若干 edge，确认检查面板显示 Edge ID 和邻接面数量。
7. 使用 Shift + 左键多选 edge。
8. 使用 Ctrl + 左键移除某条已选 edge。
9. 右键选择“锁定选中边”，确认锁边高亮显示。
10. Ctrl + Z，确认锁边撤销。
11. Ctrl + Y，确认锁边恢复。
12. 执行“检测特征边”，确认特征线显示，报告面板输出数量。
13. 点击“选择候选区域”，确认候选自动生成并进入候选选择模式。
14. 在模型上单击候选，确认当前候选 ID 和区域高亮正确。
15. 点击“原生重拟合（预览）”，确认预览期间按钮禁用、窗口仍可交互，报告页每 2 秒刷新心跳且阶段事件持续追加；完成后显示结果，通过质量门时同一按钮变为“原生重拟合（应用）”。
16. 点击同一按钮应用，确认模型刷新；Ctrl + Z / Ctrl + Y 确认使用缓存结果撤销和重做。
17. 重新执行一次候选选择，验证“一键预览并应用”只在预览全部通过时自动提交。
18. 执行“合法性检查”，确认报告面板输出 BRepCheck、free edge、multiple edge 等信息。
19. 导出 STEP/STP，并确认导出期间 GUI 仍可响应窗口刷新。
20. 确认导出完成后报告面板显示二次读取校验通过。
21. 关闭并重新打开导出的 STEP/STP，确认可正常显示。
```

---

## 8. 常见问题

### 8.1 PowerShell 禁止运行 ps1

如果出现脚本执行策略错误，可以只对当前终端放开：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

然后重新执行脚本。

### 8.2 找不到 cmake

重新打开 PowerShell 或 VS Code，让系统 PATH 生效。

也可以临时执行：

```powershell
$env:Path='C:\Program Files\CMake\bin;%USERPROFILE%\vcpkg;' + $env:Path
```

### 8.3 找不到 Qt6 或 OpenCASCADE

先确认依赖已经安装：

```powershell
.\scripts\setup_deps.ps1
```

再重新配置和构建：

```powershell
.\scripts\configure.ps1
.\scripts\build_debug.ps1
```

不要直接运行裸命令：

```powershell
cmake -S . -B build
```

因为裸命令不会自动带上本项目 preset 里的 vcpkg toolchain，容易找不到 Qt6 或 OpenCASCADE。

### 8.4 Qt platform plugin "windows" 找不到

如果运行 exe 时出现：

```text
Could not find the Qt platform plugin "windows"
```

说明 Qt 的运行时插件还没有部署到 exe 同级目录。正常情况下，当前工程的 CMake 会在构建后自动执行 `windeployqt`。可以先重新构建：

```powershell
.\scripts\build_debug.ps1
```

如果还不行，可手动执行：

```powershell
%USERPROFILE%\vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.debug.bat --no-translations --no-system-d3d-compiler --no-compiler-runtime D:\pyProject\step-patch-optimizer\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

部署成功后，exe 同级目录下应出现：

```text
platforms\qwindowsd.dll
styles\
imageformats\
```

### 8.5 链接失败 LNK1168

如果构建时出现：

```text
LINK : fatal error LNK1168: 无法打开 ... step-patch-optimizer.exe 进行写入
```

说明 GUI 程序正在运行。关闭窗口后重新构建：

```powershell
.\scripts\build_debug.ps1
```

### 8.6 VS Code 没有识别 preset

确认安装了 CMake Tools 扩展，并重新打开项目根目录：

```powershell
code D:\pyProject\step-patch-optimizer
```

VS Code 打开的目录必须是项目根目录，里面应该能看到：

```text
CMakeLists.txt
CMakePresets.json
.vscode/
src/
docs/
scripts/
```
