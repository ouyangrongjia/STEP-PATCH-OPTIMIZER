# 技术路线设计

## 1. 总体思路

本项目目标是对过度细分的 STEP/STP 曲面片进行后处理，使面片分布更符合模型真实几何结构。

核心问题不是单纯减少面片数量，而是：

```text
在保持实体合法性和几何误差可控的前提下，
尽可能消除无意义的内部 patch 边界，
同时保留棱角、棱边、凸起、凹陷和曲率突变等真实特征边界。
```

因此技术路线应采用：

```text
特征感知的受约束曲面片合并
```

也就是：

```text
Feature-aware constrained patch merging
```

---

## 2. 总体流程

```text
STEP/STP 输入
  ↓
OCCT 读取并转换为 TopoDS_Shape
  ↓
B-rep 拓扑解析
  ↓
Face / Edge / Shell / Solid 建立索引
  ↓
构建 Face Adjacency Graph
  ↓
特征线提取
  ├── 拓扑特征边
  ├── 几何特征边
  ├── 曲率特征边
  └── 用户冻结边
  ↓
合并候选生成
  ├── 共面合并
  ├── 同圆柱面合并
  ├── 同球面合并
  ├── 同圆锥面合并
  ├── 同 B-spline 几何域合并
  └── 近似 G1 连续区域合并
  ↓
特征约束过滤
  ├── 不跨越 sharp edge
  ├── 不跨越用户冻结边
  ├── 不跨越强曲率突变边
  └── 不破坏 shell / solid 拓扑
  ↓
执行合并
  ├── Same-domain unify
  ├── 必要时曲面重拟合
  └── 更新拓扑关系
  ↓
缝合与合法性检查
  ├── free edge
  ├── multiple edge
  ├── non-manifold edge
  ├── self-intersection
  ├── solid closedness
  └── geometry deviation
  ↓
GUI 前后对比
  ↓
优化后 STEP/STP 导出
```

---

## 3. 阶段一：基础 STEP/B-rep 后处理

### 3.1 目标

第一阶段目标是建立完整的工程闭环：

```text
能打开 STEP
能显示模型
能统计面片数量
能识别基础特征边
能合并同域碎片面片
能导出新的 STEP
能验证导出结果仍为合法实体
```

### 3.2 主要任务

1. 使用 OCCT 读取 STEP/STP 文件。
2. 将模型转换为 `TopoDS_Shape`。
3. 遍历 shape 中的 solid、shell、face、edge。
4. 为每个 face 和 edge 建立唯一编号。
5. 构建 edge 到 adjacent faces 的映射。
6. 构建 face adjacency graph。
7. 计算相邻面之间的二面角。
8. 根据二面角阈值识别 sharp edge。
9. 支持用户在 GUI 中手动冻结 edge。
10. 调用 same-domain unify 合并同几何域面片。
11. 执行 sewing 和 shape validation。
12. 输出优化后的 STEP/STP。

---

## 4. 阶段二：特征感知合并

### 4.1 核心问题

阶段一只能解决“明显同域”的碎片面片合并问题。阶段二要解决更接近真实任务的问题：

```text
哪些边界是真实特征？
哪些边界只是导出时产生的碎片化 patch 边界？
```

例如，一个正方体模型中，理想情况下：

- 六个主面应该保留为六个大的曲面区域；
- 十二条棱边应被识别为特征线；
- 每个平面内部不应残留大量无意义 patch 边界；
- 不应跨越棱边把两个垂直平面错误合并。

### 4.2 Face Adjacency Graph

构建面片邻接图：

```text
G = (F, E)
```

其中：

- `F` 表示 STEP 中的 faces；
- `E` 表示两个 face 之间存在共享拓扑 edge。

每条图边记录：

```text
shared_edge_id
face_a
face_b
dihedral_angle
normal_jump
curvature_jump
surface_type_pair
same_domain_flag
feature_edge_flag
user_locked_flag
merge_cost
```

### 4.3 合并代价

相邻面片是否可以合并，可以设计合并代价：

```text
merge_cost =
    w_angle   * normalized_dihedral_angle
  + w_curv    * normalized_curvature_jump
  + w_surface * surface_type_penalty
  + w_area    * area_balance_penalty
  + w_error   * fitting_error
```

当：

```text
merge_cost < threshold
```

且共享边不是特征边时，两个面片可以作为候选合并对象。

### 4.4 特征边分类

特征边建议分为：

```text
strong_feature_edge
weak_feature_edge
user_locked_edge
non_feature_edge
```

含义如下：

| 类型 | 含义 | 是否允许跨越合并 |
|---|---|---|
| `strong_feature_edge` | 明显棱边、二面角突变大 | 不允许 |
| `weak_feature_edge` | 曲率变化明显，但不一定是折角 | 默认不允许，可手动调整 |
| `user_locked_edge` | 用户手动冻结的边 | 不允许 |
| `non_feature_edge` | 无明显结构意义的内部 patch 边界 | 允许 |

---

## 5. 阶段三：近似连续区域重拟合

### 5.1 适用场景

当多个相邻 NURBS patch 并不在完全相同的几何域上，但整体上近似属于一个连续光顺区域时，仅靠 same-domain unify 不足以合并。

典型情况：

```text
多个近似共面的 B-spline 小面片
多个近似 G1 连续的小曲面片
逆向建模中被切碎的光顺外壳
玩具模型表面的细碎 NURBS patch 区域
```

### 5.2 基本流程

```text
选定 candidate face group
  ↓
提取外边界 loop
  ↓
在原始 face group 上采样点
  ↓
保留边界约束和必要特征线
  ↓
拟合新的 B-spline surface
  ↓
根据外边界重建 trimmed face
  ↓
替换原 face group
  ↓
sewing
  ↓
误差评估
  ↓
合法性检查
```

### 5.3 难点

曲面重拟合阶段难点包括：

1. 拟合曲面的阶数、控制点数量和参数化方式选择；
2. 新曲面如何严格贴合原区域外边界；
3. 原有 trim curve 如何映射到新曲面参数域；
4. 替换局部 face group 后如何保持 shell 闭合；
5. 如何评估最大偏差、平均偏差和局部异常；
6. 如何避免过度光顺导致棱边、凸起和细节被抹平。

因此该阶段不应作为第一版核心功能，而应作为后续增强方向。

---

## 6. GUI 交互路线

项目最终呈现为 GUI 软件，而非纯命令行工具。

基础交互流程：

```text
打开 STEP
  ↓
三维窗口显示模型
  ↓
用户旋转、缩放、平移查看模型
  ↓
点击 face / edge 查看属性
  ↓
自动检测特征线
  ↓
高亮显示特征线
  ↓
用户手动锁定或解锁边界
  ↓
预览可合并区域
  ↓
确认合并
  ↓
查看合并前后统计
  ↓
合法性检查
  ↓
导出 STEP
```

必要视图操作：

| 鼠标操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标滚轮 | 放大 / 缩小视图 |
| 鼠标左键 | 选择 face 或 edge |
| Shift + 鼠标左键 | 多选 face 或 edge |
| 鼠标右键 | 打开上下文菜单 |
| Ctrl + Z | 撤销上一步操作 |
| Ctrl + Y | 重做操作 |

---

## 7. 推荐开发顺序

### 第 1 阶段：GUI 与 STEP 读写

目标：

```text
打开 STEP 文件，并在 GUI 中显示。
```

任务：

- 搭建 CMake 工程；
- 集成 Qt6；
- 集成 OCCT；
- 实现主窗口；
- 嵌入 OCCT Viewer；
- 读取 STEP/STP；
- 显示 AIS_Shape；
- 支持鼠标中键旋转视角；
- 支持鼠标滚轮缩放视图；
- 支持基础平移和视图重置。

### 第 2 阶段：拓扑索引与选择

目标：

```text
点击模型中的 face / edge，并查看其属性。
```

任务：

- 遍历 TopoDS_Shape；
- 建立 face index；
- 建立 edge index；
- 建立 edge-face 关联；
- 建立 face adjacency graph；
- 支持鼠标选择 face；
- 支持鼠标选择 edge；
- 属性面板显示 face/edge 信息。

### 第 3 阶段：特征线提取

目标：

```text
自动高亮 sharp edges，并允许用户修正。
```

任务：

- 计算相邻面二面角；
- 标记 free edge / multiple edge；
- 根据角度阈值检测 sharp edge；
- GUI 高亮显示特征线；
- 支持用户 lock / unlock edge；
- 保存用户约束。

### 第 4 阶段：同域面片合并

目标：

```text
合并同一几何域上的碎片面片。
```

任务：

- 实现 SameDomainUnifier；
- 输入用户冻结边；
- 执行 same-domain 合并；
- 统计合并前后 face/edge 数量；
- 更新拓扑索引；
- 在 GUI 中显示前后差异；
- 支持撤销。

### 第 5 阶段：合法性检查与导出

目标：

```text
确保合并后仍是合法实体，并导出 STEP/STP。
```

任务：

- 检查 free edge；
- 检查 multiple edge；
- 检查 solid 数量；
- 检查 shell 闭合性；
- 检查几何偏差；
- 输出 JSON/CSV 报告；
- 导出 STEP/STP；
- 重新读取导出文件做二次校验。

---

## 8. 实验指标

后续组会和论文方向可以围绕以下指标展开：

| 指标 | 含义 |
|---|---|
| `face_count_before` | 合并前面片数 |
| `face_count_after` | 合并后面片数 |
| `face_reduction_ratio` | 面片减少比例 |
| `edge_count_before` | 合并前边数量 |
| `edge_count_after` | 合并后边数量 |
| `feature_edge_preserve_rate` | 特征边保留率 |
| `free_edge_count` | 自由边数量 |
| `multiple_edge_count` | 多重边数量 |
| `solid_count` | 实体数量 |
| `max_deviation` | 最大几何偏差 |
| `mean_deviation` | 平均几何偏差 |
| `invalid_merge_count` | 无效合并次数 |
| `manual_correction_count` | 用户手动修正次数 |

---

## 9. 当前阶段建议（2026-05-27 修订）

### 9.1 关键诊断

当前 GUI 显示模型"看起来连续"，但真实 STEP/B-rep 依赖的拓扑结构可能已经损坏：

```text
GUI 显示 → OCCT 可视化三角网格 → "看起来连续"
真实 STEP/B-rep → Face + Wire + Edge + Vertex + PCurve + Orientation
```

平面重建后，如果新 face 的边界 wire、pcurve、方向、闭合关系不正确，外部软件（非 OCCT）重新解析 STEP 时会暴露为：

```text
- 缺面（missing face）
- 飞面（floating face / detached patch）
- 大平面（unbounded infinite plane）
- 开壳（open shell）
```

**这不是渲染问题，而是 B-rep 拓扑 / 裁剪边界不稳定问题。**

### 9.2 当前路线收窄

当前最合理的路线是：

```text
立即收窄，不继续扩展球面、圆柱、圆锥真实合并。
先做 PlaneRegionMerge 导出级合法性闭环。
等平面导出稳定后，再做球面 / 圆柱。
```

理由：

```text
球面、圆柱比平面更难。平面都没做到 STEP 稳定前，
继续做球面/圆柱真实合并只会放大问题。
```

### 9.3 下一阶段任务

**Stage 3A-Fix：PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild**

任务：

```text
1. 导出级合法性闭环
   - 每次真实合并后自动执行 BRepCheck_Analyzer
   - 自动执行 ShapeValidator
   - 导出临时 STEP
   - 重新读取临时 STEP
   - 再次统计 solid/shell/face/edge
   - 若失败则 rollback

2. 冻结 PlaneRegionMerge 输入范围
   - 只允许原生 Plane
   - 只允许单 outer wire
   - 只允许无 inner loop
   - 只允许边界闭合
   - 不跨 protected/locked edge
   - 不跨 feature edge
   - 合并后 solid 数不下降
   - 近似平面降级为候选预览，不直接真实合并

3. 修复 boundary wire
   - outer wire 排序
   - edge orientation
   - 3D curve 与 2D pcurve 同步
   - ShapeFix_Wire
   - ShapeFix_Face
   - BRepLib::BuildCurves3d
   - 必要时重建 planar pcurve

4. 把不安全候选明确拒绝
   - 多边界环 → 不支持
   - 有洞 → 不支持
   - 边界不闭合 → 不支持
   - 导出重读失败 → 已回滚
   - 近似平面 → 暂不进入真实重建
```

验收标准：

```text
平面合并后，不只 GUI 正常，还必须：
1. 导出 STEP
2. 重新读取 STEP
3. solid/shell/face/edge 统计合理
4. 外部软件（非 OCCT）不再出现缺面、飞面、无限平面
```

### 9.4 冻结范围

以下能力已完成但不进一步扩展，直到 Stage 3A-Fix 验收通过：

```text
- Stage 3D SphereRegionMerge → 实验性保持
- Stage 3B CylinderRegionMerge → 冻结
- Stage 3C ConeRegionMerge → 冻结
- Stage 3E TorusRegionMerge → 冻结
- Stage 4 Freeform Candidate Detection → 冻结
- Stage 5 Freeform B-spline / Plate Refit → 冻结
```
