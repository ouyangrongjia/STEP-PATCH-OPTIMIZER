# Algorithm Design

项目名称：`step-patch-optimizer`  
中文定位：特征感知的 STEP 曲面片合并与边界优化系统  
核心目标：在不破坏 STEP/B-rep 实体合法性的前提下，减少过细碎的 NURBS/B-rep 曲面片数量，并使保留下来的面片边界尽量贴合模型真实结构特征，例如棱边、棱角、凸起、凹陷和曲率突变区域。

---

## 1. 算法目标

本项目的算法目标不是单纯减少面片数量，而是实现以下多目标优化：

1. **降低面片冗余**  
   将原始 STEP 中被过度切分的共面、同圆柱面、同球面、同 B-spline 几何域面片合并。

2. **保持结构特征边界**  
   合并过程不能跨越真实棱边、折角、凸起边界、凹陷边界和用户指定的保护边。

3. **维持 B-rep 拓扑合法性**  
   合并后模型仍应保持闭合实体，不产生新的 free edge、multiple edge、自交面或非流形结构。

4. **控制几何误差**  
   合并或重拟合后的新面片应尽量贴近原始面片集合，最大偏差和平均偏差需要低于设定阈值。

5. **支持交互式修正**  
   自动算法只能作为初始结果，GUI 需要支持用户手动锁定/解锁特征边，并重新执行合并。

---

## 2. 输入与输出

### 2.1 输入

```text
输入文件：*.step / *.stp
输入对象：TopoDS_Shape
主要几何元素：Solid / Shell / Face / Edge / Vertex
主要曲面类型：Plane / Cylinder / Sphere / Cone / Torus / BSplineSurface / BezierSurface / Other
```

### 2.2 输出

```text
输出文件：优化后的 *.step / *.stp
输出报告：JSON / CSV / Markdown 统计报告
输出状态：合并前后面片数量、特征边数量、合法性检查结果、误差指标
```

---

## 3. 总体算法流程

```text
STEP 输入
  ↓
STEP 读取与 B-rep 解析
  ↓
Face / Edge / Vertex 编号
  ↓
构建拓扑邻接图
  ↓
提取候选特征线
  ├── 拓扑特征边
  ├── 二面角特征边
  ├── 曲率突变边
  ├── 曲面类型变化边
  └── 用户锁定边
  ↓
构建合并候选
  ├── 同平面区域
  ├── 同圆柱面区域
  ├── 同球面区域
  ├── 同 B-spline 几何域区域
  └── 近似 G1 连续区域
  ↓
特征约束过滤
  ├── 不跨越 feature edge
  ├── 不跨越 user locked edge
  ├── 不跨越 topology boundary
  └── 不超过几何误差阈值
  ↓
生成 MergePlan
  ↓
用户预览与确认
  ↓
执行合并
  ├── Same-domain unify
  ├── 可选曲面重拟合
  └── 拓扑历史更新
  ↓
Sewing / Validation
  ↓
导出 STEP 与报告
```

---

## 4. 核心数据结构

### 4.1 FaceInfo

```cpp
struct FaceInfo {
    int id;
    TopoDS_Face face;
    SurfaceType surfaceType;
    double area;
    gp_Pnt center;
    gp_Dir avgNormal;
    std::vector<int> edgeIds;
    bool selected = false;
    bool removedAfterMerge = false;
};
```

### 4.2 EdgeInfo

```cpp
struct EdgeInfo {
    int id;
    TopoDS_Edge edge;
    std::vector<int> adjacentFaceIds;
    double length;
    bool isFreeEdge = false;
    bool isMultipleEdge = false;
    bool isFeatureEdge = false;
    bool isUserLocked = false;
    bool isMergeBoundary = false;
};
```

### 4.3 FaceAdjacency

```cpp
struct FaceAdjacency {
    int faceA;
    int faceB;
    int sharedEdgeId;
    double dihedralAngleDeg;
    double curvatureJump;
    double normalJump;
    double mergeCost;
    bool sameDomain;
    bool canMerge;
};
```

### 4.4 MergeCandidate

```cpp
struct MergeCandidate {
    int id;
    std::vector<int> faceIds;
    std::vector<int> internalEdgeIds;
    std::vector<int> boundaryEdgeIds;
    SurfaceType targetSurfaceType;
    double estimatedError;
    double mergeCost;
    bool valid;
};
```

### 4.5 MergePlan

```cpp
struct MergePlan {
    std::vector<MergeCandidate> candidates;
    std::vector<int> preservedFeatureEdgeIds;
    MergeOptions options;
};
```

---

## 5. 拓扑邻接图构建

### 5.1 图定义

将 B-rep 中的 face 视为图节点，将共享 edge 的 face 视为相邻节点：

```text
G = (V, E)

V: face 集合
E: face-face adjacency
```

如果两个 face 共享同一条拓扑 edge，则在图中建立一条邻接关系。

### 5.2 构建流程

```text
for each face in shape:
    assign face_id
    collect edges of face

for each edge in shape:
    assign edge_id
    find all adjacent faces
    if adjacent_face_count == 1:
        mark as free edge
    if adjacent_face_count > 2:
        mark as multiple / non-manifold edge

for each edge with exactly 2 adjacent faces:
    create FaceAdjacency(faceA, faceB, edge)
```

### 5.3 邻接图用途

邻接图用于：

- 计算相邻面的二面角；
- 判断共享边是否是特征边；
- 生成合并候选区域；
- 执行区域生长；
- 维护合并后的拓扑历史；
- 在 GUI 中高亮相邻区域。

---

## 6. 特征线提取算法

特征线提取是本项目的核心模块之一。它决定哪些边界必须保留，哪些边界可以在合并中消除。

### 6.1 特征边分类

| 类型 | 判定依据 | 是否默认保留 |
|---|---|---:|
| 拓扑边界边 | free edge / multiple edge / shell boundary | 是 |
| 折角边 | 二面角大于阈值 | 是 |
| 曲率突变边 | 两侧曲率统计差异大 | 是 |
| 曲面类型变化边 | Plane-Cylinder、Plane-BSpline 等类型变化 | 视情况 |
| 用户锁定边 | GUI 手动指定 | 是 |
| 内部冗余边 | 两侧光顺且可同域合并 | 否 |

### 6.2 二面角特征边

对相邻的两个 face，沿共享 edge 采样若干点，计算两侧法向夹角：

```text
angle = arccos(dot(n1, n2))
```

判定规则：

```text
if angle > angular_threshold:
    mark edge as feature edge
```

推荐默认值：

```text
angular_threshold = 20° ~ 30°
```

对于正方体，十二条棱边的二面角约为 90°，应稳定识别为特征边。

### 6.3 曲率突变特征边

仅依赖二面角不足以识别圆滑凸起、凹陷过渡和弱结构边界。因此需要引入曲率统计。

可选特征：

```text
mean_curvature_diff
principal_curvature_diff
normal_variation
surface_type_transition
```

判定规则示例：

```text
if curvature_jump > curvature_threshold and edge_length > min_feature_length:
    mark edge as weak feature edge
```

### 6.4 用户约束边

自动检测结果允许用户修正：

```text
Lock edge as feature
Unlock feature edge
Lock selected boundary loop
Unlock selected boundary loop
```

用户锁定边在合并阶段必须作为硬约束：

```text
if edge.isUserLocked:
    never remove this edge during merging
```

---

## 7. 合并候选生成

### 7.1 候选层级

| 等级 | 合并类型 | 难度 | 第一版实现 |
|---|---|---:|---:|
| Level 1 | 共平面面片合并 | 低 | 是 |
| Level 2 | 同解析曲面合并，例如圆柱、球、圆锥 | 中 | 是 |
| Level 3 | 同 B-spline 几何域合并 | 中 | 是 |
| Level 4 | 近似 G1 连续区域合并 | 高 | 暂缓 |
| Level 5 | 多 patch 曲面重拟合 | 很高 | 后期 |

### 7.2 两面可合并条件

两个相邻 face 允许合并需要满足：

```text
1. 两个 face 共享一条有效 edge
2. 共享 edge 不是 feature edge
3. 共享 edge 不是 user locked edge
4. 两个 face 几何上同域或近似同域
5. 二面角低于阈值
6. 曲率突变低于阈值
7. 合并后不会破坏 shell / solid
8. 合并误差低于阈值
```

### 7.3 Merge Cost

可以为每条 face adjacency 计算合并代价：

```text
merge_cost =
    w_angle   * normalized_dihedral_angle
  + w_curv    * normalized_curvature_jump
  + w_type    * surface_type_penalty
  + w_error   * estimated_fitting_error
  + w_area    * area_balance_penalty
```

默认判定：

```text
if merge_cost < merge_threshold:
    adjacency.canMerge = true
else:
    adjacency.canMerge = false
```

### 7.4 区域生长

```text
for each unvisited face:
    region = {face}
    queue.push(face)

    while queue is not empty:
        current = queue.pop()

        for each neighbor of current:
            edge = shared_edge(current, neighbor)

            if edge.isFeatureEdge:
                continue
            if edge.isUserLocked:
                continue
            if merge_cost(current, neighbor) > threshold:
                continue
            if not same_domain_or_acceptable(current, neighbor):
                continue

            region.add(neighbor)
            queue.push(neighbor)

    if region.size > 1:
        output MergeCandidate(region)
```

区域生长只生成候选，不直接修改模型。所有候选都应进入 GUI 预览，由用户确认或调整参数后再执行。

---

## 8. Same-domain 合并

第一版优先实现 same-domain 合并，即合并原本位于同一几何域上的相邻面片。

典型场景：

```text
大平面被切成多个小平面
圆柱面被切成多个条带
球面被切成多个 patch
同一 B-spline 曲面被修剪成多个 trimmed face
```

### 8.1 执行流程

```text
输入：TopoDS_Shape + frozen_edges + tolerance
  ↓
初始化 SameDomainUnifier
  ↓
设置 linear tolerance 和 angular tolerance
  ↓
将 feature edge / user locked edge 加入 KeepShape
  ↓
执行 Build
  ↓
获取输出 Shape
  ↓
重建 FaceIndex / EdgeIndex / TopologyGraph
  ↓
执行 Validation
```

### 8.2 伪代码

```cpp
TopoDS_Shape SameDomainUnifier::run(
    const TopoDS_Shape& input,
    const std::vector<TopoDS_Edge>& frozenEdges,
    double linearTol,
    double angularTol)
{
    ShapeUpgrade_UnifySameDomain unifier(input, true, true, false);

    unifier.SetLinearTolerance(linearTol);
    unifier.SetAngularTolerance(angularTol);

    for (const auto& edge : frozenEdges) {
        unifier.KeepShape(edge);
    }

    unifier.Build();
    return unifier.Shape();
}
```

---

## 9. 曲面重拟合接口

多片 NURBS patch 不完全同域但整体近似光顺时，需要后期引入曲面重拟合。第一版只保留接口，不作为主线。

### 9.1 输入

```text
待合并 face group
外边界 boundary loop
内部冗余边集合
采样密度
误差阈值
目标曲面阶数
```

### 9.2 输出

```text
新的 B-spline surface
新的 trim boundary
拟合误差
替换后的 TopoDS_Face
```

### 9.3 高层流程

```text
采样原始 face group
  ↓
构建统一参数域或 3D 点云
  ↓
拟合 B-spline surface
  ↓
重建外部 trim curve
  ↓
替换原始 face group
  ↓
Sewing
  ↓
误差评估与合法性检查
```

---

## 10. 验证算法

每次合并后必须执行验证。验证不通过时，不应覆盖当前模型状态。

### 10.1 拓扑验证

检查内容：

```text
solid_count
shell_count
face_count
edge_count
free_edge_count
multiple_edge_count
non_manifold_edge_count
```

### 10.2 几何验证

检查内容：

```text
最大几何偏差
平均几何偏差
法向偏差
曲率偏差
是否出现自交
是否存在退化边或退化面
```

### 10.3 特征保持验证

检查内容：

```text
原始 feature edge 是否被保留
用户 locked edge 是否被保留
正方体等规则模型的主棱边是否稳定存在
凸起/凹陷区域边界是否没有被错误吃掉
```

---

## 11. 参数设计

| 参数名 | 默认值 | 说明 |
|---|---:|---|
| `linear_tolerance` | `1e-4 ~ 1e-3` | 几何距离容差，需结合模型单位调整 |
| `angular_tolerance_deg` | `20 ~ 30` | 二面角特征边阈值 |
| `curvature_threshold` | 待实验确定 | 曲率突变阈值 |
| `min_feature_edge_length` | 待实验确定 | 过滤短噪声边 |
| `merge_cost_threshold` | `0.3 ~ 0.5` | 区域生长合并阈值 |
| `max_deviation` | 待实验确定 | 曲面重拟合最大误差 |
| `enable_same_domain_merge` | `true` | 是否启用同域合并 |
| `enable_refit` | `false` | 第一版默认关闭重拟合 |

---

## 12. MVP 算法范围

第一版只做稳定、可展示、可验证的内容：

```text
STEP 读取
Face / Edge 编号
拓扑邻接图
二面角特征边提取
用户锁定边
Same-domain 合并
Sewing / Validation
STEP 输出
统计报告
```

第一版暂不做：

```text
复杂 NURBS 重拟合
语义级特征识别
基于深度学习的特征线检测
自动修复所有 self-intersection
完全自动最优 patch layout
```

---

## 13. 关键风险

| 风险 | 说明 | 应对 |
|---|---|---|
| 特征边误判 | 噪声边被识别为特征，或真实特征被漏掉 | 提供 GUI 手动修正 |
| 合并过度 | 真实棱边被吃掉 | frozen edge + KeepShape 约束 |
| 合并不足 | 面片数量减少不明显 | 调整阈值，引入区域生长和重拟合 |
| 拓扑破坏 | 合并后不再是实体 | 每次操作后强制 Validation |
| NURBS 重拟合复杂 | trim curve 与边界约束难处理 | 后期模块化引入，第一版不依赖 |

---

## 14. 验收标准

MVP 算法完成标准：

```text
1. 能读取至少 5 个 STEP 测试模型。
2. 能正确统计 face / edge / shell / solid 数量。
3. 对正方体模型能识别 12 条主棱边。
4. 对过细分平面能合并冗余面片。
5. 合并后 free_edge_count 不增加。
6. 合并后 solid_count 保持为 1。
7. 能导出可重新读取的 STEP 文件。
8. GUI 中能显示合并前后统计结果。
```
