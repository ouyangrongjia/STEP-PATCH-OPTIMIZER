# 模块设计

本文只记录稳定架构、模块边界、依赖方向、事务边界和长期不变量。当前实现状态见 implementation_status.md，当前任务见 TODO.md，运行历史与精确指标见 data/runs。

## 设计目标

系统围绕一个受控的 B-rep 修改事务组织：

1. 从 STEP 建立可信的拓扑与几何索引。
2. 在不修改当前文档的前提下分析区域并生成候选。
3. 从原始事实构造替换几何。
4. 在隔离副本上执行修复和验证。
5. 成功时一次性提交，失败时保持原状态。

架构优先保证可追溯、可回滚和跨内核可验证，而不是追求单次构造成功率。

## 分层与依赖方向

依赖从交互层指向应用层，再指向领域模块和基础设施。领域核心不能反向依赖 GUI。

~~~text
app / gui
    |
    v
command
    |
    +--> reduction / merge / patch / feature
    |          |
    |          +--> brep / validate
    |
    +--> io / external
               |
               v
            common
~~~

允许的主要方向：

- app 调度 command、gui 和工作流服务。
- gui 只展示状态、收集输入和发送意图。
- command 组织一次可撤销事务。
- reduction、merge、patch 和 feature 实现领域算法。
- brep 提供文档、索引和拓扑查询。
- validate 只验证候选，不改变产品策略。
- io 负责文件边界，不决定候选是否可接受。
- external 隔离第三方工具协议。
- common 不依赖任何上层模块。

禁止的方向：

- 领域模块直接操纵 Qt 控件。
- 验证模块静默修复输入。
- IO 模块绕过 ShapeDocument 替换当前模型。
- 外部工具结果直接成为当前模型。
- GUI 状态被当作几何事实来源。

## app：应用编排

src/app 负责窗口级生命周期和跨模块工作流。

主要职责：

- 创建并持有应用级服务。
- 把 GUI 意图转换为命令或工作流调用。
- 管理当前选择、预览和进程状态。
- 汇总日志、错误和可展示报告。
- 保证长任务不会把中间结果误提交为当前模型。

AppController 是应用编排入口。MainWindow 负责窗口装配，不承载几何算法。PatchPreviewWorkflow、AppPatchWorkflowState 和 PatchPreviewRunLogger 隔离后备 patch 流程的状态、执行与运行记录。

应用层可以决定何时调用某项能力，但不能降低领域层的质量门。

## gui：交互与可视化

src/gui 负责：

- 三维视图与选择反馈。
- 模型树、检查面板和参数面板。
- 日志、进度和处理状态。
- 将报告字段以可读方式呈现。

GUI 不负责：

- 判定候选是否通过质量门。
- 根据显示网格推断 STEP 拓扑。
- 在控件回调中直接改写 B-rep。
- 维护独立于领域对象的第二份真值。

OccViewWidget 负责 Open CASCADE 视图交互。各面板只读取应用提供的稳定视图模型或报告。

## command：可撤销事务

src/command 将用户动作封装为命令。

命令的基本合同：

- execute 成功后才能进入历史。
- execute 失败时不得改变可观察模型。
- undo 恢复执行前的模型与必要状态。
- redo 重放同一语义，不依赖偶然的 GUI 状态。
- 报告和错误应足以解释失败阶段。

CommandHistory 只管理已经成功提交的命令。LoadStepCommand、ExportStepCommand、PatchReplacementCommand、ReduceSelectedFacesCommand 等分别封装文件或几何事务。

命令不得把预览对象当作已提交文档。若算法需要多阶段中间对象，应在命令私有上下文中持有，直到最终验证通过。

## brep：模型真值与索引

src/brep 是 STEP/B-rep 真值的内存边界。

ShapeDocument 持有当前形状及与其版本绑定的索引。FaceIndex、EdgeIndex 和 TopologyGraph 提供稳定查询，SurfaceTypeProbe 提供支撑曲面分类。

长期约束：

- 索引必须与当前 ShapeDocument 版本一致。
- 形状提交后重建或失效相关索引。
- 不能跨文档版本复用面号或边号。
- STL 三角形编号不能替代 B-rep 实体身份。
- 原始面集合是重新拟合和缓存寻址的基础。

## feature：特征边识别

src/feature 识别影响候选分组的特征边。

FeatureEdgeDetector 可以使用几何、拓扑和用户锁定信息，但输出只是候选生成约束，不是最终质量结论。

特征边约束需要保持可解释：

- 自动识别结果应能追溯阈值与来源。
- 用户锁定边优先于自动合并意图。
- 候选不得跨越已锁定边。
- 阈值变化不得改变原始模型。

## merge：区域分析与冻结的原生拟合

src/merge 包含区域构造、候选筛选、边界分析和历史原生单曲面拟合能力。

稳定职责：

- FeatureBoundedRegionBuilder 构造受特征边约束的连通区域。
- FeatureBoundedCandidateSelector 与 CandidateFilters 过滤不可行候选。
- RegionBoundaryAnalyzer 和 BoundaryWireBuilder 建立边界语义。
- MergePlanner 组织候选，不直接提交模型。
- SameDomainUnifier 只处理满足同域合同的情形。
- SurfaceRefitter 与相关诊断属于冻结实验能力，边界见 native_surface_refitting_guide.md。

merge 不应承载当前 Surface Reduction 的搜索策略；新主线逻辑位于 reduction。

## reduction：Surface Reduction 主线

src/reduction 实现当前原生曲面缩减产品合同。

核心对象：

- SurfaceReductionEngine：候选搜索、拟合、验证和结果选择。
- SurfaceReductionFitCache：原始面集合与拟合配置的内容寻址缓存。
- RestrictedBSplinePatchFitter：受约束 B-spline 支撑曲面拟合。
- StlHybridParameterization：在明确边界内提供混合参数化。

引擎应把以下阶段保持可区分：

1. 候选分组生成。
2. FitSet 评估。
3. 支撑曲面拟合。
4. Replay 或完整候选重建。
5. 修复与质量门。
6. 最终档案选择。

搜索合同：

- TargetRatio 保持兼容的目标比例模式。
- MinimizeFaceCount 在预算内寻找当前最好结果。
- 预算耗尽时必须报告 BestFoundWithinBudget，不能声称已证明最优。
- 搜索档案必须有限，避免无界保留重型 B-rep 对象。
- 放大后的分组从原始 STEP 面重新拟合。

缓存合同：

- 键包含原始面集合、拟合配置和影响结果的版本信息。
- 正负结果均可缓存，但失败原因必须保留。
- RefreshCache 明确绕过旧条目并刷新结果。
- 缓存命中不能跳过与最终候选有关的修复和质量门。

### region_refit：区域级局部多曲面重拟合

`src/reduction/region_refit` 在 Surface Reduction 搜索合同之外提供隔离的区域级重拟合探针，暂不接入 GUI。它包含两个正式选择入口：指定目标面数时按与目标的偏差顺序寻找合格布局；自适应模式在安全上限内生成候选、建立面数/代理误差/参数化畸变/求解内存/输出复杂度 Pareto 前沿，并最多完整拟合三个互不重复的代表候选。只有更小候选均有明确硬拒绝证据时，结果才能声明 `MinimumQualified`，否则只能声明 `BestValidatedWithinBudget`。

几何数据职责保持分离：

- `UniformUv` 等 STEP 参考采样提供拟合点、法向、曲率和原始 `FaceId/UV`；其 STL 只用于形状参考和诊断。
- 独立 `QualityBalanced` 载体只用于分区、内部缝线和局部参数化；载体必须通过连通、可定向、无孔单外环圆盘、零翻转和内存预检。
- 每张 patch 使用 trimmed-disk 局部域：完整边界单调映射到 `[0,1]²` 内的凸边界，再以确定性的 Tutte/调和方法求内部参数。边界可有多于四个转折；不能形成合法圆盘或出现 UV 翻转时直接拒绝，不回退到巨大单面后裁剪。
- 原始 STEP 三维边、相邻保留面及 `FaceId` 始终是边界和拓扑真值。

拟合分为两阶段：先用局部参考样本求有限邻域的张量积 B-spline 支撑面；再以初始控制网为尺度归一化锚点，联合修正原 STEP 外边界和内部缝线。硬约束求解必须使用 `[A C^T; C 0][x λ]=[b d]`，三个坐标轴都写入真实边界目标 `d`；内部缝线使用配对参数点的硬 G0 等式，平滑外边界和适用缝线只增加法向或导数软约束。

构造顺序是稳定合同：先完成局部曲面和任意闭合 UV trim loop，再复用外边界三维 Edge、为每条内部缝线生成唯一共享三维 Curve、附着双侧 pcurve，随后执行 `SameRange`、数值 `SameParameter`、wire/face 构造及至少 97 点逐边稠密检查。逐边报告必须区分拓扑开口、Curve-on-Surface 偏差、相邻曲面分离和三档显示三角化裂纹。

缝合且拓扑水密只表示诊断链完成。`forceDiagnosticSewing` 只允许有限、无翻转且可构造的单个候选越过产品质量门继续装配、缝合、修复和 STEP 往返诊断；它不提高容差，也不改变 `BestEffortThroughSewing` 或产品选择语义。距离、法向、外边界、内部接缝、Curve-on-Surface、输出复杂度或 STEP 往返任一产品门失败时，只能保留 `sewn_attempt.step`，并强制不可 Apply。

## patch：边界约束替换

src/patch 将候选支撑曲面重裁剪为受原始边界约束的替换区域。

主要职责：

- 定位并导入候选产物。
- 检查单面或多面 patch 结构。
- 依据原始边界构造或重裁剪替换面。
- 装配替换形状并执行受控修复。
- 输出闭合、裁剪和修复诊断。

PatchReplacementAssembler 负责替换装配。PatchReplacementRepair 负责装配后的受控修复。两者均不得通过任意放宽容差隐藏几何不一致。

边界约束是硬合同：

- 原始 STEP 边界优先于导入 patch 的自由边。
- 导入 patch 只提供支撑几何或明确的多面结构。
- Apply 必须检查边界配对、方向和裁剪有效性。
- 修复后的形状仍须通过 StrictTopologyGate。

## validate：质量门

src/validate 提供分层验证。

ShapeValidator 提供基础形状检查。StrictTopologyGate 是交付链硬门禁，至少覆盖适用的壳体、边使用、自由边、退化边和拓扑一致性。CommercialCadQualityGate 表达需要外部 CAD 或商业内核证据的边界。

验证原则：

- 构造成功不是验证成功。
- 修复成功不是拓扑门成功。
- Open CASCADE 成功不是跨内核成功。
- STEP 写出成功不是往返成功。
- 视觉无裂缝不是数值无裂缝。

适用时还应检查：

- 边界距离和法向连续性。
- seam 与 SameParameter。
- 支撑曲面参数域和控制网包络。
- STEP 导出后重新导入。
- Creo 或其他商业 CAD 打开、再生和检查。

## stl：软参考与拟合输入

src/stl 负责 STL 读取后的区域索引、裁剪、切链和拟合网格构造。

STL 的允许用途：

- 构造拟合采样。
- 提供粗糙形状参考。
- 支持偏差检查和可视化。
- 为显式后备工作流生成输入。

STL 的禁止用途：

- 覆盖 STEP 边界。
- 独立决定拓扑连通性。
- 证明壳体闭合。
- 把缓存裁剪结果误称为原始扫描。

StpSampledFittingMeshBuilder 生成的网格来源于 STEP 采样，应与外部原始 STL 明确区分。

## io：文件边界

src/io 负责 STEP 和 STL 的读取、写出及错误报告。

读取合同：

- 解析失败不得替换当前 ShapeDocument。
- 返回值应区分文件、格式和几何失败。
- 成功后由上层建立文档与索引。

写出合同：

- 只接收已经选择的明确形状。
- 写出失败不得改变当前模型。
- 正式候选写出后仍需往返验证。
- 运行产物路径由运行上下文统一记录。

## external/geomagic：第三方协议隔离

src/external/geomagic 只封装 Geomagic AutoSurface 的配置、输入输出路径和结果协议。

该模块不能：

- 自动把 Geomagic 设为默认路线。
- 把进程退出成功等同于 patch 合格。
- 绕过导入、边界重建和质量门。
- 把第三方临时文件当作仓库真值。

完整后备流程见 geomagic_patch_workflow.md。

## common：基础类型

src/common 提供 Result、Tolerance、配置、日志和通用几何类型。

基础模块应保持小而稳定：

- 不包含产品阶段状态。
- 不依赖 GUI、命令或外部工具。
- 容差必须有单位和语义，不能用无名常量传播。
- Result 应保留足够错误上下文，不以日志替代返回合同。

## 事务边界

所有会改变当前模型的路径遵循相同事务：

~~~text
当前文档
  -> 隔离候选
  -> 重建与修复
  -> 拓扑和几何验证
  -> STEP 往返或外部证据
  -> 原子提交
~~~

任一阶段失败：

- 当前文档保持不变。
- 不进入 CommandHistory。
- 预览和失败产物可保留用于诊断。
- 报告指出失败阶段和证据位置。

## 长期不变量

以下约束跨路线、跨阶段保持不变：

1. STEP 边界和拓扑优先于 STL。
2. 当前模型只有一个事实来源。
3. Apply、撤销和重做是原子事务。
4. StrictTopologyGate 与 PatchReplacementRepair 不得被旁路删除。
5. 候选分组扩大后必须从原始面重新拟合。
6. 最少面搜索必须区分最好已知结果与最优证明。
7. 缓存命中不改变质量门。
8. 实验参数和精确指标写入 data/runs，不复制到架构文档。
9. 冻结路线不会因代码仍存在而自动恢复为产品主线。
10. 外部工具和商业 CAD 证据必须可追溯到同一候选。

## 文档边界

- 本文只描述稳定结构和约束。
- implementation_status.md 描述当前能力和基线。
- TODO.md 描述未完成任务。
- completed_features.md 描述里程碑。
- project_structure.md 描述当前目录。
- 各路线指南描述对应产品或冻结实验合同。

架构变化时只更新受影响的稳定职责，不在本文追加开发日志。
