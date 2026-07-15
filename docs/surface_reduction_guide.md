# 任意选区自适应曲面减面指南

## 1. 产品合同

GUI 通过 `OccViewWidget::selectedFaceIds()` 获取一个连通选区。输入和输出面数完全由选区和拟合结果决定，不允许固定候选、面编号、分组或目标面数。

设输入面数为 `N`：

- `N < 2`：`InvalidSelection`；
- `output_faces < N` 且全部质量门通过：`Reduced`；
- `output_faces <= floor(N * targetOutputRatio)`：`TargetAchieved`，默认比例为 `0.5`；
- 没有合法合并：`NoAcceptableReduction`，不得 Apply。

默认质量门为最大距离 `0.03 mm`、最大法向差 `15°`、保留普通光顺接缝 `5°`。锁边、选区外边界和超过光顺阈值的明显接缝不可跨越。

## 2. 数据流

```text
原始 ShapeDocument
→ 校验选区连通性
→ 可选：按 STEP 修剪面域从原 STL 裁剪选区形貌
→ 可选：建立裁剪 STL 的单图调和参数化参考
→ 每个原始面作为初始分组
→ 从 TopologyGraph 构造相邻分组队列
→ 锁边、尖锐边和选区边界保护
→ 每次只拟合两个相邻分组的并集
→ 始终回到原始 FaceId 对应的 STEP 曲面采样
→ 模型选择集通过后再执行独立终验
→ 原始外边界重裁剪和参数曲线重建
→ 保守 B-rep repair
→ StrictTopologyGate + STEP 回读
→ GUI 预览
→ ReduceSelectedFacesCommand 事务 Apply
```

扩大分组时不得采样上一次拟合结果。局部三角细分只用于原始面 UV 域内的面积加权点位分配；所有验收点、法向均重新由原始 `Geom_Surface` 求值。加载原 STL 时，STL 只提供选区内部形貌参考，不替代 STEP 边界和终验基准。

## 3. 受限 B-spline 拟合

`RestrictedBSplinePatchFitter` 使用面积最大的原始面初始化参数：

1. 三次、夹持、非有理、`4×4` 控制网起步；
2. Eigen `SparseQR` 联合求解三维控制点；
3. 目标包含位置、边界位置、法向切平面和系数 `0.01` 的二阶公平性；当前边界样本只是位置权重乘 `2` 的软约束，不是等式硬约束；
4. 非混合路径每次求解后重新投影训练点更新 UV，最多五次，位移低于 `1e-8` 收敛；混合路径当前每个节点布局只求解一次，不执行这五轮 UV 重投影；
5. 模型选择失败时在最大残差所在参数区间插入节点；
6. 控制点上限为 `min(maximumControlPoints, trainingSampleCount / 2)`；
7. 参数重叠、投影失败、Jacobian 翻转或近奇异立即拒绝当前组合。

训练、模型选择和独立终验使用不同的 Halton 序列并执行空间去重。样本预算由面积、边界长度和三维容差计算，不使用原始面数作为预算规则。

启用 STEP/STL 混合路径时：

1. `StlRegionExtractor` 以 STEP 修剪面域和边界带从原 STL 保留选区三角形；也可由探针显式传入已经裁好的选区 STL；
2. 选区 STL 必须是单一边界环且参数图无翻转、无近奇异；边界按弧长映射到严格凸圆，内部用正权调和方程求解；
3. 原 STEP 样本先投到裁剪 STL 取得参考点和调和参数，但当前实现随后用这些参考点的局部 PCA 坐标覆盖最终拟合 UV；调和 UV 尚未成为最终拟合参数；
4. STL 参考点只以权重 `0.25` 的位置软约束参与拟合，STL 法向权重为 `0`；
5. 原 STEP 边界在拟合阶段仍是软约束，拟合后通过原始边界重裁剪和独立密集边界终验执行 `0.03 mm` 硬拒绝；模型选择、独立终验、商业 CAD 门和全局拓扑门仍以 STEP 为准。

## 4. B-rep 和事务边界

- 成功分组使用原始外轮廓和内孔重裁剪；精确同域合并直接保留全部线框，拟合路径无法可靠重建任一线框时只拒绝该分组；
- 未参与成功合并的面保持原始几何和拓扑；
- 邻接输出面沿原始边界复用三维边，并各自保存参数曲线；
- 单实体、单壳模型中，严格重裁剪生成的新边界边先与未替换面组成面复合体，再执行固定容差 Sewing，避免把独立边直接嵌回旧 Solid 后形成自由边；其他容器保持保守 ReShape 路径并由全局质量门决定是否接受；
- repair 不允许通过 same-domain 全局统一或自适应 Sewing 制造额外成功；
- 商业 CAD 类质量门对每个成功分组复核原始边界、角点、特征边和接缝法向偏移；
- `StrictTopologyGate` 检查 BRepCheck、自由边、多重边、实体、壳体、包围盒和 STEP 回读；
- 预览不修改文档；Apply、撤销和重做只交换缓存的 `ShapeDocument`，不重新拟合。

## 5. 分支和阶段

| 阶段 | 状态 |
|---|---|
| SR-0 新分支与旧路线冻结 | 已完成 |
| SR-1 原始 STEP 精确采样和质量报告 | 已完成第一版 |
| SR-2 动态分组和受限 B-spline 拟合 | 已完成第一版 |
| SR-3 多分组组装和全局验证 | 已完成第一版 |
| SR-4 GUI、Apply、撤销和重做 | 已完成第一版 |
| SR-5 十二个真实选区基准 | 已完成第一版 |
| SR-6 STEP/STL 混合形貌参考 | 已完成第一版 |

`surface_reduction_probe` 支持 `--source-stl <原始.stl>` 动态执行 STEP 面域裁剪，也支持 `--cropped-stl <选区.stl>` 复用已验证裁剪；选区可用 `--candidate-id <id>` 动态发现，或直接给出任意数量的 FaceId。`surface_reduction_benchmark` 从平滑内部边和现有候选属性发现选区，并动态记录输入、输出、质量指标和拒绝原因；生产逻辑和默认测试不包含 candidate 179。

2026-07-15 的最终真实基准位于 `data/runs/20260715_surface_reduction_benchmark_final12b/`：四个真实 STEP 共 12 个选区，3 个选区由 2 面减为 1 面并达到 50% 目标，9 个选区无合法减少；三个成功结果的参数化、BRepCheck、STEP 回读和商业 CAD 类边界门均为 `true`。合成测试另行覆盖部分减少、孔洞、高曲率 B-spline、锁边和尖锐边。

同日对 `data/stp/03_配件_Clay.stp` 的 candidate 179 做了 SR-6 独立手动回归。该候选动态发现为 FaceId 1702～1750，共 49 面。使用动态裁剪的原始扫描 STL、目标比例 `0.98` 时得到 `49→48`；使用缓存参考 STL、默认 50% 目标时，引擎尝试 212 个相邻组合，接受 `1728+1729`、`1736+1741`、`1738+1739` 三组，得到 `49→46` 并返回 `Reduced`。后者最大距离 `0.0286183 mm`、最大法向差 `0.699484°`；参数化、商业 CAD 门、BRepCheck、STEP 回读、零自由边和零多重边全部通过，生成 `candidate_0179_hybrid.step`。

缓存文件 `candidate_0179_reference_crop.stl` 不是 NSR-1 产物，也不是独立扫描几何。它与 `data/crop_stl/03_配件_Clay/03_配件_Clay_candidate_0179.stl` 的 SHA-256 均为 `7D094762B7B7FFE31244B65B7066DD5176B6DB85B21A8A1E360C01DF60EF516C`，来源是 `20260713_155502_505__boundary-trim-fill` 的 STEP 采样网格：`BoundaryConstrainedOcct`、线性偏差 `0.04`、角度偏差 `0.35`，基础网格 `5172` 个三角形，再增加 `204` 个结构化窄带三角形，共 `5376` 个三角形。因此它与 STEP 几何约束同源，不能作为独立几何真值；当前最多只能提供参数化脚手架和重复的弱位置参考。

唯一结果目录为 `data/runs/20260715_surface_reduction_candidate_0179_hybrid/`。

## 6. 验证命令

```powershell
cd D:\pyProject\step-patch-optimizer
.\scripts\build_debug.ps1
.\scripts\test.ps1
git diff --check
```

真实文件基准必须单独运行并把输入面数、输出面数、距离、法向、接缝、拒绝原因和 STEP 回读结果写入唯一运行目录。
