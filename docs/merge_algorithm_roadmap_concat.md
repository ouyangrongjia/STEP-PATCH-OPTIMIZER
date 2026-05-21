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

但当前合并能力主要依赖 `SameDomainUnifier`：

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
1. face reduction 不到 10%。
2. 特征边检测结果较可用，但 protectedEdges 较多，合并偏保守。
3. MergePlanner / MergeRegionGrower 仍未真正承担候选区域生成职责。
4. 解析基础图元合并尚未实现。
5. 潮玩模型常见自由曲面碎片化尚未解决。
6. SurfaceRefitter 暂不具备直接落地条件。
```

---

## 2. 总体算法路线

修正后的合并算法按五个阶段推进：

```text
Stage 1: SameDomainUnifier Enhancement
         增强当前同域合并的参数、报告和实验能力。
         关键实验点是暴露 concat_bsplines 参数，并重点验证 concat_bsplines=true 的效果。

Stage 2: Generic Merge Candidate Framework
         实现通用候选区域框架，不绑定具体几何类型。

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
2. 先解析几何，后自由曲面重拟合。
3. 平面只是第一个低风险子任务，不是合并算法边界。
4. 基础图元 Plane / Cylinder / Cone / Sphere 属于同一个解析图元阶段。
5. 自由曲面是潮玩模型的核心问题，应在 Plane baseline 后尽早进入候选检测。
6. 不跨越自动特征边和用户锁定边。
7. 每次真正修改 B-rep 后都必须进行合法性检查。
8. 任何高风险合并都必须支持 rollback 或 undo。
9. 所有破坏性几何修改必须通过 Command 层执行。
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
2. 当前代码默认 concat_bsplines=false；Stage 1 需要允许 GUI / 命令 / 测试切换 true / false。
3. 新增实验默认配置：
   - 保守模式：concat_bsplines=false
   - 增强模式：concat_bsplines=true
4. 在报告中记录本次合并使用的 concat_bsplines 值。
5. 对同一 STEP 样例分别运行 concat_bsplines=false 和 concat_bsplines=true，比较：
   - face_count_before / face_count_after
   - edge_count_before / edge_count_after
   - face_reduction_ratio
   - edge_reduction_ratio
   - protected_edge_count
   - BRepCheck 是否通过
   - 导出后二次读取是否通过
6. 如果 concat_bsplines=true 在样例中稳定通过验证，可以将 GUI 默认值切换为 true；否则保持 false，但保留用户可选项。
7. 不改变现有 MergePatchCommand 的主执行语义。
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

### 3.8 Stage 1 推荐 Codex 提示词摘要

```text
本次实现 Stage 1：SameDomainUnifier Enhancement。

目标不是重写合并算法，而是增强 SameDomainUnifier 的参数可控性和报告能力。

当前 SameDomainUnifyOptions::concat_bsplines 默认是 false。
请将 concat_bsplines 暴露为可配置参数，并贯穿：
ParameterPanel -> AlgorithmParameters -> AppController -> MergePatchCommand -> SameDomainUnifier。

要求：
1. GUI 参数面板增加“连接 B-spline 边 / Concat B-splines”复选框。
2. SameDomainUnifyOptions 使用该参数，不再固定 false。
3. 合并报告中输出 concat_bsplines=true/false。
4. 测试中覆盖 concat_bsplines=false 和 concat_bsplines=true 两种路径。
5. 不改变 SameDomainUnifier 之外的合并逻辑。
6. 不实现 MergePlanner / MergeRegionGrower / PlaneRegionMerge。
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

候选类型不要只支持 Plane。  
即使第一版只生成 PlaneLike，也必须预留 CylinderLike、SphereLike、FreeformG1 等类型，避免后续重构。

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

## 5. Stage 3：Analytic Primitive Region Merge

### 5.1 目标

在通用候选框架基础上，对解析基础图元候选区域执行真正的 B-rep 合并。

Stage 3 不是 Plane-only，而是解析图元合并阶段：

```text
Stage 3A: PlaneRegionMerge
Stage 3B: CylinderRegionMerge
Stage 3C: ConeRegionMerge
Stage 3D: SphereRegionMerge
Stage 3E: Optional TorusRegionMerge
```

### 5.2 总体执行流程

```text
1. 从 MergePlannerResult 中选择低风险 analytic primitive candidate。
2. 根据 candidate_type 分派到具体 RegionMerger。
3. 提取 candidate.faces 对应的区域。
4. 提取区域外边界 boundary loop。
5. 构建目标解析曲面。
6. 将 boundary loop 投影到目标曲面参数域。
7. 构建新的 trimmed face。
8. 替换原 region。
9. sewing。
10. ShapeValidator 检查。
11. ErrorMetric 计算偏差。
12. 成功则提交，失败则 rollback。
```

### 5.3 Stage 3A：PlaneRegionMerge

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
```

### 5.4 Stage 3B：CylinderRegionMerge

目标：

```text
多个近似同圆柱面的相邻 faces
→ 合并为一个较大的 cylindrical trimmed face
```

判断条件：

```text
axis direction angle < axis_tol
axis position distance < axis_distance_tol
radius difference < radius_tol
shared edge not protected
```

适用区域：

```text
手臂
腿部
圆柱形底座
装饰柱状结构
耳机、管状结构
圆柱类连接件
```

验收标准：

```text
1. 圆柱面条带碎片可合并。
2. 不跨越特征边或锁边。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 参数和误差写入报告。
```

### 5.5 Stage 3C：ConeRegionMerge

目标：

```text
多个近似同圆锥面的相邻 faces
→ 合并为一个较大的 conical trimmed face
```

判断条件：

```text
axis direction angle < axis_tol
apex position distance < apex_tol
semi-angle difference < semi_angle_tol
shared edge not protected
```

适用区域：

```text
尖角装饰
锥形帽子
锥形底座
局部尖锐但规则的装饰结构
```

验收标准：

```text
1. 圆锥面碎片可合并。
2. 不跨越特征边或锁边。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 参数和误差写入报告。
```

### 5.6 Stage 3D：SphereRegionMerge

目标：

```text
多个近似同球面的相邻 faces
→ 合并为一个较大的 spherical trimmed face
```

判断条件：

```text
center distance < center_tol
radius difference < radius_tol
shared edge not protected
```

适用区域：

```text
眼球
球形关节
圆形装饰件
局部近似球面的头部或身体结构
```

验收标准：

```text
1. 球面碎片可合并。
2. 不跨越特征边或锁边。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 参数和误差写入报告。
```

### 5.7 Stage 3E：TorusRegionMerge，可选

Torus 参数和周期性更复杂，建议作为可选阶段，不进入近期主线。

适用区域：

```text
环形装饰
甜甜圈状结构
环状管件
圆环边缘
```

### 5.8 Stage 3 总体验收标准

```text
1. Plane / Cylinder / Sphere 至少各有一个可验证样例。
2. 所有解析图元合并都不能跨越 protectedEdges。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 合并率、偏差、失败原因写入报告。
6. 失败时不污染当前 ShapeDocument。
7. undo / redo 正常。
```

---

## 6. Stage 4：Freeform Candidate Detection

### 6.1 目标

提前识别潮玩模型常见自由曲面碎片区域，但不立即重拟合。

该阶段回答：

```text
哪些 B-spline / Bezier / freeform patches 可能属于同一个光顺区域？
哪些区域值得后续 B-spline / Plate refit？
哪些区域风险过高，不应该自动重拟合？
```

### 6.2 为什么 Stage 4 要提前

潮玩模型中大量区域不是 Plane / Cylinder / Sphere：

```text
头发曲面
脸部曲面
衣服褶皱
圆润过渡
身体外壳
帽子、鞋子、装饰件的光顺区域
```

如果只做解析图元合并，face reduction 会有明显上限。  
因此，在 PlaneRegionMerge 得到稳定 baseline 后，应尽早启动 Freeform Candidate Detection。

### 6.3 候选类型

```cpp
MergeCandidateType::FreeformG1
MergeCandidateType::FreeformG2
```

### 6.4 判断依据

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

### 6.5 输出指标

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

### 6.6 不做内容

```text
1. 不拟合新曲面。
2. 不替换 B-rep。
3. 不重建 trim curve。
4. 不 sewing。
5. 不自动导出优化结果。
```

### 6.7 验收标准

```text
1. 能在真实潮玩 STEP 上识别一批 FreeformG1 / FreeformG2 候选区域。
2. 不跨越自动特征边或用户锁定边。
3. 预览不会改变 face/edge 数。
4. 报告中能看到每个候选区域的连续性指标和风险等级。
5. 可与 PlaneLike / CylinderLike 等候选类型共存。
```

---

## 7. Stage 5：Freeform B-spline / Plate Refit

### 7.1 目标

对高可信自由曲面候选区域执行局部重拟合，把多个光顺自由曲面碎片合并成更少的 B-spline face。

这是解决潮玩自由曲面碎片化的关键阶段，但也是最高风险阶段。

### 7.2 基本流程

```text
1. 取 Stage 4 生成的 FreeformG1 / FreeformG2 candidate。
2. 提取区域外边界 loop。
3. 从原始 faces 采样点和法向。
4. 建立局部参数化。
5. 用 B-spline surface 或 Plate surface 拟合。
6. 将边界投影到新 surface 参数域。
7. 重建 trim curve。
8. 构建新的 trimmed face。
9. 替换原 region。
10. sewing。
11. ShapeValidator + ErrorMetric。
12. 成功提交，失败 rollback。
```

### 7.3 可选技术路径

```text
1. B-spline surface fitting
   适合规则参数化、采样点分布较稳定的区域。

2. Plate surface fitting
   适合需要边界约束、点约束和局部光顺的区域。

3. Hybrid refit
   先尝试 B-spline，失败后尝试 Plate，仍失败则保留原始 patch。
```

### 7.4 风险

```text
1. 参数化困难。
2. trim curve 重建困难。
3. 容易破坏 solid 合法性。
4. 容易产生偏差过大的光顺面。
5. 容易抹掉潮玩模型的细节特征。
6. 需要更完整的误差评估。
```

### 7.5 前置条件

必须先完成：

```text
1. MergeCandidate。
2. MergePlanner。
3. MergeRegionGrower。
4. PlaneRegionMerge baseline。
5. Freeform Candidate Detection。
6. ErrorMetric。
7. ReportGenerator。
8. 更完整的 ShapeValidator。
9. Command 级 rollback / undo。
```

### 7.6 验收标准

```text
1. 只在用户确认或实验模式下启用。
2. max deviation / mean deviation / rms deviation 可计算。
3. 合并后 BRepCheck 通过。
4. 导出 STEP 二次读取通过。
5. 可通过 undo 回退。
6. 报告中记录拟合误差、采样数量、边界复杂度和合并风险。
7. 对真实潮玩自由曲面样例能产生可解释的 face reduction。
```

---

## 8. 推荐实际推进顺序

### 8.1 工程优先路线

目标是尽快看到稳定合并率提升：

```text
1. Stage 1：SameDomainUnifier 参数与报告增强，重点验证 concat_bsplines=true。
2. Stage 2：通用候选区域框架。
3. Stage 3A：PlaneRegionMerge。
4. Stage 3B：CylinderRegionMerge。
5. Stage 3D：SphereRegionMerge。
6. Stage 4：Freeform Candidate Detection。
7. Stage 5：Freeform B-spline / Plate Refit。
```

### 8.2 研究优先路线

目标是更贴近潮玩模型论文方向：

```text
1. Stage 2：通用候选区域框架。
2. Stage 3A：PlaneRegionMerge，作为稳定 baseline。
3. Stage 4：Freeform Candidate Detection。
4. Stage 5：Freeform B-spline / Plate Refit。
5. Stage 3B / 3C / 3D：解析图元补全。
```

### 8.3 当前推荐折中路线

结合当前项目状态，建议采用折中路线：

```text
1. Stage 1：增强 SameDomainUnifier 的参数与报告，重点暴露 concat_bsplines 并验证 true/false。
2. Stage 2：实现通用 MergeCandidate / MergePlanner / MergeRegionGrower。
3. Stage 2 初期只启用 PlaneLike candidate，但框架预留多类型。
4. Stage 3A：实现 PlaneRegionMerge，形成稳定 baseline。
5. Stage 4：尽早启动 Freeform Candidate Detection。
6. Stage 3B / 3D：并行补 Cylinder / Sphere。
7. Stage 5：在 ErrorMetric / ReportGenerator 完善后再做自由曲面重拟合。
```

---

## 9. Codex 开发约束

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
10. 在 Stage 2 之前不要实现 SurfaceRefitter。
11. 在候选区域没有验证前，不要做破坏性拓扑替换。
12. 每个阶段只实现当前阶段允许的最小功能，不提前混入后续阶段。
13. Stage 1 不允许硬编码 concat_bsplines=true；必须暴露参数并记录实际值。
```

---

## 10. 每阶段推荐 Codex 提示词模板

### 10.1 通用前缀

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

### 10.2 Stage 1 开发提示词摘要

```text
本次实现 Stage 1：SameDomainUnifier Enhancement。

目标：
- 暴露 concat_bsplines 参数。
- 当前 concat_bsplines 默认 false。
- 新增 GUI / 参数 / 命令链路，使 concat_bsplines 可切换 true / false。
- 重点验证 concat_bsplines=true 是否提升 edge/face reduction。
- 报告中输出 concat_bsplines 实际值、合并率、protectedEdges 数。
- 测试覆盖 true / false 两条路径。
- 不实现 MergePlanner / MergeRegionGrower / PlaneRegionMerge。
```

### 10.3 Stage 2 开发提示词摘要

```text
本次实现 Stage 2：Generic Merge Candidate Framework。

目标：
- 新增或完善 MergeCandidate / MergePlanner / MergeRegionGrower。
- 架构上支持 PlaneLike / CylinderLike / SphereLike / FreeformG1 等候选类型。
- 本轮只启用 PlaneLike candidate。
- 只生成候选，不修改 ShapeDocument。
- previewMergeCandidates 输出候选统计。
- 新增测试验证 protectedEdges / lockedEdges 能阻断区域生长。
```

### 10.4 Stage 3A 开发提示词摘要

```text
本次实现 Stage 3A：PlaneRegionMerge。

目标：
- 只处理 PlaneLike candidate。
- 从 candidate.faces 提取 boundary loop。
- 构造新的 planar trimmed face。
- 替换原区域并 sewing。
- 合并后必须 ShapeValidator 通过。
- 失败必须 rollback。
- 支持 undo / redo。
- 不处理 Cylinder / Sphere / Freeform。
```

### 10.5 Stage 4 开发提示词摘要

```text
本次实现 Stage 4：Freeform Candidate Detection。

目标：
- 只检测 FreeformG1 / FreeformG2 candidate。
- 不拟合新曲面，不修改 ShapeDocument。
- 根据法向连续性、曲率连续性、protectedEdges、lockedEdges 生成候选区域。
- 报告中输出连续性指标和风险等级。
- 不实现 SurfaceRefitter。
```

---

## 11. 文档同步建议

新增本文件后，建议同步更新：

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
   - P1-D：PlaneLike candidate 预览
   - P1-E：PlaneRegionMerge baseline
   - P1-F：Freeform Candidate Detection
```

---

## 12. 简短结论

当前路线不是：

```text
只做近似共面合并
```

而是：

```text
1. SameDomainUnifier 作为当前保守基础。
2. Stage 1 明确暴露 concat_bsplines 参数，并重点验证 concat_bsplines=true。
3. MergeCandidate / MergePlanner / MergeRegionGrower 作为通用候选框架。
4. Plane / Cylinder / Cone / Sphere 作为解析基础图元合并。
5. Freeform Candidate Detection 提前进入，服务潮玩模型自由曲面。
6. B-spline / Plate Refit 在验证、报告和 rollback 完善后实施。
```

平面只是第一个安全落地点，不是算法路线的边界。`concat_bsplines=true` 是 Stage 1 的关键实验点，但不应硬编码为唯一行为。
