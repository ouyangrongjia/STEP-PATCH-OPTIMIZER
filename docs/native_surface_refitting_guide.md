# 原生自由曲面重拟合开发指南

> 文档定位：指导 FeatureBoundedRefit 局部区域的原生自由曲面拟合开发，并定义 Geomagic 后备路线、失败分流和验收边界。
>
> 当前状态：`NSR-1.1` 已完成；当前任务为 `NSR-1.2` dense 结构诊断与固定 96 点采样分配实验，NSR-2 / NSR-4 继续冻结。
>
> 开发分支：`native-surface-refit`。该路线作为独立分支推进，不直接改写 `feature-bounded-refit` 上的 Geomagic 稳定主线。
> 更新时间：2026-07-14

---

## 1. 当前决策

项目当前主线从“继续优化 Geomagic 输入并修补导出面片”切换为：

```text
原始 STEP
→ FeatureBoundedRefit 候选区域
→ 读取原始 B-rep 边界与候选面
→ 原生单张自由曲面拟合
→ 原始边界裁剪 / pcurve / SameParameter
→ 独立误差验证
→ PatchReplacementCommand
→ PatchReplacementRepair
→ StrictTopologyGate / STEP roundtrip
```

Geomagic 不删除，但降级为后备路线。只有原生路线明确返回“不支持”或在受控参数范围内拟合失败时，才允许进入 Geomagic。

分支边界：

- `feature-bounded-refit`：保留当前 Geomagic patch preview / Apply 稳定基线和历史对照。
- `native-surface-refit`：实现原生自由曲面拟合，当前文档、TODO 和实现状态以该分支为准。
- 原生路线尚未通过 NSR-1 / NSR-2 验收前，不回合到 `feature-bounded-refit`。
- 后续是否合并由真实样例、拓扑 Gate、STEP roundtrip 和商业 CAD 类质量门共同决定。

当前不做：

- 不做整个潮玩件的全局自由曲面重建。
- 不做自动多曲面布局。
- 不做 GUI 正式入口。
- 不开发完整商业级 AutoSurface。
- 不要求与外围曲面统一 G1。
- 不通过放宽 Sewing 或 `StrictTopologyGate` 掩盖几何误差。

---

## 2. 为什么切换主线

最新真实运行目录：

```text
data/runs/20260711_000459_570__boundary-trim-fill__03_配件_Clay__cand-0179__route1-route2/
```

当前证据链：

1. `crop_stl_candidate_179.stl` 与拆除候选区域后的 STEP 边界完整贴合，角点没有缺口。
2. `raw_geomagic_crop_stp_candidate_179.stp` 的底层曲面在相同角点已经向内圆化。
3. `normalized_patch_mm.stp` 保留了相同欠覆盖，问题不是单位归一化造成的。
4. 多曲面最大投影距离约为 `0.1050485`。
5. 角点最大偏差约为 `0.1050273`，与最大投影距离基本一致。
6. Apply 可以通过提高 Sewing 容差得到拓扑闭合，但商业 CAD 类质量门仍失败。

结论：

```text
Geomagic AutoSurface 没有把裁剪 STL 的角点当成必须精确保留的 CAD 硬约束。
一旦导出的底层曲面没有覆盖原始角点，后续 original-boundary re-trim 无法凭空补出支撑曲面。
继续提高采样密度、Sewing 容差或修补 pcurve 不能解决该几何根因。
```

### 2.1 商业软件通常如何完成自由曲面拟合

需要先区分公开事实和工程推断。3D Systems 的公开资料确认 Geomagic Wrap 的 Exact Surfacing 能控制 NURBS patch layout、曲面质量和连续性，Design X 的 Selective Surfacing 会组合有机曲面和高精度特征建模；但厂商没有公开 AutoSurface 的具体目标函数、权重、求解器或失败恢复策略。因此，不能把下面的通用逆向工程流程表述为 Geomagic 的确定内部实现。

典型商业级 AutoSurface 可合理理解为以下分层流水线：

```text
点云 / 三角网格清理
→ 曲率、尖锐特征和区域分割
→ 自动或人工建立四边化 patch layout
→ 每个 patch 的二维参数化
→ NURBS 控制点拟合与光顺正则化
→ patch 间 G0 / G1 / G2 连续性协调
→ 偏差分析、局部加密和迭代优化
→ trimming、sewing、B-rep 检查和 CAD 导出
```

其中最难的通常不是解一次最小二乘，而是：

- 决定曲面应被分成几张 patch，以及 seam 放在哪里。
- 在拟合误差、曲面光顺、patch 数量、次数和连续性之间做全局折中。
- 处理任意拓扑、孔洞、分支、尖角、噪声和扫描缺失。
- 让最终 B-rep 能被不同商业 CAD 稳定读取。

自动模式通常更偏向“整体光顺、patch 网络稳定、数量受控、平均偏差合格”，而不是“任意裁剪边界和角点都作为不可移动硬约束”。这是对本项目现象的工程解释，不是对 Geomagic 私有算法的反编译结论：输入 STL 角点完整，但 raw patch 的底层曲面仍向内圆化，说明当前 AutoSurface 设置没有为该角点建立足够强的覆盖约束。

公开资料：

- [Geomagic Wrap 官方资料：Exact Surfacing、NURBS patch layout、曲面质量与连续性](https://www.3dsystems.com/sites/default/files/2019-11/3d-systems-wrap-en-letter-web-2019-11-01.pdf)
- [3D Systems 官方说明：Selective Surfacing 组合有机曲面和高精度特征建模](https://www.3dsystems.com/press-releases/3d-systems-announces-first-market-enhancements-reverse-engineering-software-enabling)
- [The Quad Layout Immersion：四边 patch layout 与 spline fitting 的关系](https://arxiv.org/abs/2012.09368)

### 2.2 自己拟合的难度边界

“复刻 Geomagic AutoSurface”和“重建一个已经圈定的局部区域”不是同一难度：

| 任务 | 难度判断 | 本项目处理 |
|---|---:|---|
| 整件任意拓扑自动分区、自动多曲面布局 | 很高 | 不做 |
| 多曲面 seam 布局和全局 G1 / G2 协调 | 很高 | NSR-5 冻结 |
| 自研通用 NURBS 参数化和约束优化器 | 高 | NSR-4 冻结 |
| 已知单外轮廓、无孔局部区域的一张曲面拟合 | 中等、可控 | 当前 NSR-1 |
| 用现成 OCCT Filling 验证可行性 | 中等偏低 | 第一版方案 |

当前任务已经由用户选出的特征线给出了区域边界，也保留原始 STEP 作为几何真值，因此绕开了商业 AutoSurface 最难的全局分区和 patch layout。真正需要验证的是：一张曲面能否在保留原始角点的同时覆盖内部形状，并在可接受复杂度下通过误差和有效性门控。这仍有算法风险，但工程规模明显小于自研商业级自动曲面系统。

---

## 3. 任务边界

### 3.1 第一阶段支持范围

原生拟合只接收满足以下条件的候选：

- 候选类型为 `FeatureBoundedRefit`。
- 候选面连通。
- 只有一个闭合外轮廓。
- 没有孔洞。
- 没有分支边界。
- 没有非流形边。
- 输出目标为一张 `TopoDS_Face`。
- 原始边界全部按 G0 约束处理。

不满足条件时必须返回明确的“不支持”原因，不得静默改变拓扑或自动扩大任务范围。

### 3.2 当前成功定义

第一阶段的成功不是“生成了一个 Face”，而是同时满足：

1. 输出只有一张拟合面。
2. 使用原始 STEP 边界，不出现 Geomagic 式角点圆化。
3. `BRepCheck_Analyzer` 通过。
4. 边界、角点和内部独立验证样本满足配置容差。
5. 曲面没有 Jacobian 翻转或近奇异区域。
6. 曲面次数、控制点和分段数量没有失控。
7. Patch Apply 仍经过 repair、`StrictTopologyGate` 和 STEP roundtrip。

### 3.3 “一步到位”的可实现边界

对本项目而言，“一步到位”可以作为产品交互目标，但不能解释为“一次固定参数的 Filling 调用保证任意自由曲面成功”。合理定义是：

```text
用户选择特征区域并触发一次原生重拟合
→ 系统检查候选拓扑和单曲面适用性
→ 在固定预算内执行少量、确定性的拟合 / 采样策略
→ 每个结果经过同一组几何与 BRep 质量门
→ 自动选择复杂度最低的合格结果
→ 全部失败时返回可定位原因或进入显式后备路线
```

预期可以覆盖的大部分区域：

- 单外轮廓、无孔、连通且整体光顺的局部自由曲面。
- 可以被一张规则参数域曲面稳定覆盖，没有内部真实折痕或分支。
- 曲率变化可由受控次数和控制点规模表达。

不能承诺由一张面自动覆盖的区域：

- 带孔、分支、非流形或需要多张参数域的区域。
- 内部存在必须保留的 G0 / G1 不连续、尖脊、折痕或自遮叠。
- 单曲面需要失控次数、控制点或局部翻转才能逼近的形状。

因此长期目标是“单次用户操作 + 内部有限策略组合 + 严格自动验收”。NSR-1.2 只验证其中最小的一步：在相同总训练点和 solver 参数下，改进训练点覆盖是否能稳定通过多数光顺局部区域；不在本阶段实现完整策略调度器。

---

## 4. 原生单曲面拟合方案

### 4.1 第一版算法选择

第一版优先使用 OCCT `BRepFill_Filling`，不立即自研 B-spline 最小二乘求解器。

原因：

- 当前仓库已经在 `g1_boundary_fill_patch_exporter` 中验证了构建、误差读取和 STEP 导出链路。
- `BRepFill_Filling` 支持边界 G0/G1/G2 约束和内部点约束。
- 第一阶段只需要验证“原始边界 + 内部 STEP 点”能否生成一张合格自由曲面。
- 失败后可以根据诊断决定是否需要进入自定义参数化和 B-spline 求解。

与既有 G1 实验的区别：

```text
既有实验：全部边界 G1，主要依赖边界填充，内部形状约束不足。
NSR-1：全部边界 G0，原始角点保留，加入独立的内部 STEP 点约束。
```

### 4.2 拟合输入

拟合输入只来自原始 STEP：

```text
原始 TopoDS_Edge      → 曲面边界约束
原始边界顶点          → 角点位置
候选面内部采样点      → 内部形状约束
候选面内部法向        → 第一版只用于验证
候选面与边界拓扑关系  → 输入合法性和输出方向
```

STL 只保留为显示、诊断和 Geomagic 后备输入，不作为原生拟合的几何真值。

### 4.3 边界约束

按有序外环将原始边直接加入 Filling：

```cpp
filling.Add(edge, GeomAbs_C0, Standard_True);
```

要求：

- 使用原始三维边，不用离散折线替代。
- 保持边界 Wire 的方向和连接顺序。
- 第一阶段不加载外围 support face，不施加 G1。
- 原始边端点必须作为角点保留。
- 生成后检查每条边的 pcurve 和 `SameParameter`。

### 4.4 内部样本

第一版采用确定性、面积加权采样：

1. 对候选区域各原始面建立 OCCT 三角化。
2. 按三角形面积分配样本。
3. 使用固定重心坐标生成训练点。
4. 将训练点重新投影或求值到对应原始面，避免三角近似成为几何真值。
5. 按项目容差做空间去重。
6. 设置总样本上限，防止 Filling 点约束失控。

训练集与验证集必须分离：

- 训练集参与 `filling.Add(point)`。
- 验证集使用不同重心坐标，只用于拟合后误差检查。
- 不允许用训练点误差代替泛化误差。

第一阶段使用固定的稀疏、中等、较密三档样本，不做无限自动调参。candidate 179 的首轮中等档只证明探针可运行，下一轮统一使用以下对照参数：

| 档位 | 训练样本 | 独立验证样本 | 每条边边界样本 | Jacobian 网格 |
|---|---:|---:|---:|---:|
| 稀疏 | 24 | 192 | 32 | 16 |
| 中等 | 48 | 192 | 32 | 16 |
| 较密 | 96 | 192 | 32 | 16 |

三档实验只改变训练样本数量。`tolerance3d=0.03`、法向阈值、Plate 次数、曲线采样、迭代次数、最大次数和最大分段必须保持一致，避免把多种变量混在一次实验中。

2026-07-13 的 candidate 179 三档实测确认除 training 外配置逐字段一致。interior max/p95 从 sparse 的 `0.154917/0.114527`，经 medium 的 `0.093389/0.042576`，降到 dense 的 `0.041323/0.025610`；说明增加训练样本有明显收益。dense 边界和角点通过，但出现 `BRepCheckFailed`，并仍有 1 个法向超限样本，因此下一独立任务继续 Filling 路线并定位结构失败与法向离群点，不在本轮混调边界参数。

### 4.5 构建流程

```text
验证 candidate / boundary
→ 构造 BRepFill_Filling
→ 按顺序加入全部 G0 原始边
→ 加入内部训练点
→ Build
→ IsDone / G0Error
→ Face
→ BRepCheck
→ 曲面有效性检查
→ 独立验证集误差
→ 输出 STEP patch 与 JSON 报告
```

沿用现有 `tolerance3d`、`toleranceAngle`、最大次数和最大分段配置，不在 NSR-1 中建立第二套容差体系。

### 4.6 曲面有效性检查

除了 `BRepCheck`，还必须在 UV 域采样并检查：

- `dU × dV` 是否接近零。
- 法向方向是否发生局部翻转。
- Jacobian 符号是否一致。
- U/V degree。
- U/V pole count。
- U/V knot / segment count。

出现 Jacobian 翻转、近奇异区域或复杂度达到上限时，直接拒绝，不通过继续放宽容差接受。

### 4.7 独立误差报告

报告必须分开记录：

```text
边界：max / p95 / rms / over_tolerance
角点：max / p95 / rms / over_tolerance
内部：max / p95 / rms / over_tolerance
法向：max_angle / p95_angle / rms_angle
```

还要记录：

- 最差样本坐标。
- 最差样本来源 face id。
- 最差边界 edge id。
- 训练/验证样本数量。
- Filling 的 G0/G1/G2 报告值。
- 输出曲面次数、控制点和分段数量。
- `BRepCheck` 结果和失败信息。
- 全部失败质量门；现有单值 `failure_reason` 只保留为兼容的首个失败项。
- 每个诊断样本的原始点、投影点、距离、是否超限、来源 face id / edge id。
- 失败诊断 patch 的语义；`patch_written=true` 不得被解释为拟合验收通过。

NSR-1.1 的诊断产物统一写入：

```text
data/runs/<run-id>/diagnostics/native_surface_fit/
  surface_error_samples.csv
  surface_error_vectors.obj
```

CSV 至少覆盖 boundary / corner / interior 三类样本。OBJ 只用于人工定位超限原始点与投影点之间的误差向量，不作为数值验收来源。

---

## 5. 最小代码落点

### 5.1 `SurfaceRefitter`

`src/merge/SurfaceRefitter.h/.cpp` 已完成 NSR-1 单曲面接口和第一版质量门。NSR-1.1 只补充失败诊断与受控实验，不设计多后端框架，也不改变拟合算法。

建议最小类型：

```text
SurfaceRefitOptions
SurfaceRefitReport
SurfaceRefitResult
SurfaceRefitter::fitSingleFace(...)
```

`SurfaceRefitResult` 返回：

- `TopoDS_Face face`
- 完整 `SurfaceRefitReport`
- 成功或失败状态
- 稳定、可搜索的失败原因

### 5.2 独立探针

新增：

```text
tools/native_surface_fit_probe.cpp
```

命令合同：

```text
native_surface_fit_probe
  --source-step <path>
  --candidate-id <id|auto>
  --output-patch <path>
  --report <path>
```

NSR-1 探针只生成 patch 和报告，不修改 GUI，不自动 Apply。

原生 patch 通过自身质量检查后，再复用现有 `patch_apply_probe` 或 `corner_baseline_probe --apply-only` 验证 Apply，避免第一步就改动 AppController 和 GUI。

### 5.3 测试范围

默认单元测试：

1. 多张共面裁剪面拟合成一张面。
2. 一张已知 B-spline 人工切分后拟合回一张面。
3. 光顺底层曲面加尖角裁剪轮廓，验证角点不圆化。
4. 带孔候选明确拒绝。
5. 非流形或开放边界候选明确拒绝。
6. 训练集与验证集分离。
7. Jacobian 翻转或近奇异结果被拒绝。

真实样例：

- candidate 179 用于手动探针和 run artifact，不写入默认单元测试。
- 真实文件测试必须存在性门控。
- 不在测试和生产代码中硬编码 `03_配件_Clay.stp` 或候选序号。

---

## 6. NSR-1 失败分流

| 失败类型 | 处理方式 |
|---|---|
| 原始边界 Wire 无法构造 | 修边界顺序、方向或输入分析，不进入拟合调参 |
| G0 边界误差超限 | 检查边约束和曲线兼容性，不提高 Sewing 容差 |
| 边界合格但内部误差大 | 在固定三档样本/复杂度范围内比较一次 |
| 距离合格但法向误差大 | 记录为需要切平面约束，NSR-1 返回失败 |
| Jacobian 翻转或近奇异 | 直接拒绝单曲面结果 |
| 控制点或分段数量失控 | 判定单曲面简化收益不足 |
| `BRepFill_Filling` 不收敛 | 记录 OCCT Plate 路线失败，不无限调参 |

只有在 NSR-1 对代表性样例失败且失败原因指向拟合能力不足时，才启动下一阶段的显式参数化和 B-spline 约束最小二乘；该阶段不属于当前任务。

### 6.1 candidate 179 当前失败判读

首轮中等档产物：

```text
data/runs/20260713_200400_711__native-surface-fit__03_配件_Clay__cand-0179__nsr-1-medium/
```

已确认：

| 检查项 | 结果 | 判读 |
|---|---:|---|
| 边界 | 12 / 416 超限，max=0.0381795 | 大部分边界合格，edge 1540 附近局部超限 |
| 角点 | 2 / 26 超限，max=0.0381795 | 与最差边界位置重合 |
| 内部 | 6 / 96 超限，max=0.0933887，p95=0.0356314 | 当前更显著的形状误差，最差来源 face 1709 |
| 法向 | max=14.1263°，阈值 15° | 通过但接近上限，不能抵消位置误差 |
| Jacobian | 无翻转、无近奇异 | 参数化健康 |
| BRep | 有效、STEP 二次读取为一张面 | 只证明结构合法，不证明几何贴合 |

`BoundaryErrorExceeded` 是按边界、角点、内部顺序返回的首个失败项，不表示内部验证通过。当前结果应归类为“边界局部超限 + 内部自由曲面欠拟合”；诊断 STEP 是失败产物，不能进入 NSR-2。

NSR-1.1 已补齐全部失败门和样本级定位产物，并完成固定三档对照：

```text
若训练样本增加后内部误差明显下降：继续评估 OCCT Filling 单曲面路线。
若内部误差基本不变且 U/V degree 持续达到 8：记录 Plate 路线能力不足证据，再评估 NSR-4。
若主要只剩 edge 1540 边界局部超限：另立边界约束参数实验，不与训练样本实验混跑。
```

实际结果：

| tier | failed_gates | boundary max/p95/RMS/over | corner max/p95/RMS/over | interior max/p95/RMS/over | normal max/p95 | degree/poles/segments | Jacobian | BRepCheck |
|---|---|---|---|---|---|---|---:|---:|
| sparse | InteriorErrorExceeded, NormalErrorExceeded | 0.014135/0.008378/0.003548/0 | 0.012527/0.011608/0.004509/0 | 0.154917/0.114527/0.053045/89 | 58.4834°/14.7188° | 8x8/529/9 | true | true |
| medium | BoundaryErrorExceeded, CornerErrorExceeded, InteriorErrorExceeded, NormalErrorExceeded | 0.038180/0.025657/0.011631/24 | 0.038180/0.034959/0.014290/2 | 0.093389/0.042576/0.019742/18 | 47.9057°/9.3050° | 8x8/256/4 | true | true |
| dense | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.015591/0.009086/0.004321/0 | 0.015591/0.013647/0.005295/0 | 0.041323/0.025610/0.010416/4 | 45.4750°/9.7698° | 8x8/529/9 | true | false |

结论：虽然三档 degree 都达到 8，但内部 max/p95 和法向超限数随训练密度明显改善，不能把当前结果归类为“Plate 能力基本不变”。medium 的最差边界仍是 edge 1540，dense 中该边仍最差但误差已低于 0.03，因此暂不进入边界约束实验。当前继续 OCCT Filling，先另立 dense BRepCheck / 法向离群诊断；所有 patch 都是失败诊断产物，不进入 NSR-2。

补充诊断：三档 patch 写出 STEP 后重新读取均为一张 face 且 `BRepCheck=true`，但 sparse / medium / dense 的 edge 数分别为 44 / 46 / 50，而内存态 face 均遍历到 26 条边。当前实现又是在输出方向反转前记录 `brep_check_valid`。因此 dense 的 `BRepCheckFailed` 尚不能直接归类为底层曲面失效，也不能用 STEP roundtrip 自动接受；NSR-1.2 必须区分 Filling 输出、方向反转后和 translator 重建后的具体失败状态。

---

## 7. Geomagic 后备路线

### 7.1 启动条件

仅在以下情况启动：

- 候选不满足原生单曲面支持范围。
- 原生拟合在受控三档参数内明确失败。
- 原生结果出现 Jacobian 翻转、近奇异或复杂度失控。

不得因为希望节省一次原生拟合而默认跳转 Geomagic。

### 7.2 后备流程

```text
原始 STEP candidate
→ 生成 fitting STL / 必要的真实邻接曲面窄带
→ Geomagic AutoSurface
→ 导入 raw patch STEP
→ 底层曲面覆盖 Gate
→ original-boundary constrained re-trim
→ Apply / repair / StrictTopologyGate
→ 商业 CAD 类质量门
```

### 7.3 覆盖 Gate

Geomagic patch 在 Apply 前必须满足：

- 每个原始边界采样点可投影到至少一张导入底层曲面。
- 所有原始角点可投影到导入底层曲面。
- 最大、P95、RMS 投影距离均记录。
- 连续未覆盖边界区间为空。
- 不允许通过提高 Sewing 容差掩盖底层曲面欠覆盖。

覆盖 Gate 失败时直接拒绝，不进入 strict re-trim 和 Apply。

### 7.4 Geomagic 输出语义

Geomagic 只提供候选拟合曲面，不拥有最终 CAD 拓扑决定权：

- 不信任 Geomagic patch outer boundary。
- 不把 Geomagic 内部 seam 自动视为理想最终 seam。
- 不把拓扑 Gate 通过视为几何质量通过。
- 不提交 raw patch 或 pre-repair shape。
- 最终边界仍来自原始 STEP。

---

## 8. 开发阶段

### NSR-0：证据与方案锁定

状态：已完成。

- 确认 fitting STL 角点完整。
- 确认 raw Geomagic 底层曲面角点圆化。
- 确认问题不是归一化、pcurve 或 Sewing 单独造成。
- 建立本指导文档。

### NSR-1：原生单曲面拟合探针

状态：工程探针已完成；candidate 179 首轮中等档未通过 0.03 几何容差。

验收：

- `SurfaceRefitter::fitSingleFace` 可用。
- G0 原始边界 + 内部 STEP 点约束可运行。
- `native_surface_fit_probe` 输出 patch 和 JSON。
- 合成测试通过。
- candidate 179 产生可审阅 run artifact，成功或失败均有可定位报告。

### NSR-1.1：失败诊断与固定三档实验

状态：已完成。

- 已报告全部失败质量门，同时保留兼容的首个 `failure_reason`。
- 已输出样本级 CSV 和超限误差向量 OBJ。
- 已补齐 solver 配置、candidate face 数、boundary valid 和诊断 patch 语义。
- 已对 candidate 179 运行稀疏、中等、较密三档；三档只改变训练样本数量。
- 结果支持继续 OCCT Filling；下一独立任务定位 dense BRepCheck 失败和法向离群，不启动 NSR-4。
- 本阶段未启动 NSR-2。

### NSR-1.2：dense 结构诊断与受控采样

状态：当前任务。

- 补齐 Filling 输出、方向反转后和 STEP 二次读取后的 BRepCheck 与具体子形状状态。
- 补齐每张源面的训练 / 验证样本覆盖，以及法向样本级诊断。
- 固定 training=96、validation=192 和全部 solver 参数，只比较纯面积加权与“每张源面至少一点 + 剩余点面积加权”。
- 不调用 repair 掩盖原生 patch 问题，不把 STEP roundtrip 后 BRepCheck 通过直接当成拟合通过。
- 只有至少一个受控策略通过全部原生质量门，才结束 NSR-1 并进入 NSR-2。

### NSR-2：复用现有 Apply 验证

状态：NSR-1 通过后启动。

- 先通过已有 probe 的 `--patch` / `--apply-only` 路径验证。
- 保留 repair、`StrictTopologyGate` 和 STEP roundtrip。
- 不先改 GUI。

### NSR-3：主流程接入

状态：NSR-2 通过后启动。

- 在 AppController 中增加原生拟合路线。
- GUI 只在真实样例通过后增加入口。
- Geomagic 保持后备路线。

### NSR-4：自定义 B-spline 求解

状态：冻结。

只有 OCCT Plate 路线被证据证明无法满足内部误差或法向质量时才解冻。

### NSR-5：原生多曲面布局

状态：冻结。

不属于当前单曲面目标。

---

## 9. 当前验证命令

日常构建和测试优先使用：

```powershell
cd D:\pyProject\step-patch-optimizer
.\scripts\build_debug.ps1
.\scripts\test.ps1
.\scripts\run_native_surface_fit_tiers.ps1 -SourceStep "<source-step>" -CandidateId <id>
```

底层验证：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

真实样例不得进入默认测试。探针运行必须写入唯一的 `data/runs/<run-id>/` 目录并保留参数、报告和产物清单。三档运行器成功时固定保留 sparse、medium、dense 和 comparison 四个正式目录；中途异常时只清理本次调用已创建且确认位于 `data/runs/` 下的目录，不删除历史 run。

---

## 10. 文档同步规则

每完成一个 NSR 子阶段：

1. `docs/TODO.md` 只勾选实际完成的具体子项。
2. `docs/implementation_status.md` 记录实现状态和验证证据。
3. 已完成、已验证或退役事实移入 `docs/completed_features.md`。
4. 若主流程或目录职责改变，同步 `README.md`、`docs/module_design.md` 或 `docs/project_structure.md`。
5. Geomagic 后备语义改变时同步 `docs/geomagic_patch_workflow.md`。

---

## 11. NSR-1.1 已执行任务模板（归档）

```text
请在真实仓库 D:\pyProject\step-patch-optimizer 中完成 NSR-1.1：原生单曲面失败诊断与固定三档受控实验。

开始前：
1. 严格遵守仓库 AGENTS.md。
2. 按规定顺序阅读 README.md、docs/module_design.md、docs/implementation_status.md、docs/completed_features.md、docs/run_gui.md、docs/project_structure.md、docs/TODO.md。
3. 额外完整阅读 docs/native_surface_refitting_guide.md；只有实际改动 Geomagic / Apply 主线时才读取并修改对应 Geomagic 文档。
4. 说明已阅读文档、任务假设和成功标准。
5. 检查 git status，保护用户已有改动。
6. 确认当前 Git 分支为 native-surface-refit；不得在 feature-bounded-refit 上实现本任务，也不得覆盖或回滚该分支的 Geomagic 稳定主线。
7. 修改任何函数、类或方法前，先运行 GitNexus impact，并报告影响范围；若为 HIGH 或 CRITICAL，先停止并说明风险。

已知基线：
- source：data/stp/03_配件_Clay.stp。
- candidate：179，仅用于手动真实探针。
- run：data/runs/20260713_200400_711__native-surface-fit__03_配件_Clay__cand-0179__nsr-1-medium
- tolerance3d=0.03。
- boundary：12/416 超限，max=0.0381795，worst edge=1540。
- corner：2/26 超限，max=0.0381795。
- interior：6/96 超限，max=0.0933887，p95=0.0356314，worst face=1709。
- normal max=14.1263°，Jacobian 和 BRepCheck 通过。
- 当前 failure_reason=BoundaryErrorExceeded 只是首个失败门，不能掩盖 corner / interior 同时失败。

本次只做两部分。

第一部分：失败诊断可解释化。
1. 保留现有 failure_reason 的兼容语义，新增 failed_gates 数组，列出本轮全部失败门。
2. 报告 candidate_face_count、boundary_valid、完整 solver 配置和 diagnostic_patch=true/false。
3. 保留 boundary / corner / interior 的聚合统计，并新增样本级诊断：类别、原始点、投影点、距离、是否超限、face id、edge id。
4. 在 run 的 diagnostics/native_surface_fit/ 下输出：
   - surface_error_samples.csv
   - surface_error_vectors.obj
5. OBJ 只写超限样本的原始点、投影点和连线，用于人工定位；数值验收仍以 JSON/CSV 为准。
6. patch_written=true 但 fit_success=false 时，报告必须明确它是失败诊断 patch，不是可接受结果。
7. 增加合成测试，至少覆盖“边界先失败但内部也失败时 failed_gates 同时包含两者”和诊断文件字段合同。

第二部分：固定三档受控实验。
1. 使用同一 source STEP 和 candidate 179，分别运行：
   - sparse：training=24
   - medium：training=48
   - dense：training=96
2. 三档统一使用 validation=192、boundary samples per edge=32、Jacobian grid=16、tolerance3d=0.03、normal tolerance=15°。
3. Plate degree、points_on_curve、iterations、max_degree、max_segments 等 solver 参数保持不变；本轮只改变训练样本数量。
4. 每档必须写入独立 data/runs/<run-id>/，保留 run.json、RUN.md、patch STEP、JSON、CSV、OBJ 和日志。
5. 生成一份三档比较表，至少包含 success、failed_gates、boundary/corner/interior max/p95/RMS/超限数、normal max/p95、degree、poles、segments、Jacobian、BRepCheck 和耗时。
6. candidate 179 只允许出现在手动探针命令和 run artifact 中，不得写进默认测试或生产逻辑。

本次不要做：
- 不改 GUI。
- 不做原生多曲面布局。
- 不启动 NSR-2、候选区域替换或 Apply。
- 不启动 NSR-4，不实现自定义 B-spline 求解器。
- 不接 Geomagic 自动回退。
- 不给全部边界施加 G1。
- 不调整 Plate、points_on_curve、iterations、max_degree、max_segments。
- 不放宽 0.03 几何容差或 15° 法向阈值。
- 不放宽 StrictTopologyGate、Sewing 或商业 CAD 类质量门。
- 不重构无关代码。

验证：
1. 优先运行 .\scripts\build_debug.ps1 和 .\scripts\test.ps1。
2. 必要时补充 cmake --build --preset windows-msvc-debug 和 ctest --preset windows-msvc-debug --output-on-failure。
3. 运行三档 candidate 179 手动探针并检查每个 run 清单和诊断产物存在。
4. 完成后运行 GitNexus detect_changes，对比 feature-bounded-refit，确认只影响预期符号和流程。
5. 同步 docs/TODO.md、docs/implementation_status.md、docs/completed_features.md 和 docs/native_surface_refitting_guide.md 的具体子项。

结果判读：
- 如果训练样本增加使 interior max/p95 明显下降，说明 OCCT Filling 路线仍值得继续。
- 如果 interior 误差基本不变且 U/V degree 持续达到 8，记录为 Plate 路线能力不足证据，但本次仍不实现 NSR-4。
- 如果内部收敛但 edge 1540 边界仍局部超限，把边界约束参数实验列为下一独立任务，不在本轮混调。

最终报告必须包含：
- 修改文件。
- GitNexus impact 风险。
- 实际验证命令与结果。
- 三档 run 路径和对比表。
- candidate 179 的全部 failed_gates 与误差趋势判读。
- 明确说明是否继续 OCCT Filling、转入边界约束实验，或只为 NSR-4 记录证据。
- 未完成项和下一步；不要擅自实现 NSR-2 或 NSR-4。
```

---

## 12. 下一步开发 Prompt：NSR-1.2

```text
请在真实仓库 D:\pyProject\step-patch-optimizer 中完成 NSR-1.2：dense 结构诊断与固定 96 点采样分配实验。

开始前：
1. 严格遵守仓库 AGENTS.md。
2. 按规定顺序完整阅读 README.md、docs/module_design.md、docs/implementation_status.md、docs/completed_features.md、docs/run_gui.md、docs/project_structure.md、docs/TODO.md。
3. 额外完整阅读 docs/native_surface_refitting_guide.md。
4. 说明已阅读文档、任务假设和成功标准。
5. 检查 git status，保护用户已有修改；确认当前分支为 native-surface-refit。
6. 修改任何函数、类或方法前，先运行 GitNexus impact；HIGH 或 CRITICAL 时先报告风险。

已知基线：
- source：data/stp/03_配件_Clay.stp。
- candidate：179，仅用于手动真实探针，不得写入生产逻辑或默认测试。
- NSR-1.1 comparison：data/runs/20260713_234510_378__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.1-three-tier/。
- dense：training=96、validation=192、boundary samples per edge=32、Jacobian grid=16。
- tolerance3d=0.03，normal tolerance=15°。
- dense boundary / corner 全部通过；interior 4/192 超限，max=0.0413227，p95=0.0256105。
- dense normal 1/192 超限，max=45.475°，最差 face=1705。
- dense 内部超限 face=1719、1717、1750、1710。
- dense 内存态 BRepCheck=false，missing pcurve=0，SameParameter failure=0，Jacobian 无翻转 / 近奇异。
- 三档 STEP 二次读取均为一张 face 且 BRepCheck=true，但 edge 数为 44/46/50；内存态均为 26 条边。
- 当前代码在输出方向反转前记录 brep_check_valid，因此必须先明确检查时机和真实失败子形状。

本次目标分为两阶段，必须先完成诊断，再决定是否执行采样 A/B；不得直接修改 solver 参数。

第一阶段：BRep 与离群点诊断。
1. 在 SurfaceRefitReport 中分别记录：
   - filling_face_brep_check_valid
   - oriented_face_brep_check_valid
   - step_roundtrip_brep_check_valid（仅 probe 层执行并报告）
2. 输出 face、wire、edge、vertex 的具体 BRepCheck_Status；必须能定位失败发生在哪类子形状和哪条输出边。
3. 对每条输出边记录：
   - edge index / 对应原始 boundary edge id（能够映射时）
   - tolerance
   - degenerated
   - SameRange / SameParameter
   - pcurve 是否存在及参数范围
   - 3D curve 与 curve-on-surface 的采样最大偏差
4. 不调用 ShapeFix、Sewing 或放宽 tolerance 修复；本阶段只诊断。
5. 报告每张候选源面的 training_count、validation_count，以及每个验证离群点到最近训练点的距离。
6. 为法向验证增加样本级记录：source point、projected point、source normal、fitted normal、angle、UV、face id、projection success、over tolerance。
7. 为法向超限样本输出单独 CSV；可选 OBJ 用线段显示源法向与拟合法向，但数值验收仍以 JSON/CSV 为准。
8. 增加合成测试，覆盖 BRep 状态序列化、方向反转前后字段、每面样本统计和法向样本字段合同。

第二阶段：固定 96 点采样分配 A/B。
只有第一阶段报告能够明确生成后，再运行两个策略：
A. area_weighted：当前纯面积加权，training=96。
B. face_stratified_area_weighted：每张源面至少分配一个确定性训练点，剩余训练点继续按面积加权；总数仍为 96。

两档必须保持以下配置逐字段一致：
- validation=192
- boundary samples per edge=32
- Jacobian grid=16
- tolerance3d=0.03
- normal tolerance=15°
- plate_degree=3
- points_on_curve=24
- iterations=3
- max_degree=8
- max_segments=12
- 其他 SurfaceRefitOptions 不变

采样要求：
1. 策略必须确定性、训练 / 验证互斥、空间去重。
2. 每张源面至少一点只适用于候选面数不超过训练预算的情况；否则明确返回 unsupported / invalid configuration，不静默超预算。
3. 不针对 face 1705、1719、1717、1750、1710 写死任何特殊逻辑。
4. 不增加总训练点，不做 residual adaptive 第三轮，不改变 solver 参数。

输出与比较：
1. 两个策略分别写入唯一 data/runs/<run-id>/。
2. 每轮保留 run.json、RUN.md、patch STEP、JSON、距离 CSV/OBJ、法向 CSV 和日志。
3. 比较表至少包含：
   - 全部 failed_gates
   - boundary / corner / interior max、p95、RMS、超限数
   - normal max、p95、RMS、超限数
   - 每面 training min / max / zero-count
   - degree、poles、segments
   - Jacobian
   - 三阶段 BRepCheck
   - STEP roundtrip face / edge 数
   - 耗时

通过标准：
1. 输出仍为一张拟合面。
2. 边界、角点、内部距离和法向全部满足既有容差。
3. oriented face 的 BRepCheck 通过；不能只依赖 STEP roundtrip 后通过。
4. 无 Jacobian 翻转或近奇异。
5. 曲面复杂度不超过现有 NSR-1 限制。
6. 只有满足全部条件的策略可以 patch_acceptable=true。

本次不要做：
- 不改 GUI、AppController、Apply、PatchReplacementRepair 或 Geomagic。
- 不启动 NSR-2、NSR-3、NSR-4 或 NSR-5。
- 不增加训练样本总数到 192。
- 不给全部边界或内部点施加 G1。
- 不调用 ShapeFix / Sewing 把诊断失败变成成功。
- 不放宽 0.03、15°、StrictTopologyGate 或商业 CAD 类质量门。
- 不实现完整多策略调度器；本轮只建立两个确定性采样策略的证据。
- 不重构无关代码。

验证：
1. 优先运行 .\scripts\build_debug.ps1 和 .\scripts\test.ps1。
2. 运行 candidate 179 两策略真实探针并检查全部 run artifacts。
3. 用 step_stats 验证每个诊断 patch 的 STEP 二次读取 face / edge / BRepCheck。
4. 运行 git diff --check。
5. 提交前运行 GitNexus detect_changes，对比 feature-bounded-refit，确认没有影响 GUI、Apply 或 Geomagic 流程。
6. 同步 docs/TODO.md、docs/implementation_status.md、docs/completed_features.md 和 docs/native_surface_refitting_guide.md。

最终报告必须包含：
- 修改文件与 GitNexus 风险。
- BRepCheck 的具体失败状态、子形状和检查阶段。
- face 1705 法向离群点的完整向量信息。
- 4 个内部超限点与每面训练覆盖的关系。
- 两种采样策略的完整比较表和 run 路径。
- 是否已有 patch_acceptable=true 的结果。
- 是否可以进入 NSR-2；若不能，明确下一步是局部切平面约束证据还是 NSR-4 证据，不要擅自实现。
```
