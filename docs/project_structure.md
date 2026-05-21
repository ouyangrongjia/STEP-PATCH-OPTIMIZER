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

当前项目已经完成 MVP 基础闭环，具备可演示的 STEP 后处理流程：

```text
STEP/STP 读取
→ B-rep 拓扑索引构建
→ OCCT Viewer 显示
→ face / edge 选择与多选
→ 基础特征边检测
→ 用户锁边 / 解锁边
→ same-domain 曲面片合并
→ undo / redo
→ 基础合法性检查
→ STEP/STP 导出与二次读取校验
```

当前阶段已经从“搭建项目结构与 MVP 主链路”转入“工程增强阶段”。后续重点是：

```text
1. MergeCandidate / MergePlanner / MergeRegionGrower 合并候选规划。
2. 合并候选区域 GUI 预览与人工确认。
3. ProjectSerializer 项目状态保存与恢复。
4. ErrorMetric / ReportGenerator 实验指标和报告输出。
5. CurvatureEstimator / BoundaryClassifier 高级特征线增强。
6. SurfaceRefitter 局部自由曲面重拟合。
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
│   ├── project_structure.md
│   └── run_gui.md
│
├── scripts/
│   ├── bootstrap_windows.ps1
│   ├── setup_deps.ps1
│   ├── configure.ps1
│   ├── build_debug.ps1
│   ├── run_gui.ps1
│   └── test.ps1
│
├── src/
│   ├── app/
│   │   ├── main.cpp
│   │   ├── MainWindow.h
│   │   ├── MainWindow.cpp
│   │   ├── AppController.h
│   │   └── AppController.cpp
│   │
│   ├── gui/
│   │   ├── GuiTypes.h
│   │   ├── OccViewWidget.h
│   │   ├── OccViewWidget.cpp
│   │   ├── ModelTreePanel.h
│   │   ├── ModelTreePanel.cpp
│   │   ├── ParameterPanel.h
│   │   ├── ParameterPanel.cpp
│   │   ├── InspectPanel.h
│   │   ├── InspectPanel.cpp
│   │   ├── LogPanel.h
│   │   └── LogPanel.cpp
│   │
│   ├── command/
│   │   ├── Command.h
│   │   ├── CommandContext.h
│   │   ├── CommandHistory.h
│   │   ├── CommandHistory.cpp
│   │   ├── LoadStepCommand.h
│   │   ├── LoadStepCommand.cpp
│   │   ├── DetectFeatureCommand.h
│   │   ├── DetectFeatureCommand.cpp
│   │   ├── LockEdgeCommand.h
│   │   ├── LockEdgeCommand.cpp
│   │   ├── UnlockEdgeCommand.h
│   │   ├── UnlockEdgeCommand.cpp
│   │   ├── MergePatchCommand.h
│   │   ├── MergePatchCommand.cpp
│   │   ├── ValidateShapeCommand.h
│   │   ├── ValidateShapeCommand.cpp
│   │   ├── ExportStepCommand.h
│   │   ├── ExportStepCommand.cpp
│   │   ├── LockedEdgeRef.h
│   │   └── LockedEdgeRef.cpp
│   │
│   ├── io/
│   │   ├── StepReader.h
│   │   ├── StepReader.cpp
│   │   ├── StepWriter.h
│   │   ├── StepWriter.cpp
│   │   ├── ProjectSerializer.h
│   │   └── ProjectSerializer.cpp
│   │
│   ├── brep/
│   │   ├── ShapeDocument.h
│   │   ├── ShapeDocument.cpp
│   │   ├── FaceIndex.h
│   │   ├── FaceIndex.cpp
│   │   ├── EdgeIndex.h
│   │   ├── EdgeIndex.cpp
│   │   ├── TopologyGraph.h
│   │   └── TopologyGraph.cpp
│   │
│   ├── feature/
│   │   ├── FeatureEdgeDetector.h
│   │   ├── FeatureEdgeDetector.cpp
│   │   ├── CurvatureEstimator.h
│   │   ├── CurvatureEstimator.cpp
│   │   ├── BoundaryClassifier.h
│   │   ├── BoundaryClassifier.cpp
│   │   ├── UserConstraintSet.h
│   │   └── UserConstraintSet.cpp
│   │
│   ├── merge/
│   │   ├── MergeCandidate.h
│   │   ├── MergeCandidate.cpp
│   │   ├── MergePlanner.h
│   │   ├── MergePlanner.cpp
│   │   ├── MergeRegionGrower.h
│   │   ├── MergeRegionGrower.cpp
│   │   ├── SameDomainUnifier.h
│   │   ├── SameDomainUnifier.cpp
│   │   ├── SurfaceRefitter.h
│   │   └── SurfaceRefitter.cpp
│   │
│   ├── validate/
│   │   ├── ShapeValidator.h
│   │   ├── ShapeValidator.cpp
│   │   ├── SewingChecker.h
│   │   ├── SewingChecker.cpp
│   │   ├── ErrorMetric.h
│   │   ├── ErrorMetric.cpp
│   │   ├── ReportGenerator.h
│   │   └── ReportGenerator.cpp
│   │
│   └── common/
│       ├── Config.h
│       ├── GeometryTypes.h
│       ├── Result.h
│       └── Tolerance.h
│
├── tests/
│   ├── test_commands.cpp
│   ├── test_feature_edges.cpp
│   ├── test_same_domain_merge.cpp
│   ├── test_step_io.cpp
│   ├── test_topology_graph.cpp
│   └── test_validation.cpp
│
└── tools/
    ├── batch_optimize.cpp
    └── step_stats.cpp
```

---

## 4. 文档职责

| 文档 | 作用 |
|---|---|
| `README.md` | 面向新用户：项目简介、环境配置、构建、运行、测试、常见问题 |
| `docs/module_design.md` | 长期架构文档：模块边界、依赖关系、阶段规划 |
| `docs/implementation_status.md` | 当前实现进度：已完成、待办、验收方式、近期开发顺序 |
| `docs/run_gui.md` | GUI 使用文档：启动方式、操作方式、快捷键、手动验证流程 |
| `docs/project_structure.md` | 目录结构文档：当前文件树、目录职责、开发边界 |

开发时的文档优先级：

```text
架构边界：module_design.md
当前任务：implementation_status.md
构建运行：README.md / run_gui.md
目录职责：project_structure.md
```

---

## 5. 目录职责说明

| 目录 | 当前职责 |
|---|---|
| `src/app/` | 程序入口、主窗口、应用级控制器；连接 GUI、Command 和几何模块 |
| `src/gui/` | Qt/OCCT GUI、三维视图、模型树、参数面板、日志/检查/验证/报告面板 |
| `src/command/` | 命令系统、CommandContext、CommandHistory、undo/redo、锁边命令、合并命令、导入导出命令 |
| `src/io/` | STEP/STP 读写、导出后二次读取校验、项目文件保存恢复接口 |
| `src/brep/` | ShapeDocument、Face/Edge 索引、TopologyGraph、拓扑查询、基础统计 |
| `src/feature/` | 基础特征边检测、曲率估计接口、边界分类接口、用户约束接口 |
| `src/merge/` | same-domain 合并、合并候选结构、候选规划、区域生长、曲面重拟合接口 |
| `src/validate/` | BRepCheck、free/multiple edge 检查、误差评估接口、报告生成接口 |
| `src/common/` | 公共类型、配置、容差、Result 返回结构 |
| `tests/` | 单元测试、命令测试、流程回归测试 |
| `tools/` | 命令行批处理与统计工具 |
| `scripts/` | Windows 环境初始化、依赖安装、配置、构建、运行、测试脚本 |
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
10. 基础报告、检查、验证信息展示。
```

### 6.2 几何主流程

```text
1. STEP/STP 读取。
2. B-rep 拓扑索引构建。
3. ShapeDocument 维护当前模型。
4. 基于二面角的 sharp edge 检测。
5. free edge / multiple edge 检测。
6. 用户锁边进入 protectedEdges。
7. SameDomainUnifier 执行基础同域合并。
8. ShapeValidator 执行基础合法性检查。
9. StepWriter 导出 STEP。
10. 导出后二次读取校验。
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
9. ValidateShapeCommand。
10. ExportStepCommand。
11. LockedEdgeRef，用于锁边几何签名和合并后重映射。
```

当前可撤销命令：

```text
LockEdgeCommand
UnlockEdgeCommand
MergePatchCommand
```

当前不可撤销命令：

```text
LoadStepCommand
DetectFeatureCommand
ValidateShapeCommand
ExportStepCommand
```

---

## 7. 当前后续增强边界

### 7.1 P1：合并候选规划

当前最优先的增强方向：

```text
1. 完善 MergeCandidate 数据结构。
2. 实现 MergePlanner 基础候选生成。
3. 实现 MergeRegionGrower 基础区域生长。
4. 生成近似共面候选区域。
5. GUI 中显示候选区域统计。
6. 后续再做候选区域 3D 高亮。
```

注意：P1 初期只生成候选区域，不应立即替换 B-rep 模型。

### 7.2 P2：项目保存与报告

```text
1. 实现 ProjectSerializer。
2. 保存 .spo.json。
3. 保存源文件路径、参数、锁边、操作日志、验证报告。
4. 实现 ReportGenerator 基础报告。
5. 输出 face/edge 变化、free/multiple edge、BRepCheck、导出校验结果。
```

### 7.3 P3：高级特征线与重拟合

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
