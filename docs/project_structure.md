# 项目结构与目录职责

项目名称：

```text
step-patch-optimizer
```

中文定位：

```text
特征感知的 STEP 曲面片合并与边界优化系统
```

英文定位：

```text
Feature-aware STEP Patch Optimization
```

---

## 1. 当前项目状态

当前项目已经完成 MVP 基础闭环、FeatureBoundedRefit 候选区域框架，以及 Geomagic patch preview / Apply 主线的核心接入。当前可运行流程是：

```text
STEP/STP 读取
→ B-rep 拓扑索引构建
→ OCCT Viewer 显示
→ face / edge 选择与多选
→ 基础特征边检测
→ 用户锁边 / 解锁边
→ FeatureBoundedRefit 候选区域生成和预览
→ STP Sampled Candidate Surface fitting STL
→ Geomagic AutoSurface patch generation / import / overlay preview
→ original STP boundary constrained Patch Apply
→ PatchReplacementRepair
→ StrictTopologyGate / CommercialCadLikeQualityGate
→ STEP/STP 导出、二次读取校验和可选外部 CAD 诊断
```

当前阶段已经从“搭建项目结构 / 候选生成”转入 Route 2 support collar 后的 Apply 收口诊断。当前重点是：

```text
1. 默认 fitting input：STP Sampled Candidate Surface。
2. 当前唯一保留的 STL 扩宽机制：adjacent-face support collar。
3. Apply 阶段继续以原 STP candidate outer boundary wire 作为最终 CAD boundary。
4. 当前 P0：定位 72-face Geomagic patch 下 strict multi-surface boundary shell open-wire closure。
5. 继续保留 PatchReplacementRepair、StrictTopologyGate、CommercialCadLikeQualityGate 和 external CAD diagnostic routing。
6. ProjectSerializer、完整 ReportGenerator、高级特征线和 SurfaceRefitter 仍是后续增强，不是当前 P0。
```

---

## 2. 推荐技术栈

当前主技术路线：

```text
C++20 + OCCT + Qt6 + CMake + vcpkg
```

当前核心依赖：

```text
OpenCASCADE / OCCT   STEP 读写、B-rep 拓扑、几何算法、Viewer
Qt6                  GUI、窗口、菜单、面板、交互
CMake                构建系统
vcpkg                依赖管理
```

后续可选依赖：

```text
CGAL          用于 mesh 层 sharp edge / region segmentation 辅助
Eigen         用于线性代数、拟合、PCA 等计算
nlohmann/json 用于项目文件与参数保存
spdlog        用于日志
GoogleTest    用于单元测试；当前测试仍采用项目内轻量测试方式
```

第一阶段仍不建议引入过多复杂依赖。当前最应该推进的是在现有 OCCT + B-rep 拓扑基础上补齐合并候选规划。

---

## 3. 当前目录结构

```text
step-patch-optimizer/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── AGENTS.md
├── vcpkg.json
│
├── .vscode/
│   ├── extensions.json
│   ├── launch.json
│   ├── settings.json
│   └── tasks.json
│
├── docs/
│   ├── module_design.md
│   ├── implementation_status.md
│   ├── completed_features.md
│   ├── TODO.md
│   ├── geomagic_patch_workflow.md
│   ├── geomagic_patch_cleanup_plan.md
│   ├── project_structure.md
│   └── run_gui.md
│
├── scripts/
│   ├── bootstrap_windows.ps1
│   ├── build_debug.ps1
│   ├── configure.ps1
│   ├── run_boundary_trim_fill_experiments.ps1
│   ├── run_corner_baseline_gate.ps1
│   ├── run_creo_step_diagnostic.ps1
│   ├── run_creo_toolkit_baseline_probe.ps1
│   ├── run_creo_toolkit_phase1_repair.ps1
│   ├── run_geomagic_patch.ps1
│   ├── run_gui.ps1
│   ├── setup_deps.ps1
│   ├── test.ps1
│   └── verify_spo.ps1
│
├── scripts/geomagic_wrap/
│   └── autosurface_pipeline.py
│
├── log/                         # 运行时 Patch preview root run log，git ignore
│
├── src/
│   ├── app/                    # main, MainWindow, AppController, process status, patch preview logging
│   ├── gui/                    # OCCT viewer, panels, process status panel
│   ├── command/                # Command system, lock/unlock, same-domain merge, PatchReplacementCommand
│   ├── io/                     # STEP/STP and STL IO, project serializer placeholder
│   ├── brep/                   # ShapeDocument, face/edge index, topology graph, boundary wire helper
│   ├── feature/                # feature edge detection and constraint interfaces
│   ├── merge/                  # FeatureBoundedRefit candidates, boundary analysis, same-domain unifier
│   ├── stl/                    # STP sampled fitting mesh, STL crop, Global Cut Chain
│   ├── patch/                  # patch import, preview, original-boundary constrained Apply, repair, diagnostics
│   ├── external/geomagic/      # Geomagic AutoSurface config/backend/output path resolver
│   ├── validate/               # ShapeValidator, StrictTopologyGate, CommercialCadLikeQualityGate
│   └── common/                 # common types, config, tolerance, Result
│
├── tests/
│   ├── fixtures/
│   ├── test_patch_replacement_command.cpp
│   ├── test_patch_replacement_repair.cpp
│   ├── test_strict_topology_gate.cpp
│   ├── test_commercial_cad_quality_gate.cpp
│   ├── test_stp_sampled_fitting_mesh.cpp
│   ├── test_stl_region_extractor.cpp
│   ├── test_geomagic_backend_mock.cpp
│   └── ...                    # 其余 command / IO / topology / patch / STL 回归测试
│
└── tools/
    ├── batch_optimize.cpp
    ├── step_stats.cpp
    ├── patch_apply_probe.cpp
    ├── corner_baseline_probe.cpp
    ├── strict_topology_gate_probe.cpp
    ├── step_unit_normalizer.cpp
    ├── creo_phase1_input_exporter.cpp
    └── g1_boundary_fill_patch_exporter.cpp
```

---

## 4. 文档职责

| 文档 | 作用 |
|---|---|
| `README.md` | 面向新用户：项目简介、环境配置、构建、运行、测试、常见问题 |
| `docs/module_design.md` | 长期架构文档：模块边界、依赖关系、阶段规划 |
| `docs/implementation_status.md` | 当前实现进度：已完成、待办、验收方式、近期开发顺序 |
| `docs/completed_features.md` | 已完成、已验证或已退役的功能事实，承接旧 TODO 中的历史计划 |
| `docs/TODO.md` | 当前执行 TODO：阶段任务、冻结范围、验收要求 |
| `docs/geomagic_patch_workflow.md` | Geomagic patch preview / Apply 主线流程 |
| `docs/geomagic_patch_cleanup_plan.md` | 旧 analytic region merge 路线清理计划和删除检查表 |
| `docs/run_gui.md` | GUI 使用文档：启动方式、操作方式、快捷键、手动验证流程 |
| `docs/project_structure.md` | 目录结构文档：当前文件树、目录职责、开发边界 |

开发时的文档优先级：

```text
架构边界：module_design.md
当前任务：implementation_status.md / TODO.md
已完成和已退役事实：completed_features.md
构建运行：README.md / run_gui.md
目录职责：project_structure.md
Geomagic patch 主线：geomagic_patch_workflow.md
旧代码清理：geomagic_patch_cleanup_plan.md
```

---

## 5. 目录职责说明

| 目录 | 当前职责 |
|---|---|
| `src/app/` | 程序入口、主窗口、应用级控制器；连接 GUI、Command 和几何模块 |
| `src/gui/` | Qt/OCCT GUI、三维视图、模型树、参数面板、日志/检查/验证/报告面板 |
| `src/command/` | 命令系统、CommandContext、CommandHistory、undo/redo、锁边命令、same-domain 合并命令、PatchReplacementCommand、导入导出命令 |
| `src/io/` | STEP/STP 读写、STL 二进制读写、导出后二次读取校验、项目文件保存恢复接口 |
| `src/brep/` | ShapeDocument、Face/Edge 索引、TopologyGraph、BoundaryWireBuilder、拓扑查询、基础统计 |
| `src/feature/` | 基础特征边检测、曲率估计接口、边界分类接口、用户约束接口 |
| `src/merge/` | same-domain 合并、FeatureBoundedRefit 候选结构、候选规划、区域边界分析、Face Inspect、曲面重拟合接口 |
| `src/stl/` | STP sampled fitting mesh、legacy/conservative STL crop、Global Cut Chain、STL mesh 和 crop report |
| `src/patch/` | Patch import / artifact lookup / preview report / Apply state / original-boundary constrained replacement / repair / trim diagnostics |
| `src/external/geomagic/` | Geomagic AutoSurface 配置、backend、result 和输出路径解析 |
| `src/validate/` | ShapeValidator、BRepCheck/free/multiple edge 检查、StrictTopologyGate、CommercialCadLikeQualityGate、误差评估接口、报告生成接口 |
| `src/common/` | 公共类型、配置、容差、Result 返回结构 |
| `tests/` | 单元测试、命令测试、patch / STL / gate / script contract 回归测试 |
| `tools/` | 命令行批处理、STEP 统计、Patch Apply probe、baseline probe、StrictTopologyGate probe、Creo / boundary fill 辅助工具 |
| `scripts/` | Windows 环境初始化、依赖安装、构建运行、验证、Geomagic、Route 2 和 Creo 诊断脚本 |
| `docs/` | 架构、进度、运行、目录文档 |

---

## 6. 当前已完成能力

### 6.1 GUI 与交互

```text
1. 主窗口布局。
2. OCCT Viewer 真实显示 STEP/STP 模型。
3. face / edge 命中选择。
4. Shift 多选 face / edge。
5. Ctrl 点击移除选择。
6. 鼠标中键旋转、滚轮缩放、右键或 Alt+左键平移。
7. 特征线显示。
8. 锁定边高亮显示。
9. undo / redo 按钮和快捷键。
10. FeatureBoundedRefit 候选区域高亮、选择、接受、拒绝、隐藏和恢复。
11. Patch preview overlay、crop / boundary diagnostics overlay。
12. Process Status Panel 展示 crop、Geomagic、import、Apply、repair、Gate 和 roundtrip 关键状态。
13. 基础报告、检查、验证信息展示。
```

### 6.2 几何主流程

```text
1. STEP/STP 读取。
2. B-rep 拓扑索引构建。
3. ShapeDocument 维护当前模型。
4. 基于二面角的 sharp edge 检测。
5. free edge / multiple edge 检测。
6. 用户锁边进入 protectedEdges。
7. MergePlanner / FeatureBoundedRegionBuilder 生成 FeatureBoundedRefit 候选区域。
8. STP Sampled Candidate Surface 生成 fitting STL。
9. Geomagic AutoSurface 生成 patch 并通过 PatchImportService 导入。
10. PatchReplacementCommand 执行 original-boundary constrained Apply。
11. PatchReplacementRepair 执行 ShapeFix / adaptive sewing / shell-to-solid / best-result selection。
12. StrictTopologyGate 执行 BRepCheck、free/multiple edge、solid/watertight、STEP export/readback gate。
13. CommercialCadLikeQualityGate 输出 boundary / corner / feature drift 和 seam continuity。
14. StepWriter 导出 STEP，外部 CAD 诊断路由只指向 Apply 成功后的已合并 STEP。
```

### 6.3 Command 与 undo/redo

```text
1. Command 基类。
2. CommandContext。
3. CommandHistory。
4. LoadStepCommand。
5. DetectFeatureCommand。
6. LockEdgeCommand。
7. UnlockEdgeCommand。
8. MergePatchCommand。
9. PatchReplacementCommand。
10. ValidateShapeCommand。
11. ExportStepCommand。
12. LockedEdgeRef，用于锁边几何签名和合并后重映射。
```

当前可撤销命令：

```text
LockEdgeCommand
UnlockEdgeCommand
MergePatchCommand
PatchReplacementCommand
```

当前不可撤销命令：

```text
LoadStepCommand
DetectFeatureCommand
ValidateShapeCommand
ExportStepCommand
```

### 6.4 Patch preview / Apply 主线

```text
1. 默认 fitting input mode 是 STP Sampled Candidate Surface。
2. 当前唯一保留的 STL 扩宽机制是 adjacent-face support collar。
3. Legacy centroid-only crop、conservative boundary-band crop 和 Global Cut Chain 保留为诊断 / 对照路线。
4. Geomagic patch outer boundary、STP sampled mesh boundary、support collar 外环和 STL crop boundary 都不能作为最终 CAD boundary。
5. Apply 使用原 STP candidate outer boundary wire / pcurve 构造 replacement。
6. Multi-surface patch 是主路径输入，patch face count > 1 不再作为 unsupported。
7. Apply 成功后才允许导出已合并 STEP 并进入外部 CAD 诊断。
8. redo 只复用缓存 afterDocument，不重新运行 Geomagic、STL generation、crop、import、repair 或 external CAD。
```

### 6.5 当前真实样例诊断事实

```text
1. Route 2 support collar 输入 STL 已验证为干净：components=1、boundaryCycles=1、effective width=0.25。
2. normalized patch 单位已正确归一到 millimeter。
3. patch 侧 CommercialCadLikeQualityGate 已明显改善。
4. 当前 blocker 不是 support collar 断开、adaptive width 被压小或单位错误。
5. 当前 blocker 是 72-face Geomagic patch 下 strict multi-surface boundary shell open-wire closure。
6. 已知失败点集中在 failed_patch_face_index=2、original-boundary edge ids 1540/1543/1546 和 endpoint gap 0.879063。
7. Creo Toolkit sewing 当前不能把失败样例救回水密实体；ModelCHECK runner 成功不等于模型验收成功。
```

---

## 7. 当前后续增强边界

### 7.1 P0：Route 2 open-wire closure 定位

当前最优先的增强方向：

```text
1. 定位 failed_patch_face_index=2 的 face edges 来源。
2. 关联 original-boundary edge 1540 / 1543 / 1546 的 split segment 参数区间和 owner face。
3. 判断 endpoint gap 0.879063 来自 patch internal seam 缺失、segment ordering 错误、owner split 错误，还是 fitted surface 局部覆盖断裂。
4. 解释为什么 pcurve rebuild 成功且 max projection distance 约 0.0108314，仍无法形成 closed wire。
5. 判断 72-face Geomagic patch 是否已经过碎，是否需要 patch 输出拒绝 / 约束或 seam selection 诊断。
```

注意：P0 不应恢复旧 guard-band / over-cover 可执行入口，不应放宽 `StrictTopologyGate`，也不应把 patch outer boundary 或 STL boundary 改成最终 CAD boundary。

### 7.2 P1：支撑带输入质量判定

```text
1. 确认 support collar 与主体 STL 拓扑连通。
2. 确认 Geomagic pre-repair components=1、boundaryCycles=1。
3. 确认 support collar effective width 不小于显式配置值。
4. 确认 normalized patch 单位仍为 millimeter。
5. 输入 STL 干净但 Apply 失败时，优先转向 strict retrim / multi-surface shell / owner split / local coverage 诊断。
```

### 7.3 P2：项目保存与报告

```text
1. 实现 ProjectSerializer。
2. 保存 .spo.json。
3. 保存源文件路径、参数、锁边、操作日志、验证报告。
4. 实现 ReportGenerator 基础报告。
5. 输出 face/edge 变化、free/multiple edge、BRepCheck、导出校验、patch apply、gate 和 external CAD routing 结果。
```

### 7.4 P3：高级特征线与重拟合

```text
1. CurvatureEstimator 实用化。
2. BoundaryClassifier 识别圆角起止线、凸起边界、凹陷边界。
3. ridge / valley / weak feature 检测。
4. SurfaceRefitter 局部 B-spline / plate surface 重拟合。
5. 局部 patch layout 重构。
```

---

## 8. 模块开发原则

后续使用 Codex 或人工开发时，应遵守以下约束：

```text
1. GUI 不直接修改 TopoDS_Shape。
2. GUI 只通过 AppController 发起业务操作。
3. AppController 不承载复杂几何算法。
4. 所有修改模型或用户约束状态的操作都应通过 Command 执行。
5. merge 模块不负责界面显示。
6. validate 模块不负责合并策略。
7. brep 模块只负责索引和查询，不负责修改模型。
8. 新增功能应优先补测试或至少给出可执行验证方式。
9. 算法增强应先生成候选和报告，再执行破坏性拓扑替换。
10. Patch Apply 不能绕过 PatchReplacementRepair、StrictTopologyGate 或 STEP roundtrip。
11. Geomagic patch outer boundary、STL crop boundary、support collar 外环都不能作为最终 CAD boundary。
12. 默认测试不得硬编码真实样例路径、candidate ordinal、Creo edge id 或 OCCT edge id。
13. redo 必须复用缓存结果，不能重新运行 Geomagic、crop、import、repair 或 external CAD。
```

---

## 9. 项目文件格式建议

后续实现 `ProjectSerializer` 时，建议保存一个项目状态文件：

```text
case_001.spo.json
```

建议结构：

```json
{
  "source_step": "case_001.step",
  "linear_tolerance": 0.001,
  "angular_tolerance_deg": 25.0,
  "curvature_tolerance": 0.1,
  "min_edge_length": 0.0,
  "locked_edges": [12, 30, 44],
  "selected_merge_groups": [[1, 2, 3], [10, 11, 12]],
  "operations": [
    {
      "type": "detect_feature_edges",
      "angle_threshold_deg": 25.0
    },
    {
      "type": "same_domain_unify",
      "face_count_before": 570,
      "face_count_after": 497,
      "edge_count_before": 2280,
      "edge_count_after": 2094
    }
  ],
  "validation": {
    "brep_check_valid": true,
    "free_edges": 0,
    "multiple_edges": 0
  }
}
```

目的：

```text
1. 保证交互过程可复现。
2. 便于后续实验记录。
3. 便于组会汇报。
4. 便于论文复现实验。
```
