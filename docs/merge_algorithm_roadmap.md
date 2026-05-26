# 合并算法路线图

> 文档定位：本文件用于指导 STEP-PATCH-OPTIMIZER 的 STEP/STP 曲面片合并算法阶段推进。  
> 架构边界遵循 `docs/module_design.md`。  
> 当前实现进度、待办状态与验收记录放在 `docs/implementation_status.md`。  
> 本文件只描述合并算法路线、阶段目标、实现边界、输入输出和验收标准。  
> 本版本为**代码对齐版**：已按当前代码中 Stage 3-0 与 Stage 3A 强化版实现修订；后续新代码仍应先补文档再实现。

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
5. Stage 3A PlaneRegionMerge 强化版已经进入真实 region merge；但当前候选层仍以 PlaneLike 为主，缺少多类型解析图元候选探测。
6. 当前 PlaneRegionMerge 采用 ReShape 替换 + boundary edge same-domain cleanup，并报告 BRepCheck 状态。
7. 潮玩模型常见自由曲面碎片化尚未解决，SurfaceRefitter 暂不具备直接落地条件。
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

## 6.7 Stage 2.7：Face / Candidate Inspect

### 6.7.1 目标

Stage 2.7 的目标不是新增候选类型，也不是执行真实合并，而是补齐交互式检查能力：

```text
用户点击模型上的某个 face
→ 系统自动反查 FaceId
→ 判断该 face 是否属于当前候选区域
→ 如果属于候选，显示 candidate 信息
→ 如果不属于候选，显示基础 inspect 信息
```

该阶段解决的问题是：

```text
1. 用户不应手动输入或理解 face id。
2. 用户应通过点击模型直接知道某个面是否属于候选。
3. 当前候选没有覆盖某个区域时，系统至少应说明：
   - 该 face 的拓扑编号；
   - surface type；
   - 是否属于已生成候选；
   - 是否被 Top N 显示过滤；
   - 是否邻接 protected edge；
   - 是否可能因为 min_region_faces / PlaneLike 限制未成为候选。
```

### 6.7.2 实现边界

允许：

```text
1. 扩展 Viewer 点击 face 后的检查信息。
2. 在 InspectPanel / ReportPanel 显示 face inspect 结果。
3. 显示 face 所属 candidate id/type/status/risk/metrics。
4. 对不属于候选的 face，显示 surface type 和基础拓扑上下文。
5. 不修改 TopoDS_Shape。
6. 不执行任何真实合并。
```

不允许：

```text
1. 不实现 Cylinder / Sphere / Cone / Torus 真实合并。
2. 不修改 Stage 3A PlaneRegionMerge。
3. 不强行生成多类型候选。
4. 不把 inspect 结果写入 ShapeDocument。
5. 不要求用户输入 face id。
```

### 6.7.3 推荐信息结构

可新增轻量结构：

```cpp
enum class FaceInspectCandidateState {
    InVisibleCandidate,
    InHiddenCandidate,
    InCandidateButNotDisplayed,
    NotInCandidate
};

struct FaceInspectInfo {
    FaceId face_id = invalidFaceId;

    std::string surface_type; // Plane / Cylinder / Cone / Sphere / Torus / BSpline / Other / Unknown

    FaceInspectCandidateState candidate_state = FaceInspectCandidateState::NotInCandidate;
    int candidate_id = -1;
    MergeCandidateType candidate_type = MergeCandidateType::Unknown;
    MergeCandidateStatus candidate_status = MergeCandidateStatus::Pending;
    MergeRiskLevel risk_level = MergeRiskLevel::Low;

    int candidate_face_count = 0;
    int candidate_boundary_edge_count = 0;
    int candidate_internal_edge_count = 0;

    bool adjacent_to_protected_edge = false;
    bool adjacent_to_locked_edge = false;
    int adjacent_protected_edge_count = 0;

    double max_normal_angle_deg = 0.0;
    double max_distance = 0.0;
};
```

该结构可以先只在 GUI 层运行时使用，不需要进入 `ShapeDocument`。

### 6.7.4 GUI 行为

推荐行为：

```text
1. 用户点击“预览合并”生成候选。
2. 用户切换到“候选区域选择”或“Inspect face”模式。
3. 用户点击任意 face。
4. 如果该 face 属于候选：
   - Viewer 高亮该 candidate；
   - InspectPanel 显示 candidate id/type/status/metrics。
5. 如果该 face 不属于候选：
   - Viewer 可短暂高亮该 face；
   - InspectPanel 显示 surface type、邻接 protected edge、是否可能被 Top N 过滤等信息。
```

### 6.7.5 验收标准

```text
1. 用户不需要知道 face id。
2. 点击属于 candidate 的 face 时，可以显示 candidate 详情。
3. 点击不属于 candidate 的 face 时，可以显示 surface type 和基础原因。
4. Inspect 不修改 B-rep。
5. Inspect 不影响 Stage 3A PlaneRegionMerge。
6. 现有候选预览、接受、拒绝、隐藏、恢复功能不被破坏。
```

---

## 6.8 Stage 2.8：Analytic Primitive Candidate Detection / Type Probing

### 6.8.1 目标

Stage 2.8 的目标是扩展候选识别层，而不是执行真实合并。

当前 Stage 2 只稳定生成 `PlaneLike` candidate。Stage 2.8 需要补充解析图元候选探测：

```text
PlaneLike，已有
CylinderLike，新增候选探测
SphereLike，新增候选探测
ConeLike / FrustumLike，新增候选探测
TorusLike，可先预留或低优先级
FreeformLike / Unknown，可作为分类结果预留
```

该阶段回答：

```text
某个区域不是 PlaneLike，那么它更可能是什么？
它是否可以作为后续 Stage 3B / 3D / 3C 的候选？
```

### 6.8.2 关键原则

```text
1. 只生成候选，不修改 B-rep。
2. 只做可视化预览，不执行真实合并。
3. 不新增 CylinderRegionMerge / SphereRegionMerge / ConeRegionMerge 的真实拓扑替换。
4. 不改变 Stage 3A PlaneRegionMerge。
5. 每个新增候选类型都必须有风险等级和 reject reason。
6. 候选探测应服务后续 Stage 3B / 3D / 3C。
```

### 6.8.3 推荐拆分

Stage 2.8 不建议一次做完所有类型。推荐子阶段：

```text
Stage 2.8A：Surface Type Probe
  对每个 face 识别底层 BRep surface type：
  Plane / Cylinder / Cone / Sphere / Torus / BSpline / Other。

Stage 2.8B：CylinderLike Candidate Detection
  基于同轴、同半径、邻接关系和 protectedEdges 生成 CylinderLike candidates。

Stage 2.8C：SphereLike Candidate Detection
  基于同球心、同半径、邻接关系和 protectedEdges 生成 SphereLike candidates。

Stage 2.8D：ConeLike / FrustumLike Candidate Detection
  基于同轴、半角、apex/reference radius、邻接关系和 protectedEdges 生成 ConeLike candidates。

Stage 2.8E：TorusLike Candidate Detection，可选
  仅在有明确 torus 样例时实现。
```

### 6.8.4 Candidate 检测输入

```text
ShapeDocument
TopologyGraph
FeatureEdgeDetectionResult
lockedEdges
protectedEdges
MergePlannerOptions
```

`protectedEdges` 仍然由：

```text
自动 feature edges + 用户 lockedEdges
```

构成。新增候选类型同样不能跨越 protectedEdges。

### 6.8.5 Candidate 输出

仍然输出 `MergeCandidate`，但 `candidate_type` 不再只有 PlaneLike：

```cpp
MergeCandidateType::CylinderLike
MergeCandidateType::SphereLike
MergeCandidateType::ConeLike
MergeCandidateType::TorusLike
```

推荐补充或复用字段：

```text
candidate_id
candidate_type
faces
boundary_edges
internal_edges
blocked_edges
protected_edges
face_count
boundary_edge_count
internal_edge_count
total_area
fit_error
risk_level
status
```

如需 primitive-specific 参数，可以先放入轻量诊断结构，不要急于污染 `MergeCandidate` 主结构。

### 6.8.6 验收标准

```text
1. PlaneLike 既有行为不回退。
2. 至少能识别一种新增解析图元候选，优先 CylinderLike 或 ConeLike/FrustumLike。
3. 新增候选不跨越 protectedEdges / lockedEdges。
4. 新增候选不会修改 ShapeDocument。
5. GUI / Report 能显示 candidate type。
6. 用户可以在 Viewer 中区分不同 candidate type。
7. 现有 Stage 3A PlaneRegionMerge 仍只处理 PlaneLike。
8. 新增 Cylinder/Sphere/Cone/Torus candidate 不会被误交给 PlaneRegionMerger。
```

---

## 6.9 Stage 2.9：Multi-type Candidate Preview

### 6.9.1 目标

Stage 2.9 的目标是让多类型候选可视化、可筛选、可解释。该阶段是通用多类型候选展示框架，即使某些类型当前数量为 0，也必须预留显示、统计和过滤通道。该阶段应作为通用多类型候选展示框架实现，即使某些类型当前数量为 0，也要预留显示、统计和过滤通道。

在 Stage 2.8 生成多类型 candidates 后，Stage 2.9 负责 GUI 呈现：

```text
按类型显示候选
按风险显示候选
按状态显示候选
点击 face 显示其候选类型与原因
```

### 6.9.2 推荐 GUI 能力

```text
1. 显示全部 PlaneLike candidates。
2. 显示全部 CylinderLike candidates。
3. 显示全部 SphereLike candidates。
4. 显示全部 ConeLike / FrustumLike candidates。
5. 显示全部 TorusLike candidates，可选；当前可能为 0。
6. 预留 FreeformG1 / FreeformG2 / Unknown candidates 的统计和显示入口。
7. 显示全部非隐藏 candidates。
7. 只显示 Accepted / Pending / Rejected / Hidden。
8. 按 candidate id 高亮。
9. 点击 face 反查所属 candidate。
10. ReportPanel 显示按类型统计：
    - PlaneLike count
    - CylinderLike count
    - SphereLike count
    - ConeLike count
    - TorusLike count
    - TorusLike count
    - FreeformG1 count
    - FreeformG2 count
    - Unknown count
```

### 6.9.3 Viewer 显示建议

```text
不同 candidate type 使用不同颜色族。
同一类型内可以使用深浅变化或循环色。
Rejected / Hidden 不默认显示。
当前选中 candidate 使用更粗边框或更高亮覆盖。
```

### 6.9.4 与 Stage 3 的边界

```text
1. Stage 2.9 只显示候选，不合并候选。
2. Stage 2.9 不新增真实 B-rep 替换。
3. Stage 2.9 不调用 CylinderRegionMerger / SphereRegionMerger / ConeRegionMerger。
4. Stage 3A 仍只处理 PlaneLike。
5. 后续 Stage 3B / 3D / 3C 再读取对应类型 candidate 做真实合并。
```

### 6.9.5 验收标准

```text
1. 多类型候选能在 Viewer 中区分显示。
2. ReportPanel 能按 candidate type 输出统计。
3. ModelTreePanel 能显示候选类型统计。
4. 用户点击 face 后能看到 candidate type / status / metrics。
5. Hidden candidate 不默认显示。
6. Rejected candidate 不进入后续真实合并。
7. 原 Stage 2.5 / Stage 2.6 / Stage 3A 不回退。
```

---

## 6.10 Stage 2.8 当前实现状态与 B-spline 事实修正

### 6.10.1 新事实

通过 Stage 2.7 / Surface Type Probe 检查后，当前样例中大量视觉上接近圆柱、圆锥、圆台、球面或过渡面的区域，底层 `SurfaceType` 主要为：

```text
B-spline
```

这说明当前 STEP/STP 模型并不是典型“解析 CAD 面片集合”，而更接近：

```text
AutoSurface / 逆向曲面化结果
→ 大量 Geom_BSplineSurface patch
→ 视觉上像解析图元
→ kernel 层面却不是 native Cylinder / Cone / Sphere
```

因此，后续路线必须从：

```text
识别 OCCT 原生解析面
```

调整为：

```text
从 B-spline patch 中反推近似解析图元
```

该事实对 Stage 2.8 / 2.9 / Stage 3 有直接影响。

### 6.10.2 当前 Stage 2.8 状态

| 类型                             | 当前状态                                                     |              默认 GUI 预览 | 当前限制                            |
| -------------------------------- | ------------------------------------------------------------ | -------------------------: | ----------------------------------- |
| PlaneLike                        | 已支持，原有平面近似区域生长                                 |                       开启 | 仍适合近似共面 patch                |
| CylinderLike                     | 已支持基础版，仅识别 OCCT 原生 Cylinder 面                   |                       开启 | 当前样例多为 B-spline，因此通常为 0 |
| SphereLike                       | 已支持增强版，识别 OCCT 原生 Sphere，也支持保守的 B-spline / Bezier 球面近似 |                       开启 | 当前最有价值的非平面候选之一        |
| ConeLike                         | 已支持基础版，仅识别 OCCT 原生 Cone 面                       |                       开启 | 当前样例多为 B-spline，因此通常为 0 |
| TorusLike                        | 只有开关和空 stub，目前不生成候选                            | 开启了开关，但不会产生候选 | 保留为后续可选高级图元              |
| FreeformG1 / FreeformG2          | 只预留类型，没有真实检测                                     |                     未开启 | 后续 Stage 4 / Stage 5 再处理       |
| Unknown / B-spline analytic-like | 当前可通过 Surface Type Probe 看见，但未形成稳定候选类型     |        可在 Stage 2.9 预留 | 后续需要近似解析图元探测            |

### 6.10.3 当前判断

当前继续推进 Stage 2.9 是合理的，但 Stage 2.9 的定位必须收窄为：

```text
多类型候选展示框架
```

而不是：

```text
多类型候选检测成熟验证
```

理由：

```text
1. 当前已经有 PlaneLike 和 SphereLike 两类有效候选，可用于验证多类型显示框架。
2. CylinderLike / ConeLike 当前为 0 是合理现象，因为底层多为 B-spline。
3. Stage 2.9 可以先建立类型统计、过滤、颜色、点击查看、后续类型预留。
4. 后续 B-spline CylinderLike / ConeLike 增强完成后，可直接复用 Stage 2.9 的展示框架。
```

明确限制：

```text
1. Stage 2.9 不负责提升 CylinderLike / ConeLike 检出率。
2. Stage 2.9 必须允许某些类型数量为 0。
3. Stage 2.9 必须预留 TorusLike、FreeformG1、FreeformG2、Unknown / B-spline analytic-like 的显示和统计通道。
4. Stage 2.9 完成后，不应直接进入 Stage 3B / Stage 3C 真实合并。
5. 下一步应优先补 B-spline CylinderLike approximate detection 和 B-spline ConeLike / FrustumLike approximate detection。
```

### 6.10.4 推荐后续路线

```text
Stage 2.9：Multi-type Candidate Preview
  建立多类型候选显示、统计、筛选、点击查看框架。
  预留 PlaneLike / CylinderLike / SphereLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 / Unknown。
  正确处理 count == 0 的类型。

Stage 2.8 Enhancement A：B-spline CylinderLike Approximate Detection
  从 B-spline / Bezier / SurfaceOfRevolution patch 中保守识别近似圆柱候选。
  只生成候选，不做真实合并。

Stage 2.8 Enhancement B：B-spline ConeLike / FrustumLike Approximate Detection
  从 B-spline / Bezier / SurfaceOfRevolution patch 中保守识别近似圆锥 / 圆台候选。
  只生成候选，不做真实合并。

Stage 3D：SphereRegionMerge
  由于 SphereLike 已有 B-spline 近似增强，Stage 3D 可能比 Stage 3B / 3C 更早具备推进条件。

Stage 3B：CylinderRegionMerge
  等 CylinderLike approximate detection 稳定后推进。

Stage 3C：ConeRegionMerge
  后置推进，避免误伤自由曲面尖角、发尖和高曲率装饰区域。

Stage 4：Freeform Candidate Detection
  面向不能稳定归入解析图元的 B-spline patch 群。

Stage 5：Freeform B-spline / Plate Refit
  面向高可信自由曲面候选区域做局部重拟合。
```

### 6.10.5 Stage 2.9 边界

允许：

```text
1. 按 candidate_type 统计数量。
2. 按 candidate_type 过滤显示。
3. 不同 candidate_type 使用不同颜色族。
4. 点击 face 显示 candidate type / surface type / status / risk / metrics。
5. ReportPanel / ModelTreePanel 显示多类型统计。
6. 对 TorusLike / FreeformG1 / FreeformG2 / Unknown / B-spline analytic-like 预留显示和统计逻辑。
7. 正确处理 CylinderLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 count == 0。
```

不允许：

```text
1. 不增强 CylinderLike / ConeLike 检测算法。
2. 不实现 CylinderRegionMerge / ConeRegionMerge / SphereRegionMerge。
3. 不构造新 face。
4. 不替换 TopoDS_Shape。
5. 不改变 Stage 3A PlaneRegionMerge。
6. 不把非 PlaneLike candidate 送入 PlaneRegionMerger。
```

### 6.10.6 Stage 2.8 Enhancement 边界

B-spline CylinderLike / ConeLike 增强属于 Stage 2.8 Enhancement，不属于 Stage 2.9：

```text
1. 只增强候选检测，不执行真实合并。
2. 允许对 B-spline / Bezier / SurfaceOfRevolution 做保守近似判断。
3. 必须设置 fit_error 和 risk_level。
4. 必须允许检测失败或不生成候选，不能强行拟合。
5. 不能误伤自由曲面尖角、发尖、高曲率装饰区域。
6. Enhancement 完成后，仍由 Stage 2.9 负责可视化预览。
```

### 6.10.7 研究意义修正

该事实使本项目更接近以下研究问题：

```text
从逆向工程生成的 B-spline patch STEP 中，
识别潜在解析图元结构，
并进行特征感知的候选区域合并与边界优化。
```

这比单纯调用 OCCT native surface type 更有研究价值。


---

## 7. Stage 3：Analytic Primitive Region Merge

### 7.1 当前定位

Stage 3 是解析基础图元真实区域合并阶段。

当前代码状态：

```text
Stage 3-0：已完成
  已有 RegionMergeResult / RegionMergeOptions / RegionMergeFailureReason。
  已有 Plane / Cylinder / Cone / Sphere / Torus RegionMerger stub。
  stub 可返回 NotImplemented / UnsupportedCandidateType / RejectedCandidate / HiddenCandidate。
  stub 不修改 ShapeDocument。

Stage 3A：已完成强化版
  PlaneRegionMerger 已不再是 stub。
  已支持 PlaneLike candidate 的真实平面区域合并。
  已支持单候选合并、Accepted 批量合并、全部可合并平面候选实验合并。
  已接入 CommandHistory、undo/redo、AppController、MainWindow GUI 和测试。

Stage 3B：未完成
  CylinderRegionMerger 当前仍处于 stub / 后续扩展阶段。

Stage 3C：未完成，后置
  ConeRegionMerger 当前仍处于 stub / 后续扩展阶段。

Stage 3D：未完成
  SphereRegionMerger 当前仍处于 stub / 后续扩展阶段。

Stage 3E：未完成，可选
  TorusRegionMerger 当前仍处于 stub / 可选扩展阶段。
```

本章节按当前代码实现对齐。  
与早期文档相比，本版本接受当前 Stage 3A 的实际实现策略：

```text
1. PlaneRegionMerge 使用 BRepTools_ReShape 做 region replacement。
2. PlaneRegionMerge 使用 ShapeUpgrade_UnifySameDomain 做边界简化 / cleanup。
3. 当前实现没有显式单独执行 BRepBuilderAPI_Sewing 作为主流程步骤。
4. 当前实现将 BRepCheck 状态写入报告；BRepCheck warning 不一定导致 success=false。
5. 当前 GUI 已存在“合并当前平面候选”“合并所有已接受平面候选”“一键合并全部可合并平面候选”三个入口。
```

### 7.2 Stage 3 总体执行链路

当前 Stage 3A 代码中的真实执行链路为：

```text
MainWindow
→ AppController::mergePlaneCandidate / mergePlaneCandidates
→ PlaneRegionMergeCommand / PlaneRegionBatchMergeCommand
→ PlaneRegionMerger::merge / mergeBatch
→ candidate 校验
→ plane estimation
→ deviation 计算
→ boundary wire 构造
→ planar trimmed face 构造
→ BRepTools_ReShape 替换区域
→ ShapeUpgrade_UnifySameDomain 边界清理
→ ShapeValidator / BRepCheck
→ RegionMergeResult
→ CommandHistory
→ Viewer / ModelTree / Report 刷新
```

与 same-domain 路径的边界：

```text
原“执行合并”：
  仍然走 SameDomainUnifier / MergePatchCommand。

平面候选合并：
  走 PlaneRegionMerger / PlaneRegionMergeCommand。
```

两条路径不得混淆。

---

### 7.3 Stage 3-0：Analytic RegionMerger Framework Preparation

#### 7.3.1 已实现目标

Stage 3-0 已完成框架准备：

```text
1. 统一 RegionMergeResult。
2. 统一 RegionMergeOptions。
3. 统一 RegionMergeFailureReason。
4. 预留 Plane / Cylinder / Cone / Sphere / Torus merger。
5. 预留 RegionMergeStub。
6. stub 不修改模型。
7. stub 测试已接入测试入口。
```

#### 7.3.2 当前数据结构

当前 `RegionMergeFailureReason` 包含：

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

当前 `RegionMergeResult` 包含：

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

    double plane_normal_x = 0.0;
    double plane_normal_y = 0.0;
    double plane_normal_z = 0.0;

    bool brep_check_valid = false;
    ShapeDocument document;
};
```

当前 `RegionMergeOptions` 包含：

```cpp
struct RegionMergeOptions {
    double max_deviation = 0.02;
    int min_region_faces = 2;
    bool allow_pending_candidate = false;
    bool require_accepted_candidate = true;
};
```

#### 7.3.3 当前 stub 行为

当前 stub 行为定义如下：

```text
1. candidate type 不匹配：
   返回 UnsupportedCandidateType。

2. candidate.valid == false：
   返回 InvalidCandidate。

3. candidate.status == Rejected：
   返回 RejectedCandidate。

4. candidate.status == Hidden：
   返回 HiddenCandidate。

5. Pending candidate 在 require_accepted_candidate=true 且 allow_pending_candidate=false 时：
   返回 NotSupported。

6. face_count 不足：
   返回 InsufficientFaces。

7. 类型正确但真实合并未实现：
   返回 NotImplemented。

8. 调用后 ShapeDocument stats 不变。
```

#### 7.3.4 Stage 3-0 验收状态

```text
1. 构建文件已接入。
2. stub 测试已接入。
3. NotImplemented 路径已覆盖。
4. UnsupportedCandidateType 路径已覆盖。
5. Rejected / Hidden 路径已覆盖。
6. stats 不变已覆盖。
```

Stage 3-0 可视为完成。

---

### 7.4 Stage 3A：PlaneRegionMerge 强化版

#### 7.4.1 目标

对 `PlaneLike` candidate 执行真实 B-rep 区域合并：

```text
多个近似共面的相邻 faces
→ 一个较大的 planar trimmed face
```

当前 Stage 3A 已实现强化版，不再是 stub。

#### 7.4.2 当前新增 / 修改模块

当前 Stage 3A 已涉及：

```text
src/merge/PlaneRegionMerger.h
src/merge/PlaneRegionMerger.cpp

src/command/PlaneRegionMergeCommand.h
src/command/PlaneRegionMergeCommand.cpp

src/command/PlaneRegionBatchMergeCommand.h
src/command/PlaneRegionBatchMergeCommand.cpp

src/app/AppController.h
src/app/AppController.cpp

src/app/MainWindow.cpp

tests/test_plane_region_merger.cpp
tests/test_plane_region_merge_command.cpp
```

CMake 中已接入：

```text
PlaneRegionMerger.cpp
PlaneRegionMergeCommand.cpp
PlaneRegionBatchMergeCommand.cpp
test_plane_region_merger.cpp
test_plane_region_merge_command.cpp
```

#### 7.4.3 当前接口

`PlaneRegionMerger` 当前提供：

```cpp
class PlaneRegionMerger {
public:
    RegionMergeResult merge(
        const ShapeDocument& document,
        const MergeCandidate& candidate,
        const PlaneRegionMergeOptions& options) const;

    RegionMergeResult mergeBatch(
        const ShapeDocument& document,
        const std::vector<MergeCandidate>& candidates,
        const PlaneRegionMergeOptions& options) const;
};
```

当前 `PlaneRegionMergeOptions`：

```cpp
struct PlaneRegionMergeOptions : RegionMergeOptions {
    double normal_angle_tolerance_degrees = 3.0;
    double plane_distance_tolerance = 0.01;
};
```

#### 7.4.4 候选过滤规则

当前单候选和批量候选都会经过保守过滤。

候选必须满足：

```text
1. document.hasShape() == true。
2. candidate.valid == true。
3. candidate.candidate_type == PlaneLike。
4. candidate.status != Rejected。
5. candidate.status != Hidden。
6. candidate.status == Pending 时：
   - 若 require_accepted_candidate=true 且 allow_pending_candidate=false，则拒绝。
   - GUI 当前合并当前候选和一键批量合并会设置 allow_pending_candidate=true。
7. candidate.face_count >= min_region_faces。
8. candidate.faces.size() >= min_region_faces。
9. candidate.boundary_edges 非空。
10. candidate.faces 引用的 face id 必须存在。
11. candidate.boundary_edges 引用的 edge id 必须存在。
12. candidate.internal_edges 引用的 edge id 必须存在。
13. candidate.internal_edges 不能包含 candidate.protected_edges。
```

注意：

```text
当前 protected edge conflict 主要检查 candidate.internal_edges 与 candidate.protected_edges 的交集。
如果未来要更严格处理 lockedEdges，需要在 MergeCandidate 中保证 locked/protected 信息完整进入 candidate.protected_edges。
```

#### 7.4.5 当前几何算法流程

当前 `PlaneRegionMerger::merge()` 的真实流程：

```text
1. 初始化 RegionMergeResult，并记录 before stats。
2. validateCandidate() 校验候选状态、类型、face/edge id、protected internal edge。
3. estimatePlaneFromCandidate()：
   - 对候选 face 中心进行采样；
   - 若 face 是解析平面，可直接获取 plane；
   - 对 NURBS-backed planar face，也通过中心点和法向估计平面；
   - 对所有 face normal 做平均；
   - 法向差超过 normal_angle_tolerance_degrees 时失败。
4. computeDeviation()：
   - 对候选 face 中心点到目标 plane 的距离计算 max / mean / rms；
   - 若 max_deviation 超过 options.max_deviation 或 plane_distance_tolerance，则失败。
5. makeBoundaryWire()：
   - 根据 candidate.boundary_edges 尝试排序形成一个闭合 wire；
   - 使用 ShapeFix_Wire 修复；
   - 不闭合则失败。
6. BRepBuilderAPI_MakeFace：
   - 基于 Geom_Plane 和 boundary wire 构造 planar trimmed face。
7. hasSingleOuterWire()：
   - 当前只接受单一稳定 outer wire。
8. applyPreparedMerges()：
   - 使用 BRepTools_ReShape；
   - 用新 face 替换 region 中第一个 face；
   - 删除 region 中剩余 faces。
9. simplifyPreparedBoundaryEdges()：
   - 使用 ShapeUpgrade_UnifySameDomain；
   - 保留非候选 boundary 的其他 edges；
   - 对候选区域相关边界做同域清理。
10. 构造新的 ShapeDocument。
11. ShapeValidator / BRepCheck。
12. 计算 face / edge reduction ratio。
13. 如果拓扑不可用、solid count 被破坏、face count 未减少，则失败。
14. 成功时返回包含新 ShapeDocument 的 RegionMergeResult。
```

#### 7.4.6 当前拓扑替换策略

当前代码优先采用：

```text
BRepTools_ReShape
→ Replace 第一个 region face 为 merged planar face
→ Remove 其余 region faces
→ ShapeUpgrade_UnifySameDomain 做 boundary cleanup
→ ShapeValidator
```

当前文档按代码对齐：

```text
1. 当前 Stage 3A 主流程没有显式单独执行 BRepBuilderAPI_Sewing。
2. 当前 RegionMergeFailureReason 中仍保留 SewingFailed，供后续更严格 sewing 流程使用。
3. 当前实际 cleanup 步骤是 ShapeUpgrade_UnifySameDomain。
4. 如果后续要改成显式 sewing，应先更新本文档，再改代码。
```

#### 7.4.7 BRepCheck 策略

当前代码策略：

```text
1. ShapeValidator 会执行 BRepCheck。
2. result.brep_check_valid 会记录 BRepCheck 结果。
3. BRepCheck warning / failure 当前不一定导致 result.success=false。
4. 当前成功条件主要包括：
   - merged shape 非空；
   - topology usable；
   - solid count preserved；
   - face_count_after < face_count_before。
```

因此，当前 Stage 3A 验收按代码对齐为：

```text
1. 合并后必须执行 ShapeValidator / BRepCheck。
2. BRepCheck 状态必须写入 RegionMergeResult 和 GUI 报告。
3. BRepCheck 不通过时，当前允许以 warning 形式报告，不强制判定失败。
4. 如果后续要强制 BRepCheck 通过，必须先修改本文档，再修改代码。
```

#### 7.4.8 GUI 入口

当前 GUI 已实现三个入口：

```text
合并当前平面候选
合并所有已接受平面候选
一键合并全部可合并平面候选
```

语义如下：

```text
合并当前平面候选：
  处理当前选中 / 高亮的 PlaneLike candidate。
  当前会设置 allow_pending_candidate=true、require_accepted_candidate=false。
  因此 Pending candidate 也可在用户显式选择后执行。

合并所有已接受平面候选：
  只收集 status == Accepted 的 PlaneLike candidate。
  走 PlaneRegionBatchMergeCommand。

一键合并全部可合并平面候选：
  收集 valid 且 type == PlaneLike 且 status != Rejected/Hidden 的 candidate。
  包含 Pending 和 Accepted。
  这是当前代码已经存在的实验性批量入口。
```

文档对“一键合并全部可合并平面候选”的定位：

```text
1. 它属于 Stage 3A 强化版中的实验入口。
2. 它不是最保守推荐入口。
3. 正式实验数据优先使用“合并当前平面候选”或“合并所有已接受平面候选”。
4. 若一键合并导致质量下降，应通过 Rejected / Hidden / Accepted 状态先筛选候选。
```

#### 7.4.9 Command / undo / redo

当前已实现：

```text
PlaneRegionMergeCommand
PlaneRegionBatchMergeCommand
```

二者均满足：

```text
1. execute() 中调用 PlaneRegionMerger。
2. 成功后更新 CommandContext.document。
3. 失败后不修改 CommandContext.document。
4. undo() 恢复 beforeDocument。
5. redo() 恢复 afterDocument。
6. 执行成功后清空 featureEdges / validationReport。
7. 执行成功后 dirty=true。
```

#### 7.4.10 AppController 接口

当前已实现：

```cpp
RegionMergeResult mergePlaneCandidate(
    const MergeCandidate& candidate,
    const PlaneRegionMergeOptions& options);

RegionMergeResult mergePlaneCandidates(
    const std::vector<MergeCandidate>& candidates,
    const PlaneRegionMergeOptions& options);
```

注意：

```text
当前 AppController 接收 MergeCandidate 对象，而不是 candidateId。
候选查找和筛选主要在 MainWindow 运行时状态中完成。
```

#### 7.4.11 当前不支持 / 高风险边界

当前 Stage 3A 仍然不应视为通用完备 B-rep region replacement。

当前边界：

```text
1. 只处理 PlaneLike candidate。
2. 只稳定支持单 outer boundary wire。
3. 不支持复杂多洞区域。
4. 不支持不闭合 boundary loop。
5. 不支持复杂 inner wire 管理。
6. 不处理 Cylinder / Cone / Sphere / Torus / Freeform。
7. 不显式执行独立 sewing 流程。
8. BRepCheck warning 当前允许作为 warning 进入报告。
9. 批量合并会跳过无法 prepare 的候选和重叠 face 候选。
10. 一键合并全部可合并平面候选属于实验入口。
```

#### 7.4.12 当前测试覆盖

当前测试已覆盖：

```text
test_plane_region_merger.cpp:
  1. 非 PlaneLike candidate 被拒绝。
  2. Rejected candidate 被拒绝。
  3. Hidden candidate 被拒绝。
  4. face_count 不足被拒绝。
  5. protected internal edge 被拒绝。
  6. invalid boundary 不污染 stats。
  7. 简单共面区域真实合并成功。
  8. NURBS-backed planar region 合并成功。
  9. split solid top fixture 合并后 solid count 保持。

test_plane_region_merge_command.cpp:
  1. Command 失败不污染 document。
  2. PlaneRegionMergeCommand 成功后支持 undo / redo。
  3. PlaneRegionBatchMergeCommand 成功后支持 undo / redo。
```

#### 7.4.13 Stage 3A 当前验收状态

按当前代码优先文档，Stage 3A 强化版可标记为：

```text
已实现，待真实复杂 STEP 样例持续验证。
```

完成项：

```text
1. 真实 PlaneLike region merge 已实现。
2. 单候选合并已实现。
3. Accepted 批量合并已实现。
4. 全部可合并平面候选实验合并已实现。
5. GUI 入口已实现。
6. AppController 接口已实现。
7. Command / undo / redo 已实现。
8. 基础单元测试已实现。
9. BRepCheck 状态已报告。
```

保留 TODO：

```text
1. 在真实 STEP/STP 潮玩样例上持续验证。
2. 对复杂多洞区域提供更明确失败报告。
3. 如果后续要求严格 BRepCheck 通过，需要先改文档再收紧代码。
4. 如果后续需要显式 sewing，需要先改文档再实现。
5. 一键合并全部可合并平面候选后续可增加确认框或实验模式标记。
```

---

### 7.5 Stage 3B：CylinderRegionMerge

#### 7.5.1 当前状态

当前 `CylinderRegionMerger` 仍属于后续阶段。  
在 Stage 3-0 中只应保留 stub 行为，不得伪装为真实圆柱区域合并。

#### 7.5.2 目标

对 `CylinderLike` candidate 执行真实 B-rep 区域合并：

```text
多个近似同圆柱面的相邻 faces
→ 一个较大的 cylindrical trimmed face
```

#### 7.5.3 前置条件

```text
1. Stage 3A PlaneRegionMerge 在真实样例上稳定。
2. CylinderLike candidate detection 已稳定。
3. Command / rollback / undo / redo 路径复用 Stage 3A。
4. 需要明确处理圆柱参数域 seam。
```

#### 7.5.4 暂不做内容

```text
1. 当前不得实现真实 CylinderRegionMerge，除非另开 Stage 3B 任务。
2. 不得在 Stage 3A 后续修补中顺手实现圆柱。
3. 不得把 CylinderLike candidate 交给 PlaneRegionMerger。
```

---

### 7.6 Stage 3C：ConeRegionMerge，后置

#### 7.6.1 当前状态

当前 `ConeRegionMerger` 仍属于后置扩展阶段。  
它容易误伤自由曲面尖角、发尖和高曲率装饰区域，因此不进入近期稳定主线。

#### 7.6.2 目标

对 `ConeLike` candidate 执行真实 B-rep 区域合并：

```text
多个近似同圆锥面的相邻 faces
→ 一个较大的 conical trimmed face
```

#### 7.6.3 后置原因

```text
1. cone apex 附近参数域退化。
2. 潮玩模型尖角不等于规则圆锥。
3. 需要更可靠的 protectedEdges 和尖角保护。
4. 需要能区分规则锥面和自由曲面尖角。
```

---

### 7.7 Stage 3D：SphereRegionMerge

#### 7.7.1 当前状态

当前 `SphereRegionMerger` 仍属于后续阶段。  
建议在 Stage 3B 之后推进。

#### 7.7.2 目标

对 `SphereLike` candidate 执行真实 B-rep 区域合并：

```text
多个近似同球面的相邻 faces
→ 一个较大的 spherical trimmed face
```

#### 7.7.3 前置条件

```text
1. Stage 3A 稳定。
2. Stage 3B 最好已完成或 Command/rollback 路径已足够稳定。
3. SphereLike candidate detection 已稳定。
4. 需要明确处理 sphere pole 和 seam。
```

---

### 7.8 Stage 3E：TorusRegionMerge，可选

#### 7.8.1 当前状态

当前 `TorusRegionMerger` 仍属于可选扩展阶段。  
只有在存在明确 torus patch 样例和工程需求时才建议实现。

#### 7.8.2 目标

对 `TorusLike` candidate 执行真实 B-rep 区域合并：

```text
多个近似同圆环面的相邻 faces
→ 一个较大的 toroidal trimmed face
```

#### 7.8.3 可选原因

```text
1. torus 参数域双周期。
2. trim curve 和 seam 处理复杂。
3. 很多圆角可能是 B-spline 过渡面，并非真 torus。
4. 没有明确样例时不应阻塞 Stage 4 / Stage 5。
```

---

### 7.9 Stage 3 推荐实现顺序

当前推荐顺序保持：

```text
1. Stage 3-0：已完成。
2. Stage 3A：已完成强化版，继续真实样例验证。
3. Stage 2.7：Face / Candidate Inspect。
4. Stage 2.8：Analytic Primitive Candidate Detection / Type Probing。
5. Stage 2.9：Multi-type Candidate Preview。
6. Stage 3B：CylinderRegionMerge。
7. Stage 3D：SphereRegionMerge。
5. Stage 4：Freeform Candidate Detection。
6. Stage 5：Freeform B-spline / Plate Refit。
7. Stage 3C：ConeRegionMerge，后置补全。
8. Stage 3E：TorusRegionMerge，可选补全。
```

说明：

```text
1. 3A 是当前真实 region merge baseline。
2. 3B / 3D 是近期更稳定的解析图元扩展。
3. 3C 保留，但后置，避免误伤尖角。
4. 3E 保留，但可选，不阻塞自由曲面路线。
```

### 7.10 Stage 3 总体验收标准，代码对齐版

当前 Stage 3A 的验收标准按代码对齐为：

```text
1. Stage 3-0 框架存在，并且 stub 不修改模型。
2. PlaneRegionMerger 能对简单 PlaneLike candidate 真实减少 face。
3. PlaneRegionMerger 能处理 NURBS-backed planar region。
4. PlaneRegionMerger 能保持 split solid top fixture 的 solid count。
5. Rejected / Hidden / 错误类型 / face 不足 / protected internal edge / invalid boundary 能失败返回。
6. 失败时不污染原 ShapeDocument。
7. PlaneRegionMergeCommand 支持 undo / redo。
8. PlaneRegionBatchMergeCommand 支持 undo / redo。
9. GUI 提供单候选、Accepted 批量、全部可合并平面候选实验入口。
10. 原 applyMerge() 仍走 SameDomainUnifier。
11. BRepCheck 状态进入 RegionMergeResult 和报告。
12. 真实 STEP/STP 导出后二次读取仍需作为手动验证项持续记录。
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
6. Stage 3A：PlaneRegionMerge 强化版，已实现，继续真实样例验证。
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
7. Stage 3A：PlaneRegionMerge 强化版已实现，继续真实 STEP/STP 样例验证。
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
   - 3A PlaneRegionMerge，已实现强化版
   - 3B CylinderRegionMerge
   - 3C ConeRegionMerge，保留但后置
   - 3D SphereRegionMerge
   - 3E TorusRegionMerge，可选补全
7. 当前近期主线是：验证 3A 强化版 → Stage 2.9 多类型候选预览框架 → Stage 2.8 Enhancement Cylinder/Cone 近似检测 → 3B/3D/3C → Stage 4 → Stage 5。
8. 3C / 3E 不删除，但不阻塞自由曲面路线。
9. Freeform Candidate Detection 提前进入，服务潮玩模型自由曲面。
10. B-spline / Plate Refit 在验证、报告和 rollback 完善后实施。
```

Stage 3 的重点不只是平面；平面只是建立真实 region merge baseline 的第一个低风险子阶段。  
ConeRegionMerge 和 TorusRegionMerge 仍然保留在路线中，但分别作为后置补全和可选补全处理。