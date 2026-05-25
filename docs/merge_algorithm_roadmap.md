# 合并算法路线图

> 文档定位：本文件用于指导 STEP-PATCH-OPTIMIZER 的 STEP/STP 曲面片合并算法阶段推进。  
> 架构边界遵循 `docs/module_design.md`。  
> 当前实现进度、待办状态与验收记录放在 `docs/implementation_status.md`。  
> 本文件只描述合并算法路线、阶段目标、实现边界、输入输出和验收标准。

---

## 1. 当前问题

当前项目已经完成 MVP 工程闭环：

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
→ STEP/STP 导出
```

当前合并能力主要依赖 `SameDomainUnifier`：

```text
FeatureEdgeDetector
+ 用户 locked edges
→ protectedEdges
→ ShapeUpgrade_UnifySameDomain
```

该方案只适合合并同一底层几何域上的相邻 faces。  
对于视觉上近似连续、但底层并非同一个 `Geom_Surface` 的碎片面，合并效果有限。

当前典型问题：

```text
1. face reduction 不到 10%～15% 时，继续调 same-domain 参数的收益有限。
2. concat_bsplines=true 主要减少部分 edge，不一定减少 face。
3. MergePlanner / MergeRegionGrower 已进入候选区域生成阶段。
4. 候选区域已经可以 GUI 高亮预览，但仍未参与真实合并。
5. 解析基础图元真实 region merge 尚未实现。
6. 潮玩模型常见自由曲面碎片化尚未解决。
7. SurfaceRefitter 暂不具备直接落地条件。
```

---

## 2. 总体算法路线

合并算法按以下阶段推进：

```text
Stage 1: SameDomainUnifier Enhancement
         增强当前同域合并的参数、报告和实验能力。
         关键实验点是暴露 concat_bsplines 参数，并验证 concat_bsplines=true 的效果。

Stage 2: Generic Merge Candidate Framework
         实现通用候选区域框架，不绑定具体几何类型。
         第一版只生成候选 region，不修改 B-rep。

Stage 2.5: Candidate GUI Preview
         在 3D Viewer 中高亮候选区域，用于人工检查候选质量。
         支持候选浏览、筛选、全部显示开关和清除预览。

Stage 2.6: Candidate Selection / Rejection
         支持用户选择、接受、拒绝、隐藏、恢复候选区域。
         仍不执行真实 B-rep 合并。

Stage 3: Analytic Primitive Region Merge
         解析基础图元真实合并总阶段：
         Stage 3-0 Analytic RegionMerger Framework Preparation，可选
         Stage 3A PlaneRegionMerge
         Stage 3B CylinderRegionMerge
         Stage 3C ConeRegionMerge
         Stage 3D SphereRegionMerge
         Stage 3E TorusRegionMerge，可选。

Stage 4: Freeform Candidate Detection
         提前识别潮玩模型常见自由曲面候选区域，但不立即重拟合。

Stage 5: Freeform B-spline / Plate Refit
         对高可信自由曲面候选区域做局部重拟合和拓扑替换。
```

核心原则：

```text
1. 先候选，后修改。
2. 先可视化，后拓扑替换。
3. 先解析几何，后自由曲面重拟合。
4. 平面只是第一个低风险子任务，不是合并算法边界。
5. 基础图元 Plane / Cylinder / Cone / Sphere / Torus 属于 Stage 3 的同一类解析图元合并任务。
6. 自由曲面是潮玩模型的核心问题，应在 Plane baseline 后尽早进入候选检测。
7. 不跨越自动特征边和用户锁定边。
8. 每次真正修改 B-rep 后都必须进行合法性检查。
9. 任何高风险合并都必须支持 rollback 或 undo。
10. 所有破坏性几何修改必须通过 Command 层执行。
```

---

## 3. Stage 1：SameDomainUnifier Enhancement

### 3.1 目标

在不改变整体架构的前提下，提升当前 same-domain 合并的可控性、可解释性和可实验性。

该阶段是对当前已有能力的增强，不解决近似曲面合并问题。

Stage 1 的关键实验点是 `concat_bsplines`。当前代码中该参数默认为 `false`，same-domain 合并偏保守。本阶段应将其暴露为可配置参数，并重点验证 `concat_bsplines=true` 是否能在不破坏 BRepCheck 和 STEP 二次读取的前提下提升 edge/face reduction。

### 3.2 输入

```text
ShapeDocument
FeatureEdgeDetectionResult
lockedEdges
linear_tolerance
angular_tolerance
min_edge_length
concat_bsplines
```

### 3.3 输出

```text
SameDomainUnifyResult
- before stats
- after stats
- protected edge count
- face reduction ratio
- edge reduction ratio
- concat_bsplines actual value
- parameter snapshot
```

### 3.4 concat_bsplines 参数策略

当前语义：

```text
SameDomainUnifyOptions::concat_bsplines = false
```

Stage 1 要求：

```text
1. 不再把 concat_bsplines 隐藏为内部固定默认值。
2. 将 concat_bsplines 暴露为可配置参数。
3. 参数链路应贯穿：
   ParameterPanel
   → AlgorithmParameters
   → AppController
   → MergePatchCommand
   → SameDomainUnifier
   → ShapeUpgrade_UnifySameDomain
4. GUI 参数面板应提供“Concat B-splines / 连接 B-spline 边”复选框。
5. 合并报告中必须输出本次实际使用的 concat_bsplines=true/false。
```

推荐实验配置：

```text
保守模式：
  concat_bsplines = false

增强模式：
  concat_bsplines = true
```

推荐默认策略：

```text
1. 底层 SameDomainUnifyOptions 默认值可继续保持 false，保证旧行为稳定。
2. GUI 或实验配置可以优先勾选 true，用于验证增强效果。
3. 如果 concat_bsplines=true 在多个样例中稳定通过 BRepCheck 和 STEP 二次读取，可再考虑将 GUI 默认值切换为 true。
```

注意：

```text
不要简单硬编码 concat_bsplines=true。
正确做法是暴露参数，并把 true 作为 Stage 1 的重点实验配置。
```

### 3.5 实现内容

```text
1. 将 SameDomainUnifyOptions::concat_bsplines 从内部固定值改为可配置参数。
2. 允许 GUI / 命令 / 测试切换 true / false。
3. 在报告中记录本次合并使用的 concat_bsplines 值。
4. 对同一 STEP 样例分别运行 concat_bsplines=false 和 concat_bsplines=true，比较：
   - face_count_before / face_count_after
   - edge_count_before / edge_count_after
   - face_reduction_ratio
   - edge_reduction_ratio
   - protected_edge_count
   - BRepCheck 是否通过
   - 导出后二次读取是否通过
5. 不改变现有 MergePatchCommand 的主执行语义。
```

### 3.6 不做内容

```text
1. 不重建 face。
2. 不做区域生长。
3. 不做近似共面合并。
4. 不做 Cylinder / Cone / Sphere / Torus 合并。
5. 不做自由曲面重拟合。
6. 不实现 MergePlanner / MergeRegionGrower。
7. 不实现 PlaneRegionMerge。
```

### 3.7 验收标准

```text
1. 现有测试全部通过。
2. concat_bsplines=false 和 concat_bsplines=true 两条路径都有测试或可重复手动验证。
3. 执行合并后 BRepCheck 通过。
4. 导出 STEP 后二次读取通过。
5. 报告中能看到：
   - concat_bsplines 实际值
   - protectedEdges 数
   - face reduction ratio
   - edge reduction ratio
   - 参数快照
6. same-domain 合并行为保持稳定。
```

---

## 4. Stage 2：Generic Merge Candidate Framework

### 4.1 目标

实现和几何类型无关的候选区域框架。

这一阶段只回答：

```text
哪些 face 理论上可能被合并？
候选区域属于哪种几何类型？
为什么可以作为候选？
为什么被拒绝？
候选区域的风险有多高？
```

Stage 2 只生成候选，不修改 B-rep 模型。

### 4.2 核心类

```cpp
class MergeCandidate;
class MergePlanner;
class MergeRegionGrower;
```

建议候选类型：

```cpp
enum class MergeCandidateType {
    SameDomain,
    PlaneLike,
    CylinderLike,
    ConeLike,
    SphereLike,
    TorusLike,
    FreeformG1,
    FreeformG2,
    Unknown
};
```

建议风险等级：

```cpp
enum class MergeRiskLevel {
    Low,
    Medium,
    High
};
```

### 4.3 MergeCandidate 数据结构

`MergeCandidate` 至少包含：

```text
candidate_id
candidate_type
risk_level
status

faces
internal_edges
boundary_edges
blocked_edges
protected_edges

total_area
face_count
internal_edge_count
boundary_edge_count

fit_error
max_distance
mean_distance
max_normal_angle_deg
mean_normal_angle_deg
curvature_deviation

valid
reject_reason
```

候选类型不要只支持 Plane。即使第一版只生成 PlaneLike，也必须预留 CylinderLike、SphereLike、FreeformG1 等类型，避免后续重构。

### 4.4 MergePlanner 职责

```text
1. 接收 ShapeDocument、FeatureEdgeDetectionResult、lockedEdges、MergePlannerOptions。
2. 汇总 protectedEdges = 自动特征边 + 用户锁定边。
3. 调用 MergeRegionGrower 生成候选区域。
4. 对候选区域做基础过滤。
5. 输出 MergePlannerResult。
6. 不修改 ShapeDocument。
7. 不负责 GUI 显示。
```

### 4.5 MergeRegionGrower 职责

通用区域生长框架：

```text
for each unvisited face:
    create candidate region from seed face

    while queue is not empty:
        current = queue.pop()

        for each neighbor of current:
            shared_edge = edge between current and neighbor

            if shared_edge is feature edge:
                reject and record blocked edge

            if shared_edge is user locked:
                reject and record blocked edge

            if geometry compatibility fails:
                reject and record reason

            add neighbor to region
```

### 4.6 Stage 2 初期允许只启用 PlaneLike 检测

为了降低风险，Stage 2 初期可以只实现 PlaneLike candidate，但框架必须保持通用。

PlaneLike 判断条件：

```text
1. seed face 可估计 plane normal。
2. neighbor face 平均法向与 seed normal 夹角 < max_normal_angle_deg。
3. neighbor face center 到 seed plane 距离 < max_plane_distance。
4. shared edge 不是 protected edge。
5. region face 数 >= min_region_faces。
```

### 4.7 推荐接口

```cpp
struct MergePlannerOptions {
    double max_normal_angle_degrees = 5.0;
    double max_plane_distance = 0.01;
    double max_cylinder_radius_delta = 0.01;
    double max_sphere_radius_delta = 0.01;
    int min_region_faces = 2;
    bool enable_plane_candidates = true;
    bool enable_cylinder_candidates = false;
    bool enable_sphere_candidates = false;
    bool enable_freeform_candidates = false;
};

struct MergePlannerResult {
    std::vector<MergeCandidate> candidates;
    int visited_faces = 0;
    int rejected_regions = 0;
    int protected_edge_count = 0;
};
```

### 4.8 不做内容

```text
1. 不替换 B-rep。
2. 不删除 face。
3. 不导出新 STEP。
4. 不做 SurfaceRefitter。
5. 不做 PlaneRegionMerge。
6. 不做 Cylinder / Cone / Sphere / Torus 的真正拓扑替换。
7. 不做候选接受/拒绝状态持久化。
```

### 4.9 验收标准

```text
1. 点击“预览合并”能输出候选区域数量。
2. 报告中能看到最大候选区域 face 数、总候选 face 数。
3. 报告中能看到候选类型统计，例如 PlaneLike 数量。
4. 执行预览不会改变 face/edge 数。
5. protectedEdges 能阻断区域生长。
6. lockedEdges 能阻断区域生长。
7. 新增测试覆盖 MergePlanner 和 MergeRegionGrower。
```

---

## 5. Stage 2.5：Candidate GUI Preview

### 5.1 目标

在不修改 B-rep 模型的前提下，将 MergePlanner 生成的候选区域在 3D Viewer 中高亮显示，让用户能够直观看到候选区域的位置、大小和边界。

这一阶段回答：

```text
候选区域到底在哪里？
候选区域是否跨越了不该跨越的结构？
候选区域是否过大、过碎或误判？
阈值设置是否合理？
```

### 5.2 是否应该默认高亮所有候选区域

不建议无条件默认高亮所有候选区域。

更严谨的策略是：

```text
1. 支持“显示全部候选区域”开关。
2. 默认只高亮 Top N 候选区域，或默认高亮当前选中的候选区域。
3. 报告面板显示全部候选统计。
4. Viewer 高亮显示应可控、可清除、可筛选。
```

原因：

```text
1. 候选区域数量可能很多，全部高亮会造成视觉噪声。
2. 多个候选区域可能相邻或重叠，全部显示会让边界难以判断。
3. 大模型上全部高亮可能带来明显渲染开销。
4. 用户真正需要的是定位和检查候选质量，而不是一次性把屏幕染满。
5. 若所有候选使用同一颜色，无法区分候选之间的边界；若使用多色，颜色管理和图例又会增加复杂度。
```

### 5.3 推荐交互

```text
1. 点击“预览合并”后：
   - 生成 candidates
   - 报告面板输出候选统计
   - Viewer 默认高亮最大候选区域或前 N 个候选区域

2. 报告面板或候选列表中可以选择某个 candidate：
   - Viewer 高亮该 candidate 的 faces
   - 其他 candidate 淡化或隐藏

3. 提供显示策略：
   - 仅显示当前候选
   - 显示 Top N 候选
   - 显示全部候选
   - 清除候选预览
```

### 5.4 实现内容

```text
1. AppController 保存或返回最近一次 MergePlannerResult。
2. MainWindow::previewMergeCandidates() 将候选 faces 传给 OccViewWidget。
3. OccViewWidget 新增候选区域高亮能力。
4. 支持按 candidate_id 高亮候选区域。
5. 支持清除候选高亮。
6. 支持至少一种候选显示策略：
   - 最大候选
   - Top N 候选
   - 全部候选
7. 高亮只用于预览，不改变 ShapeDocument。
8. applyMerge() 仍然走 SameDomainUnifier。
```

### 5.5 不做内容

```text
1. 不接受/拒绝候选。
2. 不持久化候选状态。
3. 不执行 PlaneRegionMerge。
4. 不替换 B-rep。
5. 不实现 SurfaceRefitter。
6. 不修改 applyMerge() 的 same-domain 合并路径。
```

### 5.6 验收标准

```text
1. 点击“预览合并”后，Viewer 能高亮候选区域。
2. 报告面板仍输出候选统计。
3. 支持清除候选高亮。
4. 重新打开 STEP/STP 文件后，候选高亮自动清空。
5. 预览前后 face/edge 数不变。
6. 执行 applyMerge() 仍走原 same-domain 合并路径。
7. 大候选数量场景下 UI 不应明显卡死。
```

---

## 6. Stage 2.6：Candidate Selection / Rejection

### 6.1 目标

在候选区域可视化基础上，支持用户对候选区域进行选择、接受、拒绝、隐藏和恢复，为后续 Stage 3 的真实合并提供人工筛选基础。

Stage 2.6 仍然不修改 B-rep。它只管理候选状态。

### 6.2 候选状态

建议引入：

```cpp
enum class MergeCandidateStatus {
    Pending,
    Accepted,
    Rejected,
    Hidden
};
```

语义：

```text
Pending:
  默认状态，候选区域尚未被用户处理。

Accepted:
  用户认为该候选区域可以进入后续真实合并。

Rejected:
  用户明确拒绝该候选，后续不应自动合并。

Hidden:
  用户暂时隐藏该候选，不代表永久拒绝。
```

### 6.3 推荐交互

```text
1. 用户在候选列表中点击候选：
   - Viewer 高亮该候选区域。
   - 报告/检查面板显示候选详情。

2. 用户右键候选或点击按钮：
   - 接受候选
   - 拒绝候选
   - 隐藏候选
   - 恢复候选

3. 用户可以按状态过滤：
   - 全部
   - Pending
   - Accepted
   - Rejected
   - Hidden

4. 用户可以清空本次预览结果。
```

### 6.4 推荐实现边界

```text
1. 候选状态可以先保存在 MainWindow 或 AppController 的运行时状态中。
2. 本阶段不强制实现 ProjectSerializer 持久化。
3. 如果后续需要保存候选状态，再接入 .spo.json。
4. 不要将候选状态写入 ShapeDocument。
5. 不要修改真实 TopoDS_Shape。
```

### 6.5 与 Command 的关系

Stage 2.6 可以暂时不进入 CommandHistory，因为它不修改模型。  
但如果希望候选接受/拒绝也能 undo/redo，可以后续增加：

```text
AcceptCandidateCommand
RejectCandidateCommand
HideCandidateCommand
RestoreCandidateCommand
```

建议第一版不做这些 Command，避免过度设计。

### 6.6 不做内容

```text
1. 不执行真实合并。
2. 不替换 B-rep。
3. 不构造新 face。
4. 不 sewing。
5. 不调用 SurfaceRefitter。
6. 不把 Accepted candidate 自动应用到 applyMerge()。
```

### 6.7 验收标准

```text
1. 用户可以选择某个候选区域并在 Viewer 中高亮。
2. 用户可以接受 / 拒绝 / 隐藏 / 恢复候选区域。
3. 报告或候选列表能显示候选状态。
4. 状态变化不改变 face/edge 数。
5. 清除预览或打开新文件后候选状态清空。
6. Accepted candidates 能被后续 Stage 3A 读取或预留接口读取。
```

---

## 7. Stage 3：Analytic Primitive Region Merge

### 7.1 目标

在通用候选框架和候选可视化基础上，对解析基础图元候选区域执行真正的 B-rep 合并。

Stage 3 不是 Plane-only，而是解析图元合并总阶段：

```text
Stage 3-0: Analytic RegionMerger Framework Preparation，可选
Stage 3A: PlaneRegionMerge
Stage 3B: CylinderRegionMerge
Stage 3C: ConeRegionMerge
Stage 3D: SphereRegionMerge
Stage 3E: Optional TorusRegionMerge
```

进入 Stage 3A 前，建议至少完成：

```text
1. Stage 2：候选区域生成。
2. Stage 2.5：候选区域 GUI 高亮预览。
3. Stage 2.6：候选接受/拒绝状态，或至少能在 GUI 中选中一个候选。
4. Stage 3-0：可选。如果希望先统一 result/options/failure reason，可以先做；如果当前只追求最小可用 PlaneRegionMerge，也可以直接进入 Stage 3A。
```

### 7.2 Stage 3 总体执行流程

```text
1. 从 MergePlannerResult 中选择低风险 analytic primitive candidate。
2. 优先处理用户 Accepted candidate。
3. 不处理 Rejected candidate。
4. 根据 candidate_type 分派到具体 RegionMerger。
5. 提取 candidate.faces 对应的区域。
6. 提取区域外边界 boundary loop。
7. 构建目标解析曲面。
8. 将 boundary loop 投影到目标曲面参数域。
9. 构建新的 trimmed face。
10. 替换原 region。
11. sewing。
12. ShapeValidator / BRepCheck。
13. ErrorMetric 计算 max / mean / RMS deviation。
14. 成功则提交，失败则 rollback。
```

Stage 3 不应直接复用 `SameDomainUnifier` 的路径。它应形成独立的 region merge command，并与现有 same-domain merge 路径保持边界清晰。

---

### 7.3 Stage 3-0：Analytic RegionMerger Framework Preparation，可选

#### 7.3.1 目标

在进入 Stage 3A/B/C/D/E 的真实解析图元区域合并前，预留统一的 region merge 基础框架。

该阶段只做框架准备，不执行真实 B-rep 替换。

它回答：

```text
1. Stage 3 各解析图元合并器如何统一返回结果？
2. 失败原因如何统一记录？
3. Plane / Cylinder / Cone / Sphere / Torus 的 merger 文件和接口如何预留？
4. 后续 Command / GUI / Report 如何使用统一结果？
```

Stage 3-0 不是必须阶段。  
如果当前代码规模较小，也可以直接进入 Stage 3A，并在 Stage 3A 中最小化实现所需 result/options。  
但若希望后续 3B/3C/3D/3E 更规整，可以先完成 Stage 3-0。

#### 7.3.2 定位

Stage 3-0 是文档和接口层面的准备阶段：

```text
允许：
  统一 options / result / failure reason。
  预留 primitive-specific merger stub。
  预留 NotImplemented / NotSupported 返回路径。
  增加基础测试，确保 stub 不修改模型。

不允许：
  执行真实 PlaneRegionMerge。
  执行 Cylinder / Cone / Sphere / Torus 合并。
  替换 TopoDS_Shape。
  sewing。
  新增 GUI 执行入口。
  修改 applyMerge()。
  修改 SameDomainUnifier。
```

#### 7.3.3 推荐新增数据结构

推荐新增：

```cpp
enum class RegionMergeFailureReason {
    None,
    NotImplemented,
    NotSupported,
    CandidateNotFound,
    InvalidCandidate,
    UnsupportedCandidateType,
    RejectedCandidate,
    HiddenCandidate,
    InsufficientFaces,
    ProtectedEdgeConflict,
    LockedEdgeConflict,
    BoundaryLoopInvalid,
    MultipleOuterLoopsNotSupported,
    InnerLoopsNotSupported,
    PrimitiveFitFailed,
    DeviationTooLarge,
    SurfaceConstructionFailed,
    TopologyReplacementFailed,
    SewingFailed,
    ValidationFailed
};
```

推荐通用结果结构：

```cpp
struct RegionMergeResult {
    bool success = false;
    RegionMergeFailureReason failure_reason = RegionMergeFailureReason::None;
    std::string message;

    int candidate_id = -1;
    MergeCandidateType candidate_type = MergeCandidateType::Unknown;

    int face_count_before = 0;
    int face_count_after = 0;
    int edge_count_before = 0;
    int edge_count_after = 0;

    double face_reduction_ratio = 0.0;
    double edge_reduction_ratio = 0.0;

    double max_deviation = 0.0;
    double mean_deviation = 0.0;
    double rms_deviation = 0.0;
};
```

推荐通用 options：

```cpp
struct RegionMergeOptions {
    double max_deviation = 0.02;
    int min_region_faces = 2;
    bool allow_pending_candidate = false;
    bool require_accepted_candidate = true;
};
```

如果项目现有代码更适合 primitive-specific result/options，也可以不强制抽象。  
原则是：不要为了未来所有阶段过度设计，但要避免 3A/3B/3D 各自重复定义完全相同的统计字段。

#### 7.3.4 推荐预留文件

可选新增：

```text
src/merge/RegionMergeResult.h
src/merge/RegionMergeOptions.h

src/merge/PlaneRegionMerger.h
src/merge/PlaneRegionMerger.cpp

src/merge/CylinderRegionMerger.h
src/merge/CylinderRegionMerger.cpp

src/merge/ConeRegionMerger.h
src/merge/ConeRegionMerger.cpp

src/merge/SphereRegionMerger.h
src/merge/SphereRegionMerger.cpp

src/merge/TorusRegionMerger.h
src/merge/TorusRegionMerger.cpp
```

其中 `PlaneRegionMerger` 可以先返回 `NotImplemented`，也可以留到 Stage 3A 再实现。  
其他 `Cylinder/Cone/Sphere/Torus` merger 在 Stage 3-0 中只能作为 stub 存在，不得实现真实拓扑替换。

#### 7.3.5 推荐 stub 行为

每个 primitive-specific merger 的 stub 应满足：

```text
1. 接收 input shape / topology / candidate / options。
2. 检查 candidate type 是否匹配。
3. 不修改 input shape。
4. 返回 RegionMergeResult。
5. 对未实现逻辑返回 NotImplemented。
6. 对不支持 candidate 返回 UnsupportedCandidateType。
7. message 中清楚说明当前阶段未实现真实合并。
```

示例语义：

```text
PlaneRegionMerger:
  Stage 3-0 中可返回 NotImplemented。
  Stage 3A 中再实现真实合并。

CylinderRegionMerger:
  Stage 3-0 中只能返回 NotImplemented。
  Stage 3B 中再实现真实合并。

ConeRegionMerger:
  Stage 3-0 中只能返回 NotImplemented。
  Stage 3C 中再实现真实合并。

SphereRegionMerger:
  Stage 3-0 中只能返回 NotImplemented。
  Stage 3D 中再实现真实合并。

TorusRegionMerger:
  Stage 3-0 中只能返回 NotImplemented。
  Stage 3E 中再实现真实合并。
```

#### 7.3.6 与 Command / GUI 的关系

Stage 3-0 不要求新增 Command，也不要求新增 GUI 入口。

允许：

```text
1. 预留 Command 未来可能使用的 result/options。
2. 在代码注释中说明 Stage 3A 后由 PlaneRegionMergeCommand 调用 PlaneRegionMerger。
3. 增加测试验证 stub 不修改模型。
```

不允许：

```text
1. 新增“执行平面候选合并”按钮。
2. 新增“执行圆柱候选合并”按钮。
3. 将 stub 接入 MainWindow。
4. 将 stub 接入 applyMerge()。
5. 将 Accepted candidates 自动应用到任何真实合并。
```

#### 7.3.7 不做内容

```text
1. 不执行真实 region merge。
2. 不替换 B-rep。
3. 不删除 face。
4. 不构造新 face。
5. 不 sewing。
6. 不调用 SurfaceRefitter。
7. 不修改 SameDomainUnifier。
8. 不修改 MergePatchCommand 的 same-domain 合并语义。
9. 不实现 Plane / Cylinder / Cone / Sphere / Torus 的真实 trimmed face 构造。
10. 不引入大规模抽象或复杂继承层级。
```

#### 7.3.8 验收标准

```text
1. 项目构建通过。
2. 现有测试通过。
3. 新增 result/options/failure reason 后，不破坏 Stage 1 / Stage 2 / Stage 2.5 / Stage 2.6。
4. stub merger 调用不会修改 ShapeDocument。
5. stub merger 对未实现功能返回 NotImplemented。
6. stub merger 对错误 candidate type 返回 UnsupportedCandidateType。
7. 没有 GUI 入口暴露未实现功能。
8. applyMerge() 仍然走 SameDomainUnifier。
9. 文档中明确：Stage 3-0 是可选框架准备，不是实际合并阶段。
```

---

### 7.4 Stage 3A：PlaneRegionMerge

#### 7.8.1 目标

对 `PlaneLike` candidate 执行真实 B-rep 区域合并。

目标形式：

```text
多个近似共面的相邻 faces
→ 合并为一个较大的 planar trimmed face
```

适用对象：

```text
1. 底面。
2. 平台。
3. 切平面。
4. 机械式平面结构。
5. 潮玩底座中的平面块。
```

#### 7.8.2 前置条件

必须先完成：

```text
1. Stage 2：MergeCandidate / MergePlanner / MergeRegionGrower。
2. Stage 2.5：Candidate GUI Preview。
3. Stage 2.6：Candidate Selection / Rejection。
4. ShapeValidator 基础合法性检查。
5. ErrorMetric 基础误差统计接口。
```

#### 7.8.3 输入

```text
ShapeDocument
TopologyGraph
MergeCandidate(candidate_type == PlaneLike)
protectedEdges
lockedEdges
MergeCandidateStatus
PlaneRegionMergeOptions
```

推荐参数：

```cpp
struct PlaneRegionMergeOptions {
    double normal_angle_tolerance_degrees = 3.0;
    double plane_distance_tolerance = 0.01;
    double max_deviation = 0.02;
    int min_region_faces = 2;
};
```

#### 7.8.4 候选判断条件

`PlaneLike` candidate 应满足：

```text
1. 区域内 faces 可拟合到同一个 plane。
2. face normal 与目标 plane normal 夹角 < normal_angle_tolerance_degrees。
3. face center 到目标 plane 的距离 < plane_distance_tolerance。
4. shared edge 不是 protected edge。
5. shared edge 不是 locked edge。
6. candidate status != Rejected。
7. region face count >= min_region_faces。
```

#### 7.8.5 实现流程

```text
1. 从 candidate.faces 提取区域内 faces。
2. 提取区域外边界 boundary loop。
3. 拟合或确定目标 Geom_Plane。
4. 将 boundary loop 投影到 plane 参数域。
5. 构建新的 planar trimmed face。
6. 用新 face 替换原 candidate region。
7. sewing。
8. ShapeValidator / BRepCheck。
9. ErrorMetric 计算 max / mean / RMS deviation。
10. 成功则提交，失败则 rollback。
```

#### 7.8.6 实现边界

```text
1. 只处理低风险 PlaneLike candidate。
2. 优先处理用户 Accepted candidate。
3. 不处理 Rejected candidate。
4. 不跨越 protectedEdges。
5. 不跨越 lockedEdges。
6. 不处理复杂多洞平面区域。
7. 不处理 cylinder / cone / sphere / torus / freeform。
```

#### 7.8.7 风险点

```text
1. boundary loop 提取错误会导致新 face 拓扑不合法。
2. 多洞区域可能需要 inner wire 管理。
3. 微小边、退化边可能导致 face 构造失败。
4. sewing 后可能产生 free edge 或 multiple edge。
```

#### 7.8.8 验收标准

```text
1. 对明确共面的碎片区域，face 数明显下降。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 后二次读取通过。
4. max deviation 小于阈值。
5. 不跨越 protectedEdges / lockedEdges。
6. 用户 Rejected candidate 不会被应用。
7. undo / redo 正常。
8. 报告中记录：
   - candidate id
   - plane normal
   - face reduction
   - edge reduction
   - max / mean / RMS deviation
```

---

### 7.5 Stage 3B：CylinderRegionMerge

#### 7.8.1 目标

对 `CylinderLike` candidate 执行真实 B-rep 区域合并。

目标形式：

```text
多个近似同圆柱面的相邻 faces
→ 合并为一个较大的 cylindrical trimmed face
```

适用对象：

```text
1. 圆柱形底座。
2. 手臂、腿部、杆状结构。
3. 管状装饰件。
4. 圆柱连接件。
5. 由多个窄面片拼成的圆柱侧壁。
```

#### 7.8.2 前置条件

必须先完成：

```text
1. Stage 2：MergeCandidate / MergePlanner / MergeRegionGrower。
2. Stage 2.5：Candidate GUI Preview。
3. Stage 2.6：Candidate Selection / Rejection。
4. PlaneRegionMerge baseline。
5. ShapeValidator 和 ErrorMetric。
```

#### 7.8.3 输入

```text
ShapeDocument
TopologyGraph
MergeCandidate(candidate_type == CylinderLike)
protectedEdges
lockedEdges
MergeCandidateStatus
CylinderRegionMergeOptions
```

推荐参数：

```cpp
struct CylinderRegionMergeOptions {
    double axis_angle_tolerance_degrees = 3.0;
    double axis_position_tolerance = 0.01;
    double radius_tolerance = 0.01;
    double max_deviation = 0.02;
    int min_region_faces = 2;
};
```

#### 7.8.4 候选判断条件

`CylinderLike` candidate 应满足：

```text
1. 区域内 faces 可拟合到同一个圆柱面。
2. 圆柱轴线方向差 < axis_angle_tolerance_degrees。
3. 圆柱轴线位置偏差 < axis_position_tolerance。
4. 半径差 < radius_tolerance。
5. shared edge 不是 protected edge。
6. shared edge 不是 locked edge。
7. candidate status != Rejected。
8. region face count >= min_region_faces。
```

#### 7.8.5 实现流程

```text
1. 从 candidate.faces 提取区域内 faces。
2. 提取区域外边界 boundary loop。
3. 对区域内 faces 估计或拟合目标 cylinder：
   - axis direction
   - axis location
   - radius
4. 检查区域边界是否可投影到 cylinder 参数域。
5. 将 boundary loop 投影到 cylindrical surface 的参数域。
6. 构建新的 cylindrical trimmed face。
7. 用新 face 替换原 candidate region。
8. sewing。
9. ShapeValidator / BRepCheck。
10. ErrorMetric 计算 max / mean / RMS deviation。
11. 成功则提交，失败则 rollback。
```

#### 7.8.6 实现边界

```text
1. 只处理低风险 CylinderLike candidate。
2. 优先处理用户 Accepted candidate。
3. 不处理 Rejected candidate。
4. 不跨越 protectedEdges。
5. 不跨越 lockedEdges。
6. 不处理复杂多洞圆柱区域。
7. 不处理拓扑自交风险高的圆柱区域。
8. 不处理 cone / sphere / torus / freeform。
```

#### 7.8.7 风险点

```text
1. 圆柱参数域存在周期 seam，边界投影可能跨越 0/2π。
2. 圆柱面 trim curve 重建比平面复杂。
3. 多个窄片拼接时可能存在局部法向噪声。
4. 候选区域过大时可能误把非同轴圆柱合并。
5. sewing 后可能产生 free edge 或 multiple edge。
```

#### 7.8.8 验收标准

```text
1. 对明确同轴、同半径的圆柱碎片区域，face 数明显下降。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 后二次读取通过。
4. max deviation 小于阈值。
5. 不跨越 protectedEdges / lockedEdges。
6. 用户 Rejected candidate 不会被应用。
7. undo / redo 正常。
8. 报告中记录：
   - candidate id
   - radius
   - axis direction
   - face reduction
   - edge reduction
   - max / mean / RMS deviation
```

---

### 7.6 Stage 3C：ConeRegionMerge

#### 7.8.1 目标

对 `ConeLike` candidate 执行真实 B-rep 区域合并。

目标形式：

```text
多个近似同圆锥面的相邻 faces
→ 合并为一个较大的 conical trimmed face
```

适用对象：

```text
1. 锥形帽子。
2. 尖角装饰。
3. 锥形底座。
4. 圆锥形连接结构。
5. 规则尖锐但非自由曲面的装饰部件。
```

#### 7.8.2 前置条件

必须先完成：

```text
1. Stage 2：MergeCandidate / MergePlanner / MergeRegionGrower。
2. Stage 2.5：Candidate GUI Preview。
3. Stage 2.6：Candidate Selection / Rejection。
4. CylinderRegionMerge 中的轴线、半径、参数域处理经验。
5. ShapeValidator 和 ErrorMetric。
```

#### 7.8.3 输入

```text
ShapeDocument
TopologyGraph
MergeCandidate(candidate_type == ConeLike)
protectedEdges
lockedEdges
MergeCandidateStatus
ConeRegionMergeOptions
```

推荐参数：

```cpp
struct ConeRegionMergeOptions {
    double axis_angle_tolerance_degrees = 3.0;
    double apex_position_tolerance = 0.02;
    double semi_angle_tolerance_degrees = 2.0;
    double radius_tolerance = 0.01;
    double max_deviation = 0.02;
    int min_region_faces = 2;
};
```

#### 7.8.4 候选判断条件

`ConeLike` candidate 应满足：

```text
1. 区域内 faces 可拟合到同一个圆锥面。
2. 圆锥轴线方向差 < axis_angle_tolerance_degrees。
3. 顶点位置偏差 < apex_position_tolerance。
4. 半角差 < semi_angle_tolerance_degrees。
5. 局部半径变化符合圆锥参数。
6. shared edge 不是 protected edge。
7. shared edge 不是 locked edge。
8. candidate status != Rejected。
```

#### 7.8.5 实现流程

```text
1. 从 candidate.faces 提取区域内 faces。
2. 估计目标 cone：
   - axis direction
   - apex location
   - semi-angle
   - reference radius
3. 提取区域外边界 boundary loop。
4. 检查 boundary loop 是否靠近 cone apex。
5. 如果区域过于接近 apex，标记为 High risk 并拒绝自动合并。
6. 将 boundary loop 投影到 conical surface 参数域。
7. 构建新的 conical trimmed face。
8. 替换原 region。
9. sewing。
10. ShapeValidator / BRepCheck。
11. ErrorMetric 计算偏差。
12. 成功提交，失败 rollback。
```

#### 7.8.6 实现边界

```text
1. 第一版只处理远离 apex 的稳定圆锥区域。
2. 不处理包含 cone apex 的复杂区域。
3. 不跨越 protectedEdges / lockedEdges。
4. 不处理自由曲面尖角。
5. 不处理由多个不同 cone 拼成的装饰区域。
```

#### 7.8.7 风险点

```text
1. cone apex 附近参数域退化，trim curve 容易不稳定。
2. 尖角结构可能同时是重要特征，不能被错误光顺。
3. 潮玩模型中的“尖角”很多并非规则圆锥，而是自由曲面尖锐结构。
4. 误把自由曲面尖角当成 cone 会破坏造型。
```

#### 7.8.8 验收标准

```text
1. 对明确规则圆锥碎片区域，face 数下降。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 后二次读取通过。
4. 不跨越 protectedEdges / lockedEdges。
5. 不处理 Rejected candidate。
6. apex 附近高风险区域不会自动合并。
7. 报告中记录：
   - candidate id
   - apex
   - axis direction
   - semi-angle
   - max / mean / RMS deviation
   - risk level
```

---

### 7.7 Stage 3D：SphereRegionMerge

#### 7.8.1 目标

对 `SphereLike` candidate 执行真实 B-rep 区域合并。

目标形式：

```text
多个近似同球面的相邻 faces
→ 合并为一个较大的 spherical trimmed face
```

适用对象：

```text
1. 眼球。
2. 球形关节。
3. 圆形装饰件。
4. 局部近似球面的头部或身体结构。
5. 多个碎片面组成的球冠区域。
```

#### 7.8.2 前置条件

必须先完成：

```text
1. Stage 2：MergeCandidate / MergePlanner / MergeRegionGrower。
2. Stage 2.5：Candidate GUI Preview。
3. Stage 2.6：Candidate Selection / Rejection。
4. PlaneRegionMerge baseline。
5. ShapeValidator 和 ErrorMetric。
```

#### 7.8.3 输入

```text
ShapeDocument
TopologyGraph
MergeCandidate(candidate_type == SphereLike)
protectedEdges
lockedEdges
MergeCandidateStatus
SphereRegionMergeOptions
```

推荐参数：

```cpp
struct SphereRegionMergeOptions {
    double center_tolerance = 0.01;
    double radius_tolerance = 0.01;
    double normal_angle_tolerance_degrees = 5.0;
    double max_deviation = 0.02;
    int min_region_faces = 2;
};
```

#### 7.8.4 候选判断条件

`SphereLike` candidate 应满足：

```text
1. 区域内 faces 可拟合到同一个球面。
2. 球心偏差 < center_tolerance。
3. 半径差 < radius_tolerance。
4. face normal 与球面理论法向夹角 < normal_angle_tolerance_degrees。
5. shared edge 不是 protected edge。
6. shared edge 不是 locked edge。
7. candidate status != Rejected。
```

#### 7.8.5 实现流程

```text
1. 从 candidate.faces 提取区域内 faces。
2. 拟合目标 sphere：
   - center
   - radius
3. 提取区域外边界 boundary loop。
4. 检查 boundary loop 是否接近 sphere pole 或跨越参数 seam。
5. 将 boundary loop 投影到 spherical surface 参数域。
6. 构建新的 spherical trimmed face。
7. 替换原 region。
8. sewing。
9. ShapeValidator / BRepCheck。
10. ErrorMetric 计算偏差。
11. 成功提交，失败 rollback。
```

#### 7.8.6 实现边界

```text
1. 第一版只处理球冠类稳定区域。
2. 不处理覆盖超过半球且跨 seam 的复杂区域。
3. 不处理包含球面极点附近退化边界的区域。
4. 不跨越 protectedEdges / lockedEdges。
5. 不处理自由曲面近似球面但误差较大的区域。
```

#### 7.8.7 风险点

```text
1. 球面参数域存在极点退化。
2. 球面存在 seam，boundary loop 可能跨 seam。
3. 潮玩头部可能只是近似球面，不应强行拟合成精确球。
4. 过强的球面拟合可能损失造型细节。
```

#### 7.8.8 验收标准

```text
1. 对明确同球心、同半径的球面碎片区域，face 数下降。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 后二次读取通过。
4. 不跨越 protectedEdges / lockedEdges。
5. 用户 Rejected candidate 不会被应用。
6. 对高误差近似球面区域应拒绝自动合并或标记 High risk。
7. 报告中记录：
   - candidate id
   - center
   - radius
   - max / mean / RMS deviation
   - risk level
```

---

### 7.8 Stage 3E：TorusRegionMerge，可选

#### 7.8.1 目标

对 `TorusLike` candidate 执行真实 B-rep 区域合并。

目标形式：

```text
多个近似同圆环面的相邻 faces
→ 合并为一个较大的 toroidal trimmed face
```

适用对象：

```text
1. 环形装饰。
2. 甜甜圈状结构。
3. 环状管件。
4. 圆环边缘。
5. 某些圆角过渡区域。
```

#### 7.8.2 定位

`TorusRegionMerge` 是 Stage 3 的可选子阶段，不建议近期优先实现。

推荐优先级：

```text
PlaneRegionMerge
→ CylinderRegionMerge
→ SphereRegionMerge
→ ConeRegionMerge
→ TorusRegionMerge
```

原因：

```text
1. torus 参数更多。
2. 参数域双周期，trim curve 更复杂。
3. 工程实现和验证难度高。
4. 潮玩模型中的环状区域占比通常低于平面、圆柱、球面和自由曲面。
```

#### 7.8.3 输入

```text
ShapeDocument
TopologyGraph
MergeCandidate(candidate_type == TorusLike)
protectedEdges
lockedEdges
MergeCandidateStatus
TorusRegionMergeOptions
```

推荐参数：

```cpp
struct TorusRegionMergeOptions {
    double axis_angle_tolerance_degrees = 3.0;
    double center_tolerance = 0.02;
    double major_radius_tolerance = 0.02;
    double minor_radius_tolerance = 0.01;
    double max_deviation = 0.02;
    int min_region_faces = 2;
};
```

#### 7.8.4 候选判断条件

`TorusLike` candidate 应满足：

```text
1. 区域内 faces 可拟合到同一个 torus。
2. torus axis 方向差 < axis_angle_tolerance_degrees。
3. torus center 偏差 < center_tolerance。
4. major radius 差 < major_radius_tolerance。
5. minor radius 差 < minor_radius_tolerance。
6. shared edge 不是 protected edge。
7. shared edge 不是 locked edge。
8. candidate status != Rejected。
```

#### 7.8.5 实现流程

```text
1. 从 candidate.faces 提取区域内 faces。
2. 拟合目标 torus：
   - center
   - axis direction
   - major radius
   - minor radius
3. 提取区域外边界 boundary loop。
4. 检查 boundary loop 是否跨越双周期 seam。
5. 将 boundary loop 投影到 toroidal surface 参数域。
6. 构建新的 toroidal trimmed face。
7. 替换原 region。
8. sewing。
9. ShapeValidator / BRepCheck。
10. ErrorMetric 计算偏差。
11. 成功提交，失败 rollback。
```

#### 7.8.6 实现边界

```text
1. 第一版只处理低风险、局部 torus patch。
2. 不处理跨越大范围双周期 seam 的 torus。
3. 不处理复杂环面自交边界。
4. 不处理自由曲面圆角伪 torus。
5. 不跨越 protectedEdges / lockedEdges。
```

#### 7.8.7 风险点

```text
1. torus 参数域双周期，boundary loop 处理复杂。
2. 很多 CAD 圆角可能是 torus，也可能是 B-spline 过渡面。
3. 误判 torus 会破坏圆角或装饰细节。
4. 验证和 rollback 成本高。
```

#### 7.8.8 验收标准

```text
1. 对明确 torus patch 的测试样例可以合并。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 后二次读取通过。
4. 不跨越 protectedEdges / lockedEdges。
5. 用户 Rejected candidate 不会被应用。
6. 高风险 torus candidate 不自动合并。
7. 报告中记录：
   - candidate id
   - center
   - axis direction
   - major radius
   - minor radius
   - max / mean / RMS deviation
   - risk level
```

---

### 7.9 Stage 3 子阶段推荐实现顺序

Stage 3 虽然被划分为 A/B/C/D/E 五个子阶段，但不建议并行实现。  
这里的顺序不是按字母顺序，而是按工程风险、样例常见度和对当前系统的收益排序。

推荐完整实现顺序：

```text
1. Stage 3A：PlaneRegionMerge
   最低风险，边界投影和 trimmed face 构造最稳定，用于建立真实 region merge baseline。

2. Stage 3B：CylinderRegionMerge
   工程价值高，潮玩和机械结构中较常见，参数复杂度中等。

3. Stage 3D：SphereRegionMerge
   潮玩眼球、球形装饰、球形关节常见，参数较少，但存在极点和 seam 问题。

4. Stage 3C：ConeRegionMerge
   保留但后置。它适合规则锥面，但容易和自由曲面尖角、发尖、高曲率装饰区域混淆。
   需要更稳定的 protectedEdges、ConeLike candidate detection 和 apex 风险控制后再做。

5. Stage 3E：TorusRegionMerge
   保留但作为可选高级阶段。torus 参数域双周期，trim curve 和 seam 处理复杂。
   只有在存在明确 torus patch 样例和工程需求时才建议实现。
```

推荐近期主线：

```text
Stage 2.6
→ Stage 3A PlaneRegionMerge
→ Stage 3B CylinderRegionMerge
→ Stage 3D SphereRegionMerge
→ Stage 4 Freeform Candidate Detection
→ Stage 5 Freeform B-spline / Plate Refit
```

解析图元补全路线：

```text
Stage 3C ConeRegionMerge
→ Stage 3E TorusRegionMerge，可选
```

说明：

```text
1. Stage 3C 和 Stage 3E 不是被删除，也不是不做。
2. Stage 3C 因为容易误伤自由曲面尖角，所以应后置。
3. Stage 3E 因为参数域复杂、样例占比不稳定，所以作为可选补全。
4. 3A / 3B / 3D 建立稳定收益后，再根据样例需求补 3C / 3E。
5. 3C / 3E 不应阻塞 Stage 4 / Stage 5 的自由曲面路线。
```

### 7.10 Stage 3 总体验收标准

```text
1. Stage 3A/3B/3D 至少各有一个可验证样例。
2. 所有解析图元合并都不能跨越 protectedEdges。
3. 所有解析图元合并都不能跨越 lockedEdges。
4. 用户 Rejected candidate 不会被应用。
5. Accepted candidate 可优先参与真实合并。
6. 合并后 BRepCheck 通过。
7. 导出 STEP 后二次读取通过。
8. ErrorMetric 可记录 max / mean / RMS deviation。
9. 合并率、偏差、失败原因写入报告。
10. 失败时不污染当前 ShapeDocument。
11. undo / redo 正常。
12. applyMerge() 的 same-domain 路径与 region merge 路径边界清晰。
```

---

## 8. Stage 4：Freeform Candidate Detection

### 8.1 目标

提前识别潮玩模型常见自由曲面碎片区域，但不立即重拟合。

该阶段回答：

```text
哪些 B-spline / Bezier / freeform patches 可能属于同一个光顺区域？
哪些区域值得后续 B-spline / Plate refit？
哪些区域风险过高，不应该自动重拟合？
```

### 8.2 为什么 Stage 4 要提前

潮玩模型中大量区域不是 Plane / Cylinder / Sphere：

```text
头发曲面
脸部曲面
衣服褶皱
圆润过渡
身体外壳
帽子、鞋子、装饰件的光顺区域
```

如果只做解析图元合并，face reduction 会有明显上限。因此，在 PlaneRegionMerge 得到稳定 baseline 后，应尽早启动 Freeform Candidate Detection。

### 8.3 候选类型

```cpp
MergeCandidateType::FreeformG1
MergeCandidateType::FreeformG2
```

### 8.4 判断依据

```text
1. surface type 是 B-spline / Bezier / Other。
2. 相邻 face 之间近似 G1 连续。
3. 法向变化连续。
4. 曲率跳变较小。
5. shared edge 不是 feature edge。
6. shared edge 不是 locked edge。
7. 区域边界闭合。
8. 区域复杂度在可控范围内。
```

### 8.5 输出指标

```text
freeform candidate count
faces per candidate
boundary edge count
normal continuity score
curvature continuity score
boundary complexity
risk_level
reject_reason
```

### 8.6 不做内容

```text
1. 不拟合新曲面。
2. 不替换 B-rep。
3. 不重建 trim curve。
4. 不 sewing。
5. 不自动导出优化结果。
```

### 8.7 验收标准

```text
1. 能在真实潮玩 STEP 上识别一批 FreeformG1 / FreeformG2 候选区域。
2. 不跨越自动特征边或用户锁定边。
3. 预览不会改变 face/edge 数。
4. 报告中能看到每个候选区域的连续性指标和风险等级。
5. 可与 PlaneLike / CylinderLike 等候选类型共存。
6. Freeform candidates 可以复用 Stage 2.5 / 2.6 的高亮和接受/拒绝机制。
```

---

## 9. Stage 5：Freeform B-spline / Plate Refit

### 9.1 目标

对高可信自由曲面候选区域执行局部重拟合，把多个光顺自由曲面碎片合并成更少的 B-spline face。

这是解决潮玩自由曲面碎片化的关键阶段，但也是最高风险阶段。

### 9.2 基本流程

```text
1. 取 Stage 4 生成的 FreeformG1 / FreeformG2 candidate。
2. 优先处理用户 Accepted candidate。
3. 提取区域外边界 loop。
4. 从原始 faces 采样点和法向。
5. 建立局部参数化。
6. 用 B-spline surface 或 Plate surface 拟合。
7. 将边界投影到新 surface 参数域。
8. 重建 trim curve。
9. 构建新的 trimmed face。
10. 替换原 region。
11. sewing。
12. ShapeValidator + ErrorMetric。
13. 成功提交，失败 rollback。
```

### 9.3 可选技术路径

```text
1. B-spline surface fitting
   适合规则参数化、采样点分布较稳定的区域。

2. Plate surface fitting
   适合需要边界约束、点约束和局部光顺的区域。

3. Hybrid refit
   先尝试 B-spline，失败后尝试 Plate，仍失败则保留原始 patch。
```

### 9.4 风险

```text
1. 参数化困难。
2. trim curve 重建困难。
3. 容易破坏 solid 合法性。
4. 容易产生偏差过大的光顺面。
5. 容易抹掉潮玩模型的细节特征。
6. 需要更完整的误差评估。
```

### 9.5 前置条件

必须先完成：

```text
1. MergeCandidate。
2. MergePlanner。
3. MergeRegionGrower。
4. Candidate GUI Preview。
5. Candidate Selection / Rejection。
6. PlaneRegionMerge baseline。
7. Freeform Candidate Detection。
8. ErrorMetric。
9. ReportGenerator。
10. 更完整的 ShapeValidator。
11. Command 级 rollback / undo。
```

### 9.6 验收标准

```text
1. 只在用户确认或实验模式下启用。
2. max deviation / mean deviation / rms deviation 可计算。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 可通过 undo 回退。
6. 报告中记录拟合误差、采样数量、边界复杂度和合并风险。
7. 对真实潮玩自由曲面样例能产生可解释的 face reduction。
8. 用户 Rejected candidate 不会被应用。
```

---

## 10. 推荐实际推进顺序

### 10.1 工程优先路线

目标是尽快看到稳定合并率提升：

```text
1. Stage 1：SameDomainUnifier 参数与报告增强，重点验证 concat_bsplines=true。
2. Stage 2：通用候选区域框架。
3. Stage 2.5：候选区域 GUI 高亮预览。
4. Stage 2.6：候选区域选择 / 接受 / 拒绝。
5. Stage 3-0：Analytic RegionMerger Framework Preparation，可选。
6. Stage 3A：PlaneRegionMerge。
7. Stage 3B：CylinderRegionMerge。
8. Stage 3D：SphereRegionMerge。
9. Stage 4：Freeform Candidate Detection。
10. Stage 5：Freeform B-spline / Plate Refit。
11. Stage 3C：ConeRegionMerge，作为解析图元补全任务，后置。
12. Stage 3E：TorusRegionMerge，可选，仅在有明确样例需求时实现。
```

### 10.2 研究优先路线

目标是更贴近潮玩模型论文方向：

```text
1. Stage 2：通用候选区域框架。
2. Stage 2.5：候选区域 GUI 高亮预览。
3. Stage 3A：PlaneRegionMerge，作为稳定 baseline。
4. Stage 4：Freeform Candidate Detection。
5. Stage 2.6：候选区域接受 / 拒绝，用于构建人工标注和对比数据。
6. Stage 5：Freeform B-spline / Plate Refit。
7. Stage 3B / 3C / 3D：解析图元补全。
```

### 10.3 当前推荐折中路线

结合当前项目状态，建议采用折中路线：

```text
1. Stage 1：增强 SameDomainUnifier 的参数与报告，重点暴露 concat_bsplines 并验证 true/false。
2. Stage 2：实现通用 MergeCandidate / MergePlanner / MergeRegionGrower。
3. Stage 2 初期只启用 PlaneLike candidate，但框架预留多类型。
4. Stage 2.5：实现候选区域 GUI 高亮预览。
5. Stage 2.6：实现候选区域选择 / 接受 / 拒绝。
6. Stage 3-0：可选，预留 RegionMerger result/options/failure reason 和 NotImplemented stub。
7. Stage 3A：实现 PlaneRegionMerge，形成稳定 baseline。
8. Stage 3B：实现 CylinderRegionMerge。
9. Stage 3D：实现 SphereRegionMerge。
10. Stage 4：尽早启动 Freeform Candidate Detection。
11. Stage 5：在 ErrorMetric / ReportGenerator 完善后再做自由曲面重拟合。
12. Stage 3C：在尖角保护、ConeLike 检测和 apex 风险控制稳定后实现。
13. Stage 3E：作为 Optional/Future Work，仅在 torus 样例充分时实现。
```

当前折中路线的含义是：

```text
1. 文档先行，代码只能实现文档中已经定义的阶段和边界。
2. Stage 3-0 是可选框架准备，不是实际合并阶段。
3. 3A / 3B / 3D 是近期稳定收益主线。
4. 3C / 3E 是解析图元补全路线，不删除，但不阻塞 Stage 4 / Stage 5。
5. 3C 的核心风险是误伤自由曲面尖角。
6. 3E 的核心风险是 torus 双周期参数域和 trim curve 复杂度。
```

---

## 11. Codex 开发约束

Codex 修改合并算法时必须遵守：

```text
1. 文档先行。代码只能实现本文件和相关 docs 中已经定义的阶段、边界和验收标准。
2. 架构边界遵循 docs/module_design.md。
3. 当前任务状态遵循 docs/implementation_status.md。
4. 合并算法路线遵循本文件。
4. GUI 不直接修改 TopoDS_Shape。
5. 修改模型的操作必须通过 Command。
6. MergePlanner / MergeRegionGrower 只负责候选生成，不负责 GUI。
7. merge 模块不负责界面显示。
8. validate 模块只负责验证，不负责合并策略。
9. brep 模块只负责索引和查询，不负责修改模型。
10. 在 Stage 2.5 / 2.6 之前不要实现 PlaneRegionMerge。
11. 在候选区域没有验证前，不要做破坏性拓扑替换。
12. 每个阶段只实现当前阶段允许的最小功能，不提前混入后续阶段。
13. Stage 1 不允许硬编码 concat_bsplines=true；必须暴露参数并记录实际值。
14. Candidate Preview 不应改变 ShapeDocument。
15. Candidate Selection / Rejection 不应自动应用到真实合并。
16. Stage 3A/B/C/D/E 都必须通过 Command 层执行真实模型修改。
17. Stage 3 的 region merge 路径必须与 applyMerge() 的 same-domain 路径边界清晰。
```

---

## 12. 每阶段推荐 Codex 提示词模板

### 12.1 通用前缀

```text
你正在修改 C++20 + Qt6 + OpenCASCADE 项目 step-patch-optimizer。

架构边界遵循 docs/module_design.md。
当前任务优先级和验收标准遵循 docs/implementation_status.md。
合并算法阶段路线遵循 docs/merge_algorithm_roadmap.md。

本次只实现指定阶段，不提前实现后续阶段。
不要大规模重构无关代码。
不要改变现有 GUI 主流程。
现有测试必须保持通过。
```

### 12.2 Stage 2.6 开发提示词摘要

```text
本次实现 Stage 2.6：Candidate Selection / Rejection。

目标：
- 支持用户选择候选区域。
- 支持接受 / 拒绝 / 隐藏 / 恢复候选区域。
- 候选状态只作为运行时预览状态，不修改 ShapeDocument。
- 不执行真实合并。
- 不实现 PlaneRegionMerge。

要求：
1. 增加 MergeCandidateStatus：Pending / Accepted / Rejected / Hidden。
2. GUI 能显示候选状态。
3. 用户选择候选后 Viewer 高亮该 candidate。
4. 用户拒绝候选后，该 candidate 不再参与后续候选应用。
5. 清除预览或打开新文件时状态清空。
6. Accepted candidates 预留给 Stage 3A 使用，但本阶段不应用。
```

### 12.3 Stage 3A 开发提示词摘要

```text
本次实现 Stage 3A：PlaneRegionMerge。

目标：
- 只处理 PlaneLike candidate。
- 优先处理用户 Accepted candidate。
- 不处理 Rejected candidate。
- 从 candidate.faces 提取 boundary loop。
- 构造新的 planar trimmed face。
- 替换原区域并 sewing。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
- 支持 undo / redo。
- 不处理 Cylinder / Sphere / Freeform。
```

### 12.4 Stage 3B 开发提示词摘要

```text
本次实现 Stage 3B：CylinderRegionMerge。

目标：
- 只处理 CylinderLike candidate。
- 优先处理用户 Accepted candidate。
- 不处理 Rejected candidate。
- 检查 axis direction / axis location / radius compatibility。
- 从 candidate.faces 提取 boundary loop。
- 构造新的 cylindrical trimmed face。
- 替换原区域并 sewing。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
- 支持 undo / redo。
- 不处理 Cone / Sphere / Torus / Freeform。
```

### 12.5 Stage 3C 开发提示词摘要

```text
本次实现 Stage 3C：ConeRegionMerge。

目标：
- 只处理 ConeLike candidate。
- 优先处理用户 Accepted candidate。
- 不处理 Rejected candidate。
- 检查 axis / apex / semi-angle compatibility。
- apex 附近高风险区域不自动合并。
- 构造新的 conical trimmed face。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
- 不处理自由曲面尖角。
```

### 12.6 Stage 3D 开发提示词摘要

```text
本次实现 Stage 3D：SphereRegionMerge。

目标：
- 只处理 SphereLike candidate。
- 优先处理用户 Accepted candidate。
- 不处理 Rejected candidate。
- 检查 center / radius compatibility。
- 避免处理跨 seam 或靠近 pole 的高风险区域。
- 构造新的 spherical trimmed face。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
- 不强行拟合高误差近似球面。
```

### 12.7 Stage 3E 开发提示词摘要

```text
本次实现 Stage 3E：TorusRegionMerge，可选。

目标：
- 只处理 TorusLike candidate。
- 优先处理用户 Accepted candidate。
- 不处理 Rejected candidate。
- 检查 axis / center / major radius / minor radius compatibility。
- 只处理低风险局部 torus patch。
- 不处理跨越大范围双周期 seam 的区域。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
```

---

## 13. 文档同步建议

新增或更新本文件后，建议同步更新：

```text
1. README.md
   在“开发文档阅读顺序”中加入 docs/merge_algorithm_roadmap.md。

2. docs/module_design.md
   在合并模块章节中增加：
   “具体合并算法阶段路线见 docs/merge_algorithm_roadmap.md。”

3. docs/implementation_status.md
   将 P1/P2 合并候选规划与真实合并路线细化为：
   - P1-A：SameDomainUnifier 参数与报告增强，重点验证 concat_bsplines=true
   - P1-B：MergeCandidate 数据结构
   - P1-C：MergePlanner + MergeRegionGrower 通用候选框架
   - P1-D：PlaneLike candidate 预览统计
   - P1-E：候选区域 GUI 高亮预览
   - P1-F：候选区域选择 / 接受 / 拒绝
   - P2-A：PlaneRegionMerge baseline
   - P2-B：CylinderRegionMerge
   - P2-C：SphereRegionMerge
   - P2-D：ConeRegionMerge，后置补全
   - P2-E：TorusRegionMerge，可选补全
   - P3：Freeform Candidate Detection
   - P4：Freeform B-spline / Plate Refit
```

---

## 14. 简短结论

当前路线不是：

```text
只做近似共面合并
```

而是：

```text
1. SameDomainUnifier 作为当前保守基础。
2. Stage 1 明确暴露 concat_bsplines 参数，并重点验证 concat_bsplines=true。
3. MergeCandidate / MergePlanner / MergeRegionGrower 作为通用候选框架。
4. Stage 2.5 在 GUI 中高亮候选区域，先验证候选是否合理。
5. Stage 2.6 支持用户选择、接受、拒绝候选区域，形成后续真实合并的人工控制入口。
6. Stage 3 拆分为 3-0/A/B/C/D/E：
   - 3-0 Analytic RegionMerger Framework Preparation，可选
   - 3A PlaneRegionMerge
   - 3B CylinderRegionMerge
   - 3C ConeRegionMerge，保留但后置
   - 3D SphereRegionMerge
   - 3E TorusRegionMerge，可选补全
7. 推荐近期主线是 3A → 3B → 3D → Stage 4 → Stage 5。
8. 3C / 3E 不删除，但不阻塞自由曲面路线。
9. Freeform Candidate Detection 提前进入，服务潮玩模型自由曲面。
10. B-spline / Plate Refit 在验证、报告和 rollback 完善后实施。
```

Stage 3 的重点不只是平面；平面只是建立真实 region merge baseline 的第一个低风险子阶段。  
ConeRegionMerge 和 TorusRegionMerge 仍然保留在路线中，但分别作为后置补全和可选补全处理。