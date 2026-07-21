# Surface Reduction 指南

状态：当前产品主线。

本文定义 SurfaceReductionEngine 的产品合同、输入模式、搜索语义、拟合边界、质量门和运行证据。当前验证快照见 implementation_status.md。

## 产品合同

SurfaceReductionEngine 接收当前 ShapeDocument 中任意连通的 FaceId 集合。GUI 先检测特征边，由 MergePlanner 动态生成候选，再把当前 MergeCandidate::faces 作为请求。

禁止：

- 固定真实 candidate ordinal。
- 在通用实现中写死 FaceId 或 EdgeId。
- 预设某个样例的输入、输出面数或接受分组。
- 从普通 GUI face 选择推断已保存候选。
- 用父候选拟合面作为扩展分组的真值。

设输入面数为 N：

- N 小于 2：InvalidSelection。
- 输出面数小于 N 且全部门禁通过：Reduced。
- TargetRatio 达到目标比例：TargetAchieved。
- MinimizeFaceCount 达到受保护边图下界：OptimalProven。
- MinimizeFaceCount 有通过结果但未达到下界：BestFoundWithinBudget。
- 预算内没有通过结果：NoValidatedResultFound。
- 没有合法合并：NoAcceptableReduction。

只有通过结果可以 Apply。

OptimalProven 只对当前硬边界、每个 Cluster 由一个替换面表示、当前拟合选项和当前质量门成立；它不是无约束曲面分割问题的数学全局最优证明。

## 输入模式

### StepBrepOnly

默认模式。

- STEP 修剪面和 Geom_Surface 是拟合真值。
- 原始边界、模型选择和终验都来自 STEP。
- 不需要扫描 STL。

### STEP 加扫描 STL 软参考

必须显式选择并提供来源明确的扫描 STL。

- STL 只为选区内部形貌提供位置软目标。
- 参数域、边界和终验仍来自 STEP。
- STEP 与扫描指纹进入不同缓存键。
- 加载 STL 不会自动切换模式。

WholeModel、SelectionCrop 与 None 必须在报告中区分。原始扫描、缓存裁剪和 STEP 采样 mesh 不可互换命名。

## 数据流

~~~text
原始 ShapeDocument
  -> 特征边与锁边
  -> 动态候选区域
  -> 选区连通性和硬边界
  -> 原始面初始分组
  -> 理论下界直探或预算搜索
  -> 从原始 STEP 面进行受限拟合
  -> 分组重裁剪
  -> 候选装配与保守修复
  -> 几何、接缝与商业 CAD 类门
  -> StrictTopologyGate
  -> STEP 往返
  -> GUI 预览
  -> ReduceSelectedFacesCommand 事务 Apply
~~~

预览不改变 ShapeDocument。Apply、撤销和重做交换已验证的文档状态，不重新执行昂贵拟合。

## 硬边界

下列边不得被候选内化：

- 用户锁定边。
- 选区外边界。
- 非流形边。
- 明显折角。
- 产品配置指定的其他保护边。

普通光顺接缝可以被合并，但最终结果仍需满足距离、法向、边界和保留接缝质量门。

默认阈值以运行配置和报告为准。修改阈值时需要保持单位、语义和回归证据，不在本文维护每次实验数值。

## 搜索模式

### TargetRatio

兼容锁定比例回归：

- 以目标输出比例决定停止条件。
- 找到达到目标且通过门禁的结果即可返回。
- candidate-179 49→46 基线使用此模式和显式扫描 STL 软参考。

### MinimizeFaceCount

GUI 默认产品目标：

- 先计算受保护边图连通分量下界。
- 可直接探测下界分区。
- 使用确定性束搜索探索组+组、组+单面和单面+单面提案。
- 在局部条件满足时允许有限三组跳级。
- 以拟合数、最终验证数、墙钟和档案大小控制预算。
- 归档通过结果，并按输出面数和稳定次序选择当前最好结果。

预算耗尽、前沿耗尽或没有进一步有效提案都不自动构成最优证明。

## 拟合

RestrictedBSplinePatchFitter 对每个分组：

1. 从原始 STEP 面重新采样。
2. 优先尝试合适的原始支撑面参数投影。
3. 必要时建立 STEP 调和参数图。
4. 构造夹持的非有理 B-spline。
5. 以原始边界锚点建立等式约束。
6. 使用内部位置、法向、公平性和可选扫描软项求解。
7. 执行 UV 迭代、回溯和模型选择。
8. 检查参数域、边界、留出误差和支撑曲面包络。

边界硬约束、独立终验和商业 CAD 类门不会因启用 STL 而放宽。

参数图翻转、投影失败、约束残差超限、支撑面包络超限或 Jacobian 异常都会拒绝当前分组。

## 重裁剪与装配

通过拟合的分组使用原始外轮廓和内孔重裁剪。

- 精确同域合并保留全部适用线框。
- 拟合路径不能可靠重建任一线框时拒绝该分组。
- 未参与合并的面保持原始几何。
- 邻接输出面复用适用的三维边，并建立各自参数曲线。
- SameParameter 和连续参数投影接受稠密终验。
- 单实体、单壳模型可在固定容差下进行受控 sewing。
- repair 不允许通过全局同域统一或自适应放宽制造成功。

## 最终质量门

最终候选按适用顺序执行：

1. 原始边界、角点、特征边和接缝偏移。
2. 最大距离、法向和保留光顺接缝。
3. 参数化、支撑面包络与 SameParameter。
4. PatchReplacementRepair。
5. 全局 BRepCheck。
6. StrictTopologyGate。
7. STEP 写出与重新读入。
8. CommercialCadQualityGate 或外部 CAD 证据。

任一门禁失败：

- 不得 Apply。
- 不进入 CommandHistory。
- 报告保留阶段化拒绝原因。
- 搜索可以在预算内继续探索其他状态。

## 缓存

SurfaceReductionFitCache 以归一化 FaceId 集合和输入变体寻址，键覆盖：

- STEP 指纹。
- 可选扫描指纹。
- 全部影响拟合的选项。
- 拟合算法指纹。
- 影响二进制结果的构建身份。

允许写入：

- 成功拟合。
- 带明确几何失败阶段的确定性负结果。

不写入负缓存：

- 缓存缺失。
- 墙钟预算。
- 未分类异常。
- 外部基础设施失败。

执行模式：

- FitSet：建立或检查选定分组的拟合事实。
- Replay：只重放兼容缓存。
- Full：执行完整搜索和终验。
- RefreshCache：忽略旧条目并刷新。

缓存命中不跳过最终候选装配和质量门。

## GUI 合同

GUI 入口位于“曲面减面”菜单和主工具栏。

操作顺序：

1. 检测特征边。
2. 选择候选区域。
3. 选择拟合输入模式。
4. 原生重拟合预览。
5. 检查报告。
6. Apply 或放弃。

状态化按钮在“预览”和“应用”间切换。“一键预览并应用”只在同一候选的预览通过全部质量门后提交。

模型、候选、输入模式或参数变化会使旧预览失效。后台预览固定报告心跳、搜索层、拟合计数、活动分组和终验阶段。

详细交互见 run_gui.md。

## 探针

surface_reduction_probe 支持：

- --search-objective target-ratio|minimize-face-count
- --candidate-id <id> 动态发现候选
- 显式 FaceId 集合
- --source-stl <原始扫描>
- --cropped-stl <已验证选区裁剪>
- --maximum-final-validations
- --maximum-skip-triples
- --search-time-budget-ms
- --selection-output <选区 STEP>

具体可用选项以工具 --help 为准。

step_stats <文件> --curve-on-surface 用于检查三维边与参数曲线偏差及支撑曲面范围。

surface_reduction_benchmark 保留 TargetRatio 基准合同；默认测试不依赖 candidate-179 的真实路径或编号。

## 当前证据

12 区域回归：

data/runs/20260715_surface_reduction_benchmark_final12b/

candidate-179 49→46 TargetRatio 跨内核回归：

data/runs/20260715_surface_reduction_candidate_0179_hybrid/

candidate-179 MinimizeFaceCount 预算结果：

data/runs/20260721_055601_195__surface-reduction-minimize__03-clay__cand-0179__windows-msvc-debug/

结论摘要见 implementation_status.md；精确参数、指标和产物只以对应 run.json、RUN.md 与实际文件为准。

## 验证命令

~~~powershell
./scripts/build_debug.ps1
./scripts/test.ps1
git diff --check
~~~

真实文件基准必须使用独立 run-id，并记录输入面数、输出面数、结果语义、拒绝原因和 STEP 往返结果。

## 不变量

- STEP 边界和几何是硬事实。
- STL 是软参考。
- 扩大分组从原始 STEP 面重新拟合。
- StrictTopologyGate 和 PatchReplacementRepair 不可绕过。
- Open CASCADE 成功不替代跨内核证据。
- BestFoundWithinBudget 不等于最优。
- 精确实验事实留在 data/runs。
