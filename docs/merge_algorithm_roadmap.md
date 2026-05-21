# 合并算法路线图

> 文档定位：本文件用于指导 STEP 曲面片合并算法的阶段推进。  
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

该方案只适合合并同一底层几何域上的相邻 faces。对于视觉上近似连续、但底层并非同一个 `Geom_Surface` 的碎片面，合并效果有限。

当前典型问题：

```text
1. face reduction 不到 10%～15% 时，继续调 same-domain 参数的收益有限。
2. concat_bsplines=true 主要可能减少 edge 数，不一定减少 face 数。
3. MergePlanner / MergeRegionGrower 已进入候选区域生成阶段，但第一版仅输出统计信息。
4. 候选区域尚未在 GUI Viewer 中可视化，难以判断候选是否合理。
5. 解析基础图元合并尚未实现。
6. 潮玩模型常见自由曲面碎片化尚未解决。
7. SurfaceRefitter 暂不具备直接落地条件。
```

---

## 2. 总体算法路线

合并算法按以下阶段推进：

```text
Stage 1: SameDomainUnifier Enhancement
         增强当前同域合并的参数、报告和实验能力。
         关键实验点是暴露 concat_bsplines 参数，并重点验证 concat_bsplines=true 的效果。

Stage 2: Generic Merge Candidate Framework
         实现通用候选区域框架，不绑定具体几何类型。
         第一版只生成候选 region，不修改 B-rep。

Stage 2.5: Candidate GUI Preview
         在 3D Viewer 中高亮候选区域，用于人工检查候选质量。
         支持候选浏览、筛选、全部显示开关和清除预览。

Stage 2.6: Candidate Selection / Rejection
         支持用户选择、接受、拒绝、隐藏候选区域。
         仍不执行真实 B-rep 合并。

Stage 3: Analytic Primitive Region Merge
         实现解析基础图元合并：Plane / Cylinder / Cone / Sphere / Torus。

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
5. 基础图元 Plane / Cylinder / Cone / Sphere 属于同一个解析图元阶段。
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
4. 不做 Cylinder / Cone / Sphere 合并。
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

建议引入候选类型：

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

建议引入风险等级：

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
6. 不做 Cylinder / Cone / Sphere 的真正拓扑替换。
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

推荐默认行为：

```text
默认模式：
  高亮最大候选区域，或高亮候选列表中的当前选中项。

辅助模式：
  高亮 Top N 候选区域，例如 Top 5 / Top 10。

实验模式：
  支持显示全部候选区域，但需要显式开关。
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

### 5.5 显示建议

```text
1. 候选 face 使用半透明覆盖或边界高亮。
2. 不要遮挡自动特征边和用户锁定边。
3. 用户锁定边的优先级高于候选高亮。
4. 自动特征边的显示优先级高于普通候选边界。
5. Top N 候选可以使用同一颜色加不同透明度，也可以使用少量离散颜色。
6. 若显示全部候选，应避免给每个候选随机生成难以复现的颜色。
```

### 5.6 不做内容

```text
1. 不接受/拒绝候选。
2. 不持久化候选状态。
3. 不执行 PlaneRegionMerge。
4. 不替换 B-rep。
5. 不实现 SurfaceRefitter。
6. 不修改 applyMerge() 的 same-domain 合并路径。
```

### 5.7 验收标准

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

在候选区域可视化基础上，支持用户对候选区域进行选择、接受、拒绝和隐藏，为后续 Stage 3 的真实合并提供人工筛选基础。

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
2. 用户可以接受 / 拒绝 / 隐藏候选区域。
3. 报告或候选列表能显示候选状态。
4. 状态变化不改变 face/edge 数。
5. 清除预览或打开新文件后候选状态清空。
6. Accepted candidates 能被后续 Stage 3A 读取或预留接口读取。
```

---

## 7. Stage 3：Analytic Primitive Region Merge

### 7.1 目标

在通用候选框架和候选可视化基础上，对解析基础图元候选区域执行真正的 B-rep 合并。

Stage 3 不是 Plane-only，而是解析图元合并阶段：

```text
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
```

### 7.2 总体执行流程

```text
1. 从 MergePlannerResult 中选择低风险 analytic primitive candidate。
2. 优先处理用户 Accepted candidate。
3. 根据 candidate_type 分派到具体 RegionMerger。
4. 提取 candidate.faces 对应的区域。
5. 提取区域外边界 boundary loop。
6. 构建目标解析曲面。
7. 将 boundary loop 投影到目标曲面参数域。
8. 构建新的 trimmed face。
9. 替换原 region。
10. sewing。
11. ShapeValidator 检查。
12. ErrorMetric 计算偏差。
13. 成功则提交，失败则 rollback。
```

### 7.3 Stage 3A：PlaneRegionMerge

优先处理：

```text
多个近似共面的相邻 faces
→ 合并为一个较大的 planar face
```

判断条件：

```text
normal angle < threshold
point-to-plane distance < threshold
shared edge not protected
region face count >= min_region_faces
candidate status != Rejected
```

适用区域：

```text
底面
平台
切平面
机械式平面结构
潮玩底座中的平面块
```

不做内容：

```text
1. 不处理 Cylinder / Cone / Sphere。
2. 不处理自由曲面。
3. 不处理复杂多洞区域。
4. 不做大范围 patch layout 重构。
```

验收标准：

```text
1. 对明确共面的碎片区域，face 数明显下降。
2. 合并后 BRepCheck 通过。
3. 导出 STEP 二次读取通过。
4. max deviation 小于阈值。
5. feature edge preserve rate 满足阈值。
6. undo / redo 正常。
7. 用户 Rejected candidate 不会被应用。
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
5. Stage 3A：PlaneRegionMerge。
6. Stage 3B：CylinderRegionMerge。
7. Stage 3D：SphereRegionMerge。
8. Stage 4：Freeform Candidate Detection。
9. Stage 5：Freeform B-spline / Plate Refit。
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
6. Stage 3A：实现 PlaneRegionMerge，形成稳定 baseline。
7. Stage 4：尽早启动 Freeform Candidate Detection。
8. Stage 3B / 3D：并行补 Cylinder / Sphere。
9. Stage 5：在 ErrorMetric / ReportGenerator 完善后再做自由曲面重拟合。
```

---

## 11. Codex 开发约束

Codex 修改合并算法时必须遵守：

```text
1. 架构边界遵循 docs/module_design.md。
2. 当前任务状态遵循 docs/implementation_status.md。
3. 合并算法路线遵循本文件。
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

### 12.2 Stage 2.5 开发提示词摘要

```text
本次实现 Stage 2.5：Candidate GUI Preview。

目标：
- 将 MergePlanner 生成的候选区域在 3D Viewer 中高亮显示。
- 默认高亮最大候选或 Top N 候选。
- 支持显式开关显示全部候选。
- 支持清除候选预览。
- 不修改 ShapeDocument。
- 不改变 applyMerge() 的 same-domain 合并路径。
- 不实现候选接受/拒绝。
- 不实现 PlaneRegionMerge。

要求：
1. AppController 保存或返回最近一次 MergePlannerResult。
2. MainWindow::previewMergeCandidates() 将候选 faces 传给 OccViewWidget。
3. OccViewWidget 支持 showMergeCandidates(...) 和 clearMergeCandidates()。
4. 重新打开文件或清除预览时，候选高亮消失。
5. 报告面板仍输出候选统计。
6. 新增或更新测试，至少保证预览逻辑不改变 document stats。
```

### 12.3 Stage 2.6 开发提示词摘要

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

### 12.4 Stage 3A 开发提示词摘要

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
   将 P1 合并候选规划细化为：
   - P1-A：SameDomainUnifier 参数与报告增强，重点验证 concat_bsplines=true
   - P1-B：MergeCandidate 数据结构
   - P1-C：MergePlanner + MergeRegionGrower 通用候选框架
   - P1-D：PlaneLike candidate 预览统计
   - P1-E：候选区域 GUI 高亮预览
   - P1-F：候选区域选择 / 接受 / 拒绝
   - P1-G：PlaneRegionMerge baseline
   - P1-H：Freeform Candidate Detection
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
6. Plane / Cylinder / Cone / Sphere 作为解析基础图元合并。
7. Freeform Candidate Detection 提前进入，服务潮玩模型自由曲面。
8. B-spline / Plate Refit 在验证、报告和 rollback 完善后实施。
```

是否高亮所有候选区域的结论：

```text
应该支持“显示全部候选区域”，但不建议默认无条件显示全部。
默认应优先高亮最大候选、当前选中候选或 Top N 候选。
全部显示应作为显式开关，用于实验和整体分布检查。
```
