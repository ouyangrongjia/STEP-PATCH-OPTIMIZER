# 分模块设计

> 文档定位：本文件作为项目的长期模块规划与架构设计文档。  
> 当前实现进度、已完成任务、待办事项、测试状态不放在本文件中，统一记录在 `implementation_status.md`。  
> 目标是让本文件保持稳定，作为后续开发、重构和汇报时的模块边界参考。

---

## 1. 总体架构

项目采用分层模块设计：

```text
GUI 层
  ↓
Application Controller
  ↓
Command / Operation 层
  ↓
Core Geometry Modules
  ├── IO
  ├── B-rep Topology
  ├── Feature Detection
  ├── Patch Merge
  └── Validation
  ↓
OCCT Kernel
```

核心原则：

```text
1. GUI 不直接修改 TopoDS_Shape。
2. GUI 只通过 AppController 发起业务操作。
3. AppController 负责组织 Command 与 Core Geometry Modules，不承载复杂几何算法。
4. 所有会修改模型或用户约束状态的操作，应通过 Command 执行。
5. IO、B-rep、Feature、Merge、Validation 模块保持职责单一。
6. OCCT 作为底层几何内核，不向 GUI 层泄露过多实现细节。
```

---

## 2. 模块总览

| 模块 | 目录 | 规划职责 |
|---|---|---|
| GUI 模块 | `src/gui/` | 三维显示、用户交互、参数配置、结果展示、上下文菜单 |
| 应用控制模块 | `src/app/` | 连接 GUI 与 Command/Core 模块，维护主流程入口 |
| 命令模块 | `src/command/` | 封装可执行操作，支持 undo/redo 与操作上下文 |
| IO 模块 | `src/io/` | STEP/STP 读写、项目文件保存与恢复 |
| B-rep 拓扑模块 | `src/brep/` | face/edge 索引、邻接图、拓扑查询、基础统计 |
| 特征线模块 | `src/feature/` | 自动特征边检测、曲率特征、拓扑异常边、用户约束整合 |
| 合并模块 | `src/merge/` | 候选区域生成、same-domain 合并、区域生长、局部重拟合 |
| 验证模块 | `src/validate/` | 合法性检查、误差评估、导出校验、报告生成 |
| 公共模块 | `src/common/` | 公共类型、配置、Result、容差参数 |
| 测试模块 | `tests/` | 单元测试、流程测试、回归测试 |

---

## 3. GUI 模块

### 3.1 目标

GUI 模块负责最终软件的可交互呈现。用户应能够在界面中完成：

```text
STEP 导入
→ 模型查看
→ face/edge 选择
→ 特征线检测与显示
→ 用户锁边/解锁边
→ 合并候选预览
→ 执行合并
→ 合法性检查
→ undo/redo
→ STEP 导出
```

### 3.2 界面布局

推荐界面结构：

```text
┌─────────────────────────────────────────────────────────────┐
│ Menu: File | View | Detect | Merge | Validate | Export      │
├───────────────┬───────────────────────────────┬─────────────┤
│ Model Tree    │                               │ Parameters  │
│ - Solid       │                               │ angle tol   │
│ - Shell       │          3D Viewer            │ linear tol  │
│ - Faces       │                               │ merge mode  │
│ - Edges       │                               │             │
├───────────────┴───────────────────────────────┴─────────────┤
│ Log / Inspect / Validation / Report                         │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 交互规划

| 鼠标/键盘操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标滚轮 | 放大 / 缩小视图 |
| 鼠标左键 | 选择 face 或 edge |
| Shift + 鼠标左键 | 多选 face 或 edge |
| Ctrl + 鼠标左键 | 从选择集中移除 face 或 edge |
| 鼠标右键 | 打开上下文菜单 |
| Ctrl + Z | 撤销最近一次可撤销编辑 |
| Ctrl + Y | 重做最近一次撤销编辑 |
| F | 切换到面选择模式 |
| E | 切换到边选择模式 |
| H | 显示 / 隐藏特征线 |
| M | 预览合并候选 |
| R | 重置视角 |
| V | 合法性检查 |

### 3.4 右键菜单规划

Edge 选择模式：

```text
锁定选中边
解锁选中边
显示边信息
隐藏边
```

Face 选择模式：

```text
显示面信息
加入合并组
移出合并组
预览合并区域
```

Candidate 选择模式：

```text
预览此候选区域
应用此候选区域
拒绝此候选区域
显示候选区域统计
```

### 3.5 核心类规划

```cpp
class MainWindow;
class OccViewWidget;
class ModelTreePanel;
class ParameterPanel;
class InspectPanel;
class LogPanel;
```

`OccViewWidget` 负责 Qt 鼠标事件与 OCCT Viewer 的桥接。

---

## 4. 应用控制模块

### 4.1 目标

应用控制模块负责协调 GUI、Command 与底层几何模块，避免 GUI 直接调用复杂几何处理逻辑。

### 4.2 核心类规划

```cpp
class AppController {
public:
    Result openStepFile(const std::filesystem::path& path);
    Result exportStepFile(const std::filesystem::path& path);

    FeatureEdgeDetectionResult detectFeatureEdges(
        double angularThresholdDegrees,
        double minEdgeLength);

    SameDomainUnifyResult unifySameDomain(
        double angularThresholdDegrees,
        double minEdgeLength,
        double linearTolerance);

    ShapeValidationReport validateShape();

    Result lockEdges(const std::vector<EdgeId>& edgeIds);
    Result unlockEdges(const std::vector<EdgeId>& edgeIds);

    Result undo();
    Result redo();

    bool canUndo() const;
    bool canRedo() const;
    bool hasDocument() const;

    const ShapeDocument& document() const;
};
```

### 4.3 职责

```text
1. 管理当前 CommandContext。
2. 管理 CommandHistory。
3. 通过 Command 封装会修改状态的操作。
4. 调用 StepReader / StepWriter。
5. 调用 FeatureEdgeDetector。
6. 调用 SameDomainUnifier / MergePlanner。
7. 调用 ShapeValidator。
8. 向 GUI 提供统一的业务入口。
```

---

## 5. 命令模块

### 5.1 目标

命令模块用于封装用户操作，并为破坏性或状态修改操作提供 undo/redo 能力。

### 5.2 核心类规划

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual const char* name() const = 0;
    virtual Result execute(CommandContext& context) = 0;

    virtual bool undoable() const { return false; }
    virtual Result undo(CommandContext& context);
    virtual Result redo(CommandContext& context);
};

struct CommandContext;
class CommandHistory;
```

### 5.3 CommandContext 规划

`CommandContext` 保存当前命令执行所需的上下文状态：

```text
1. 当前 ShapeDocument。
2. 当前特征边检测结果。
3. 当前合法性检查报告。
4. 当前源文件路径。
5. 用户锁定边集合。
6. dirty 状态。
```

### 5.4 可撤销命令与不可撤销命令

| 命令 | 是否可撤销 | 原因 |
|---|---:|---|
| `MergePatchCommand` | 是 | 修改几何模型，是破坏性编辑 |
| `LockEdgeCommand` | 是 | 修改用户约束状态 |
| `UnlockEdgeCommand` | 是 | 修改用户约束状态 |
| `LoadStepCommand` | 否 | 打开新文件是会话边界，成功后清空历史 |
| `DetectFeatureCommand` | 否 | 重新计算检测结果，不属于编辑 |
| `ValidateShapeCommand` | 否 | 只生成验证报告 |
| `ExportStepCommand` | 否 | 导出文件，不修改当前模型 |

### 5.5 Undo/Redo 设计原则

```text
1. 只有 undoable() == true 且 execute 成功的命令进入 undo 栈。
2. 执行新的可撤销命令后，应清空 redo 栈。
3. 非编辑命令不影响 undo/redo 栈。
4. MergePatchCommand 的 redo 不应重新运行几何算法，而应恢复已保存的 afterDocument。
5. 打开新 STEP 后清空 undo/redo 历史与用户锁边状态。
6. 当前语义约定：撤销 MergePatchCommand 时，清空合并后的用户锁边状态。这是有意设计，不作为缺陷处理。
```

---

## 6. IO 模块

### 6.1 目标

IO 模块负责 STEP/STP 文件读写，以及项目文件保存与恢复。

### 6.2 核心类规划

```cpp
class StepReader;
class StepWriter;
class ProjectSerializer;
```

### 6.3 StepReader 职责

```text
1. 读取 .step / .stp 文件。
2. 转换为 TopoDS_Shape。
3. 检查读取状态。
4. 获取单位信息。
5. 返回 ShapeDocument 初始化所需数据。
```

### 6.4 StepWriter 职责

```text
1. 将当前 TopoDS_Shape 写出为 STEP/STP。
2. 保留必要单位信息。
3. 写出后可重新读取验证。
```

### 6.5 ProjectSerializer 职责

项目文件建议使用：

```text
.spo.json
```

应保存：

```text
1. 源 STEP 文件路径。
2. 当前参数配置。
3. 用户锁边信息。
4. 合并历史或操作日志。
5. 最近一次验证报告。
6. GUI 可恢复状态。
```

---

## 7. B-rep 拓扑模块

### 7.1 目标

B-rep 拓扑模块负责把 OCCT 的 `TopoDS_Shape` 转换为程序内部可查询、可统计、可关联的结构。

### 7.2 核心类规划

```cpp
class ShapeDocument;
class FaceIndex;
class EdgeIndex;
class TopologyGraph;
```

### 7.3 职责

```text
1. 为每个 face 编号。
2. 为每条 edge 编号。
3. 建立 face → edges 映射。
4. 建立 edge → adjacent faces 映射。
5. 建立 face → neighbor faces 映射。
6. 支持从 TopoDS_Shape 反查 FaceId / EdgeId。
7. 支持 GUI 选择后的属性查询。
8. 提供基础统计信息。
```

### 7.4 后续可增强数据

```cpp
struct FaceInfo {
    FaceId id;
    TopoDS_Face face;
    SurfaceType surfaceType;
    double area;
    gp_Pnt center;
    gp_Dir avgNormal;
    std::vector<EdgeId> edgeIds;
};

struct EdgeInfo {
    EdgeId id;
    TopoDS_Edge edge;
    std::vector<FaceId> adjacentFaceIds;
    double length;
    bool isFreeEdge;
    bool isMultipleEdge;
    bool isFeatureEdge;
    bool isUserLocked;
};
```

---

## 8. 特征线模块

### 8.1 目标

特征线模块判断哪些边应当保留，哪些边可以被合并消除。

特征线是本项目的关键，因为合并结果是否合理，主要取决于边界是否贴合模型真实结构。

### 8.2 特征边类型规划

```cpp
enum class FeatureEdgeType {
    None,
    StrongFeature,
    WeakFeature,
    TopologicalFeature,
    UserLocked
};
```

| 类型 | 说明 | 是否允许跨越合并 |
|---|---|---|
| `None` | 普通内部 patch 边界 | 允许 |
| `StrongFeature` | 明显棱边、折角、二面角突变 | 不允许 |
| `WeakFeature` | 曲率突变或凸起边界 | 默认不允许 |
| `TopologicalFeature` | free edge、multiple edge、non-manifold edge | 不允许 |
| `UserLocked` | 用户手动冻结边 | 不允许 |

### 8.3 核心类规划

```cpp
class FeatureEdgeDetector;
class CurvatureEstimator;
class BoundaryClassifier;
class UserConstraintSet;
```

### 8.4 检测依据

```text
1. 二面角。
2. 相邻面法向变化。
3. 曲率变化。
4. 曲面类型变化。
5. free edge / multiple edge / non-manifold edge。
6. 用户手动冻结边。
```

### 8.5 基础规则

```text
如果 edge 是 free edge：
    标记为 TopologicalFeature。

如果 edge 相邻面数量 > 2：
    标记为 TopologicalFeature。

如果 edge 两侧 face 的二面角 > angle_threshold：
    标记为 StrongFeature。

如果 edge 两侧曲率变化 > curvature_threshold：
    标记为 WeakFeature。

如果 edge 被用户手动锁定：
    标记为 UserLocked。
```

---

## 9. 合并模块

### 9.1 目标

合并模块负责生成合并候选、过滤不合理候选并执行面片合并。

本项目的合并目标不是单纯减少面片数，而是在尽量保留棱边、凸起边界、圆角起止线和潮玩件造型细节的前提下，减少转换过程造成的碎片面。

### 9.2 核心类规划

```cpp
class MergeCandidate;
class MergeRegionGrower;
class MergePlanner;
class SameDomainUnifier;
class SurfaceRefitter;
```

### 9.3 合并候选类型

| 类型 | 说明 | 阶段定位 |
|---|---|---|
| 共面 face 合并 | 多个相邻平面碎片合为大平面 | 第一阶段 |
| 同圆柱面合并 | 圆柱面被切成多个条带 | 第一阶段 |
| 同球面合并 | 球面碎片合并 | 第一阶段 |
| 同圆锥面合并 | 圆锥面碎片合并 | 可选 |
| 同 B-spline 几何域合并 | 同一 B-spline surface 上的 trimmed faces 合并 | 第一阶段 |
| 近似 G1 连续区域重拟合 | 重新拟合新 B-spline surface | 后续增强 |
| 局部 patch layout 重构 | 重构局部曲面片布局 | 研究增强 |

### 9.4 MergePlanner 职责

```text
1. 从 TopologyGraph 获取相邻 face。
2. 判断共享边是否为特征边。
3. 判断共享边是否为用户锁定边。
4. 计算合并优先级或合并风险。
5. 生成候选合并区域。
6. 将候选区域传给 GUI 预览。
7. 根据用户确认生成 MergePlan。
```

### 9.5 MergeRegionGrower 职责

```text
for each unvisited face:
    create new region
    push seed face

    while queue is not empty:
        current = queue.pop()

        for each neighbor of current:
            if neighbor is visited:
                continue

            edge = shared edge between current and neighbor

            if edge is feature edge:
                continue

            if edge is user locked:
                continue

            if merge risk is too high:
                continue

            add neighbor to region
            push neighbor
```

### 9.6 SameDomainUnifier 职责

`SameDomainUnifier` 负责最可靠的一类合并：

```text
多个相邻 face 位于同一个几何域，
只是被 STEP 导出过程切成了多个 trimmed face。
```

输入：

```text
1. ShapeDocument。
2. protectedEdges，即自动特征边 + 用户锁定边。
3. linear_tolerance。
4. angular_tolerance。
```

输出：

```text
1. 合并后的 ShapeDocument。
2. 合并前后统计。
3. 被保护边数量。
```

### 9.7 SurfaceRefitter 职责

`SurfaceRefitter` 用于后续增强，不作为第一阶段核心实现。

后续用于：

```text
1. 多个近似共面 B-spline patch 合并。
2. 多个近似 G1 连续 patch 重拟合。
3. 光顺区域碎片化严重的局部重建。
```

---

## 10. 验证模块

### 10.1 目标

每次执行合并后，都必须验证模型是否仍然合法。

验证模块需要回答：

```text
1. 模型是否仍然是一个合法实体。
2. 是否产生新的 free edge / multiple edge。
3. face / edge / solid 数量是否异常变化。
4. 几何偏差是否可接受。
5. 特征边是否被错误删除。
6. 导出的 STEP 是否能重新读取。
```

### 10.2 核心类规划

```cpp
class ShapeValidator;
class SewingChecker;
class ErrorMetric;
class ReportGenerator;
```

### 10.3 基础检查内容

```text
1. 是否存在 shape。
2. 是否仍为 solid。
3. solid 数量是否异常变化。
4. shell 是否闭合。
5. 是否产生 free edge。
6. 是否产生 multiple edge。
7. OCCT BRepCheck 是否通过。
8. STEP 导出后重新读取是否成功。
```

### 10.4 后续评估指标

```text
face_count_before
face_count_after
face_reduction_ratio
edge_count_before
edge_count_after
free_edge_count
multiple_edge_count
solid_count
shell_count
max_deviation
mean_deviation
rms_deviation
feature_edge_preserve_rate
locked_edge_preserve_rate
invalid_merge_count
manual_correction_count
```

### 10.5 报告示例

```json
{
  "input_file": "toy_model.step",
  "output_file": "toy_model_optimized.step",
  "face_count_before": 2850,
  "face_count_after": 940,
  "face_reduction_ratio": 0.670,
  "edge_count_before": 5712,
  "edge_count_after": 1881,
  "free_edge_count": 0,
  "multiple_edge_count": 0,
  "solid_count": 1,
  "max_deviation": 0.012,
  "rms_deviation": 0.003,
  "feature_edge_preserve_rate": 0.983,
  "locked_edge_preserve_rate": 1.000
}
```

---

## 11. 公共模块

### 11.1 目标

公共模块保存通用类型、容差配置、日志、错误返回结构。

### 11.2 建议类型

```cpp
struct ToleranceConfig {
    double linearTol = 0.001;
    double angularTolDeg = 25.0;
    double curvatureTol = 0.1;
};

struct AlgorithmParameters {
    double angular_threshold_degrees = 25.0;
    double linear_tolerance = 0.001;
    double curvature_threshold = 0.1;
    double min_edge_length = 0.0;
};
```

### 11.3 配置项

```text
linear_tolerance
angular_tolerance_degrees
curvature_tolerance
min_edge_length
merge_mode
preserve_feature_edges
preserve_user_locked_edges
enable_same_domain_merge
enable_region_growing
enable_surface_refit
max_deviation
```

---

## 12. 模块依赖关系

推荐依赖方向：

```text
gui
 ↓
app
 ↓
command
 ↓
io / brep / feature / merge / validate
 ↓
common
 ↓
OCCT
```

约束：

```text
1. GUI 不直接修改 TopoDS_Shape。
2. GUI 只通过 AppController 发起操作。
3. app 可以组织 command，但不应承载复杂几何算法。
4. command 可以调用 core geometry modules。
5. merge 模块不负责界面显示。
6. validate 模块不负责合并策略。
7. brep 模块只负责索引和查询，不负责修改模型。
8. 所有修改模型或用户约束状态的操作都应进入 command 系统。
```

---

## 13. 阶段规划

### 13.1 MVP 阶段

目标：跑通稳定、可演示的 STEP 后处理闭环。

```text
1. STEP 读取 / 显示。
2. face / edge 选择。
3. Shift 多选。
4. 特征边检测。
5. 用户锁边 / 解锁。
6. same-domain 合并。
7. undo / redo。
8. 基础合法性检查。
9. STEP 导出。
```

### 13.2 工程增强阶段

目标：从“能用”提升到“可控、可解释、可批量验证”。

```text
1. MergePlanner 候选区域生成。
2. MergeRegionGrower 区域生长。
3. 合并候选 GUI 预览。
4. 手动合并组编辑。
5. ProjectSerializer 保存项目。
6. ErrorMetric 几何误差评估。
7. ReportGenerator 实验报告。
8. 更完整的锁边状态持久化。
```

### 13.3 研究增强阶段

目标：面向潮玩件复杂曲面和论文方向拓展。

```text
1. CurvatureEstimator 实用化。
2. BoundaryClassifier 圆角起止线、凸起边界识别。
3. ridge / valley / weak feature 检测。
4. SurfaceRefitter 局部 B-spline 重拟合。
5. 局部 patch layout 重构。
6. 学习辅助候选区域推荐。
```
