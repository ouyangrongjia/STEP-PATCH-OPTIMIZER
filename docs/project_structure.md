# 项目结构设计

项目名称建议：

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

## 1. 项目目标

本项目面向 STEP/STP 后处理场景，目标是对导出的高细分 NURBS/B-rep 曲面片进行合理合并，使曲面片边界尽可能贴合模型真实结构特征，例如棱角、棱边、凸起边界、凹陷边界和曲率突变区域。

项目最终形态是一个可交互 GUI 工具，支持：

- STEP/STP 文件导入；
- B-rep 模型三维显示；
- 面片、边界线、特征线选择；
- 特征边自动提取与手动修正；
- 曲面片合并候选预览；
- 特征约束下的曲面片合并；
- 合并后合法性检查；
- 优化后的 STEP/STP 文件导出；
- 合并前后统计报告输出。

---

## 2. 推荐技术栈

推荐主技术路线：

```text
C++20 + OCCT + Qt6 + CMake
```

辅助依赖可选：

```text
CGAL         用于 mesh 层 sharp edge / region segmentation 辅助
Eigen        用于线性代数、拟合、PCA 等计算
nlohmann/json 用于项目文件与参数保存
spdlog       用于日志
GoogleTest   用于单元测试
```

第一版不建议引入过多复杂依赖。应优先跑通：

```text
STEP 读写
→ B-rep 拓扑索引
→ GUI 可视化
→ 特征边检测
→ 同域面片合并
→ 合法性检查
→ STEP 导出
```

---

## 3. 推荐目录结构

```text
step-patch-optimizer/
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── docs/
│   ├── 01_project_structure.md
│   ├── 02_technical_route.md
│   ├── 03_module_design.md
│   ├── algorithm_design.md
│   ├── ui_design.md
│   └── experiment_protocol.md
│
├── assets/
│   ├── icons/
│   └── shaders/
│
├── data/
│   ├── samples/
│   │   ├── cube.step
│   │   ├── toy_model.step
│   │   └── oversegmented_case.step
│   └── expected/
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
│   │   ├── MergeRegionGrower.h
│   │   ├── MergeRegionGrower.cpp
│   │   ├── SameDomainUnifier.h
│   │   ├── SameDomainUnifier.cpp
│   │   ├── SurfaceRefitter.h
│   │   ├── SurfaceRefitter.cpp
│   │   ├── MergePlanner.h
│   │   └── MergePlanner.cpp
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
│   ├── command/
│   │   ├── Command.h
│   │   ├── UndoStack.h
│   │   ├── LoadStepCommand.h
│   │   ├── DetectFeatureCommand.h
│   │   ├── LockEdgeCommand.h
│   │   ├── UnlockEdgeCommand.h
│   │   ├── MergePatchCommand.h
│   │   └── ExportStepCommand.h
│   │
│   └── common/
│       ├── Config.h
│       ├── Logger.h
│       ├── Result.h
│       ├── GeometryTypes.h
│       └── Tolerance.h
│
├── tests/
│   ├── test_step_io.cpp
│   ├── test_topology_graph.cpp
│   ├── test_feature_edges.cpp
│   ├── test_same_domain_merge.cpp
│   └── test_validation.cpp
│
├── tools/
│   ├── batch_optimize.cpp
│   └── step_stats.cpp
│
└── experiments/
    ├── configs/
    ├── results/
    └── notebooks/
```

---

## 4. 目录职责说明

| 目录 | 职责 |
|---|---|
| `src/app/` | 程序入口、主窗口、应用级控制器 |
| `src/gui/` | GUI 界面、OCCT 三维视图、参数面板、日志面板、检查面板 |
| `src/io/` | STEP/STP 读写、项目文件保存与恢复 |
| `src/brep/` | B-rep 拓扑索引、Face/Edge 编号、邻接图构建 |
| `src/feature/` | 特征线提取、曲率估计、边界分类、用户约束边管理 |
| `src/merge/` | 合并候选生成、区域生长、同域合并、曲面重拟合接口 |
| `src/validate/` | 实体合法性检查、缝合检查、几何误差评估、报告生成 |
| `src/command/` | GUI 操作命令化，支持撤销与重做 |
| `src/common/` | 公共类型、日志、错误返回、容差配置 |
| `tools/` | 命令行工具，服务批量实验与统计 |
| `experiments/` | 实验配置、实验结果、论文或周报材料 |

---

## 5. 项目文件格式建议

除输出 STEP/STP 外，建议保存一个项目状态文件，例如：

```text
case_001.spo.json
```

其中记录：

```json
{
  "source_step": "case_001.step",
  "linear_tolerance": 0.01,
  "angular_tolerance_deg": 20.0,
  "locked_edges": [12, 30, 44],
  "selected_merge_groups": [[1, 2, 3], [10, 11, 12]],
  "operations": [
    {
      "type": "detect_feature_edges",
      "angle_threshold_deg": 20.0
    },
    {
      "type": "same_domain_unify",
      "face_count_before": 2850,
      "face_count_after": 940
    }
  ]
}
```

这样可以保证交互过程可复现，便于后续实验记录、组会汇报和论文复现实验。

---

## 6. 第一版工程边界

第一版建议只实现确定性较强的功能：

```text
STEP 读取
B-rep 显示
Face/Edge 索引
面片邻接图
二面角特征边检测
用户手动冻结边
same-domain 面片合并
合并后合法性检查
STEP 导出
```

第一版暂不建议做复杂 NURBS 重拟合。曲面重拟合涉及采样、边界约束、trim curve 重建、误差控制和拓扑替换，难度较高，应放在第二阶段或第三阶段。
