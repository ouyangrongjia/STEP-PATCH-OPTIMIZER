# 分模块设计

## 1. 模块总览

项目采用分层模块设计：

```text
GUI 层
  ↓
Application Controller
  ↓
Command / Undo-Redo
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

主要模块包括：

| 模块 | 目录 | 作用 |
|---|---|---|
| GUI 模块 | `src/gui/` | 三维显示、用户交互、参数配置、结果展示 |
| 应用控制模块 | `src/app/` | 连接 GUI 与几何处理模块 |
| IO 模块 | `src/io/` | STEP/STP 读写、项目状态保存 |
| B-rep 拓扑模块 | `src/brep/` | face/edge 索引、邻接图、拓扑查询 |
| 特征线模块 | `src/feature/` | 特征边检测、曲率估计、用户约束 |
| 合并模块 | `src/merge/` | 合并候选生成、同域合并、区域生长、重拟合接口 |
| 验证模块 | `src/validate/` | 合法性检查、误差评估、报告生成 |
| 命令模块 | `src/command/` | 撤销/重做、操作历史管理 |
| 公共模块 | `src/common/` | 日志、配置、公共类型、容差 |

---

# 2. GUI 模块

## 2.1 目标

GUI 模块负责最终软件的可交互呈现。用户应能够在界面中完成从 STEP 导入、模型检查、特征线编辑、面片合并、结果验证到 STEP 导出的完整流程。

## 2.2 界面布局

推荐界面结构：

```text
┌─────────────────────────────────────────────────────────────┐
│ Menu: File | View | Detect | Merge | Validate | Export      │
├───────────────┬───────────────────────────────┬─────────────┤
│ Model Tree    │                               │ Parameters  │
│ - Solid 0     │                               │ angle tol   │
│ - Shell 0     │          3D Viewer            │ linear tol  │
│ - Faces       │                               │ merge mode  │
│ - Edges       │                               │             │
├───────────────┴───────────────────────────────┴─────────────┤
│ Log / Report / Validation                                   │
└─────────────────────────────────────────────────────────────┘
```

## 2.3 必须支持的鼠标交互

| 鼠标/键盘操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标滚轮 | 放大 / 缩小视图 |
| 鼠标左键 | 选择 face 或 edge |
| Shift + 鼠标左键 | 多选 face 或 edge |
| 鼠标右键 | 打开上下文菜单 |
| Ctrl + Z | 撤销 |
| Ctrl + Y | 重做 |
| F | 聚焦选中对象 |
| H | 显示/隐藏特征线 |
| M | 预览合并候选 |

## 2.4 右键菜单

当用户选中 edge 时，右键菜单建议包含：

```text
Lock as Feature Edge
Unlock Feature Edge
Show Edge Info
Hide Edge
```

当用户选中 face 时，右键菜单建议包含：

```text
Show Face Info
Add to Merge Group
Remove from Merge Group
Preview Merge Region
```

## 2.5 核心类

```cpp
class MainWindow;
class OccViewWidget;
class ModelTreePanel;
class ParameterPanel;
class InspectPanel;
class LogPanel;
```

其中，`OccViewWidget` 负责 OCCT Viewer 与 Qt 鼠标事件的桥接。

## 2.6 OccViewWidget 职责

`OccViewWidget` 应负责：

```text
1. 初始化 OCCT Viewer
2. 显示 AIS_Shape
3. 支持鼠标中键旋转视角
4. 支持鼠标滚轮缩放视图
5. 支持鼠标左键选择 face / edge
6. 支持高亮特征线
7. 支持高亮候选合并区域
8. 支持视图重置、适应窗口、正交/透视切换
```

鼠标事件逻辑建议：

```cpp
void OccViewWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        beginRotation(event->pos());
    } else if (event->button() == Qt::LeftButton) {
        selectShapeAt(event->pos());
    } else if (event->button() == Qt::RightButton) {
        showContextMenu(event->pos());
    }
}

void OccViewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (isRotating_) {
        rotateView(event->pos());
    }
}

void OccViewWidget::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() > 0) {
        zoomIn(event->position());
    } else {
        zoomOut(event->position());
    }
}
```

---

# 3. 应用控制模块

## 3.1 目标

应用控制模块负责协调 GUI 与底层算法模块，避免 GUI 直接调用大量几何处理逻辑。

## 3.2 核心类

```cpp
class AppController {
public:
    void openStepFile(const std::string& path);
    void detectFeatureEdges(const FeatureDetectOptions& options);
    void previewMergeCandidates(const MergeOptions& options);
    void applyMerge(const MergePlan& plan);
    void validateCurrentShape();
    void exportStepFile(const std::string& path);
};
```

## 3.3 职责

```text
1. 管理当前 ShapeDocument
2. 调用 StepReader / StepWriter
3. 调用 FeatureEdgeDetector
4. 调用 MergePlanner / SameDomainUnifier
5. 调用 ShapeValidator
6. 通知 GUI 刷新显示
7. 记录操作日志
8. 将操作封装为 Command，支持撤销/重做
```

---

# 4. IO 模块

## 4.1 目标

IO 模块负责 STEP/STP 文件读写，以及项目文件保存与恢复。

## 4.2 核心类

```cpp
class StepReader;
class StepWriter;
class ProjectSerializer;
```

## 4.3 StepReader

职责：

```text
1. 读取 .step / .stp 文件
2. 转换为 TopoDS_Shape
3. 检查读取状态
4. 获取单位信息
5. 返回 ShapeDocument 初始化所需数据
```

接口示例：

```cpp
class StepReader {
public:
    Result<TopoDS_Shape> read(const std::string& filePath);
};
```

## 4.4 StepWriter

职责：

```text
1. 将当前 TopoDS_Shape 写出为 STEP/STP
2. 保留必要单位信息
3. 写出后可重新读取验证
```

接口示例：

```cpp
class StepWriter {
public:
    Result<void> write(const TopoDS_Shape& shape, const std::string& filePath);
};
```

## 4.5 ProjectSerializer

职责：

```text
1. 保存当前项目状态
2. 保存用户冻结边
3. 保存参数配置
4. 保存合并历史
5. 恢复上次编辑状态
```

建议项目文件格式为：

```text
.spo.json
```

---

# 5. B-rep 拓扑模块

## 5.1 目标

B-rep 拓扑模块负责把 OCCT 的 `TopoDS_Shape` 转换为程序内部可查询、可统计、可关联的结构。

## 5.2 核心类

```cpp
class ShapeDocument;
class FaceIndex;
class EdgeIndex;
class TopologyGraph;
```

## 5.3 核心数据结构

```cpp
struct FaceInfo {
    int id;
    TopoDS_Face face;
    SurfaceType surfaceType;
    double area;
    gp_Pnt center;
    gp_Dir avgNormal;
    std::vector<int> edgeIds;
};

struct EdgeInfo {
    int id;
    TopoDS_Edge edge;
    std::vector<int> adjacentFaceIds;
    double length;
    bool isFreeEdge;
    bool isMultipleEdge;
    bool isFeatureEdge;
    bool isUserLocked;
};

struct FaceAdjacency {
    int faceA;
    int faceB;
    int sharedEdgeId;
    double dihedralAngleDeg;
    double curvatureJump;
    double mergeCost;
    bool canMerge;
};
```

## 5.4 职责

```text
1. 为每个 face 编号
2. 为每条 edge 编号
3. 建立 face → edges 映射
4. 建立 edge → adjacent faces 映射
5. 建立 face → neighbor faces 映射
6. 计算 face 面积、中心点、平均法向
7. 计算 edge 长度、相邻面数量、拓扑类型
8. 为 GUI 提供 face/edge 选择后的属性查询能力
```

---

# 6. 特征线模块

## 6.1 目标

特征线模块判断哪些边应当保留，哪些边可以被合并消除。

特征线是本项目的关键，因为合并结果是否合理，主要取决于边界是否能贴合模型真实结构。

## 6.2 特征边类型

建议定义：

```cpp
enum class FeatureEdgeType {
    None,
    StrongFeature,
    WeakFeature,
    TopologicalFeature,
    UserLocked
};
```

含义：

| 类型 | 说明 | 是否允许跨越合并 |
|---|---|---|
| `None` | 普通内部 patch 边界 | 允许 |
| `StrongFeature` | 明显棱边、折角、二面角突变 | 不允许 |
| `WeakFeature` | 曲率突变或凸起边界 | 默认不允许 |
| `TopologicalFeature` | free edge、multiple edge、non-manifold edge | 不允许 |
| `UserLocked` | 用户手动冻结边 | 不允许 |

## 6.3 核心类

```cpp
class FeatureEdgeDetector;
class CurvatureEstimator;
class BoundaryClassifier;
class UserConstraintSet;
```

## 6.4 检测依据

特征线检测依据包括：

```text
1. 二面角
2. 相邻面法向变化
3. 曲率变化
4. 曲面类型变化
5. 拓扑异常边
6. 用户手动冻结边
```

## 6.5 基础规则

```text
如果 edge 是 free edge：
    标记为 TopologicalFeature

如果 edge 相邻面数量 > 2：
    标记为 TopologicalFeature

如果 edge 两侧 face 的二面角 > angle_threshold：
    标记为 StrongFeature

如果 edge 两侧曲率变化 > curvature_threshold：
    标记为 WeakFeature

如果 edge 被用户手动锁定：
    标记为 UserLocked
```

---

# 7. 合并模块

## 7.1 目标

合并模块负责生成合并候选、过滤不合理候选并执行面片合并。

## 7.2 核心类

```cpp
class MergeCandidate;
class MergeRegionGrower;
class MergePlanner;
class SameDomainUnifier;
class SurfaceRefitter;
```

## 7.3 合并候选类型

| 类型 | 说明 | 第一版是否实现 |
|---|---|---|
| 共面 face 合并 | 多个相邻平面碎片合为大平面 | 是 |
| 同圆柱面合并 | 圆柱面被切成多个条带 | 是 |
| 同球面合并 | 球面碎片合并 | 是 |
| 同圆锥面合并 | 圆锥面碎片合并 | 可选 |
| 同 B-spline 几何域合并 | 同一 B-spline surface 上的 trimmed faces 合并 | 是 |
| 近似 G1 连续区域重拟合 | 重新拟合新 B-spline surface | 后续阶段 |

## 7.4 MergePlanner

职责：

```text
1. 从 TopologyGraph 获取相邻 face
2. 判断共享边是否为特征边
3. 计算合并代价
4. 生成候选合并区域
5. 将候选区域传给 GUI 预览
6. 根据用户确认生成 MergePlan
```

## 7.5 MergeRegionGrower

区域生长逻辑：

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

            if merge_cost(current, neighbor) > threshold:
                continue

            add neighbor to region
            push neighbor
```

## 7.6 SameDomainUnifier

负责最可靠的合并：

```text
多个相邻 face 位于同一个几何域，
只是被 STEP 导出过程切成了多个 trimmed face。
```

流程：

```text
输入 TopoDS_Shape
输入 frozen_edges
设置 linear_tolerance
设置 angular_tolerance
调用 OCCT same-domain unify
输出 unified_shape
重新构建拓扑索引
执行合法性检查
```

接口示例：

```cpp
class SameDomainUnifier {
public:
    Result<TopoDS_Shape> run(
        const TopoDS_Shape& input,
        const std::vector<TopoDS_Edge>& frozenEdges,
        double linearTol,
        double angularTol
    );
};
```

## 7.7 SurfaceRefitter

第一版只保留接口，不作为核心实现。

后续用于：

```text
1. 多个近似共面 B-spline patch 合并
2. 多个近似 G1 连续 patch 重拟合
3. 光顺区域碎片化严重的局部重建
```

难点包括：

```text
1. 外边界 loop 提取
2. 原始区域采样
3. 曲面参数化
4. B-spline surface 拟合
5. trim curve 重建
6. 局部替换
7. sewing
8. 误差评估
```

---

# 8. 验证模块

## 8.1 目标

每次执行合并后，都必须验证模型是否仍然合法。

## 8.2 核心类

```cpp
class ShapeValidator;
class SewingChecker;
class ErrorMetric;
class ReportGenerator;
```

## 8.3 检查内容

```text
1. 是否仍为 solid
2. solid 数量是否异常变化
3. shell 是否闭合
4. 是否产生 free edge
5. 是否产生 multiple edge
6. 是否产生 non-manifold edge
7. 是否产生自交
8. 合并前后几何偏差是否超阈值
9. 特征边是否被错误删除
10. STEP 导出后重新读取是否成功
```

## 8.4 报告指标

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
feature_edge_preserve_rate
invalid_merge_count
manual_correction_count
```

## 8.5 报告示例

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
  "mean_deviation": 0.003,
  "feature_edge_preserve_rate": 0.983
}
```

---

# 9. 命令模块

## 9.1 目标

命令模块用于支持 GUI 中的撤销和重做。

## 9.2 核心类

```cpp
class Command {
public:
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual ~Command() = default;
};

class UndoStack;
class LoadStepCommand;
class DetectFeatureCommand;
class LockEdgeCommand;
class UnlockEdgeCommand;
class MergePatchCommand;
class ExportStepCommand;
```

## 9.3 应封装为命令的操作

```text
1. 打开 STEP
2. 检测特征线
3. 锁定 edge
4. 解锁 edge
5. 创建合并组
6. 应用合并
7. 回滚合并
8. 导出 STEP
```

---

# 10. 公共模块

## 10.1 目标

公共模块保存通用类型、容差配置、日志、错误返回结构。

## 10.2 建议类型

```cpp
struct ToleranceConfig {
    double linearTol = 0.01;
    double angularTolDeg = 20.0;
    double curvatureTol = 0.1;
};

template <typename T>
class Result {
public:
    bool ok() const;
    const T& value() const;
    const std::string& error() const;
};
```

## 10.3 配置项

```text
linear_tolerance
angular_tolerance_deg
curvature_tolerance
merge_cost_threshold
max_deviation
enable_same_domain_merge
enable_region_growing
enable_surface_refit
```

---

# 11. 模块依赖关系

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
1. GUI 不直接修改 TopoDS_Shape
2. GUI 只通过 AppController 发起操作
3. merge 模块不负责界面显示
4. validate 模块不负责合并策略
5. brep 模块只负责索引和查询，不负责修改模型
6. 所有修改模型的操作都应进入 command 系统
```

---

# 12. 第一版模块优先级

第一版优先级：

```text
P0:
- StepReader
- StepWriter
- OccViewWidget
- ShapeDocument
- FaceIndex
- EdgeIndex
- TopologyGraph

P1:
- FeatureEdgeDetector
- UserConstraintSet
- SameDomainUnifier
- ShapeValidator
- SewingChecker

P2:
- MergePlanner
- MergeRegionGrower
- ReportGenerator
- UndoStack

P3:
- CurvatureEstimator
- BoundaryClassifier
- SurfaceRefitter
```

第一版应以 P0 和 P1 为主。  
P2 可以逐步补充。  
P3 属于后续研究和增强方向。
