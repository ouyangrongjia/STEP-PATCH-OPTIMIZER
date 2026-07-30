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
- 独立 `QualityBalanced` 载体只用于临时分区和局部参数化；载体必须通过连通、可定向、无孔单外环圆盘、零翻转和内存预检，不能成为最终内部共享曲线的几何真值。
- `RegionCurveNetwork` 在支撑面拟合前显式冻结节点、唯一三维曲线边、相邻 patch、期望与实际连续性、构造模式、原边界来源、方向化 patch 闭环和稳定 hash。外边界直接引用或按已有内部采样参数受控切分原 STEP Edge；内部界面先由临时分区提出并投影到原 STEP，删除超过门限的局部反向载体段后，构造端点固定、正则化的 C2 三次 B-spline。每条内部曲线分别检查投影走廊、转角、弯曲能、自交和非节点交叉；三次曲线仍不合格时，只允许用穿过原投影点的一级 B-spline 继续诊断，并永久取消产品资格。
- 载体网格只提出临时 patch 分区。`parameters` 是正权调和载体坐标，只作为低秩支撑面的初始参数；`surfaceParameters` 在每轮“支撑面拟合—样本投影—参数 barrier”中更新，拓扑方向传播后的所有三角形必须保持同号且非退化。拟合样本只能依据原始 `FaceId/UV` 在同面载体三角形内做确定性重心插值；无同面三角形时才记录同面回退，禁止跨面借用。每条 edge-use 的 `RegionPatchEdgePcurve` 保存唯一三维曲线共同参数 `t` 下的严格递增采样和一般二维折线，不再要求成为支撑面的 U/V iso 边。节点投影在同一 patch 内共享，保证 pcurve 闭环；布局、拟合、builder、缓存和诊断消费同一份更新后参数域。
- 原始 STEP 三维边、相邻保留面及 `FaceId` 始终是边界和拓扑真值。

每张 patch 先从独立 6×6 三次张量积支撑面开始，数据行、法向行和二阶控制网公平项组成独立稀疏最小二乘；第二轮起使用 Cauchy 稳健权重，并把外部 STEP Edge 与内部冻结曲线按一般 pcurve 投影约束写回。控制网更新先与非退化仿射基线做最大可构造混合，样本参数更新再做无翻转线搜索；残差驱动 knot 只在失败 patch 和主导参数方向局部插入，U/V 实际 poles 均硬限为 `min(配置上限, 25)`。内部曲线相对原区域的投影误差使用 `0.03 mm` 曲线网络门；外部 Edge 和内部 seam 相对支撑面的 Curve-on-Surface、双侧分离与 `SameParameter` 使用独立的 `0.001 mm` B-Rep 贴面门，二者不得互换。

联合拟合前有独立的 closure-feasibility prepass。它允许显式载入阶段 1 的四张未裁剪低秩支撑面并以 SHA-256 固定 provenance；对外部边建立 `S(pcurve(t)) = Edge(t)`，对内部 seam 建立 `S1(pcurve1(t)) = Edge(t) = S2(pcurve2(t))`。预求解只使用最小位移、弱公平和 Jacobian barrier，交替更新一般 pcurve 与控制点；每轮只对失败 patch 的主导参数方向局部插 knot，并在有限性、正 Jacobian、pcurve 闭环、意外交线和单方向 25 poles 真门下验收。`allConstructible=false` 只触发该预求解，不能被强改为可构造；只有硬位置闭合通过后才进入正式联合公平系统。

正式联合阶段不传播内部 seam 的 knot family。五条内部曲线继续保持唯一三维曲线和共同参数 `t`，G0 保持硬等式约束，G1/G2、数据、法向和公平项再通过 continuation 逐步加入；每轮必须先证明闭合不退化。Coons 只允许形成 `BoundaryLockedDiagnostic`，结果同时报告 closure prepass、阶段路由、`formalFairnessSolveEntered`、`internalControlPointsSolved`、`coonsDiagnosticFallbackUsed` 和 `resultProvenance`，并逐 patch/逐 seam 保存 `residual-vs-poles`、最坏 `t`/UV 与 Jacobian barrier 接受比例。若预求解没有形成新的有限可构造闭合面，发布面必须标记 `CarryForwardFromStage1Artifact`，不能把旧面冒充新结果。最终 `Geom_BSplineSurface` 直接提供逐面 U/V poles、knots、总 poles、最大单边 poles、公平能量、法向变化、曲率振荡、归一化 Jacobian 下界和条件数分位数；非有限、翻转、退化或单边 poles 超限均取消产品资格。

构造顺序是稳定合同：每个曲线节点只创建一个拓扑 Vertex，每条内部曲线只创建一个三维 Edge；builder 把布局保存的严格递增一般 pcurve 作为一级二维 B-spline 附着到该 Edge 的两侧支撑面，不在 B-Rep 阶段重新投影或生成替代 pcurve。完成双侧附着后统一执行 `SameRange` 与 `SameParameter`，再严格按方向化 pcurve 闭环构造 Wire 与 Face。内部 seam 必须沿该唯一三维 Edge 的共同参数 `t` 同时评价 `Edge(t)` 与双侧 `Surface(pcurve(t))`；Curve-on-Surface、相邻支撑面分离、法向和导数比例共享同一取样语义。Face 构造后逐对执行 `BRepAlgoAPI_Section`，除预期共享 Edge 外的交线一律记为 `UnexpectedFaceIntersection`；内部 Edge 必须恰好被两张 Face 使用，replacement quilt 的自由边只能属于原 STEP 外边界。逐边报告必须区分拓扑开口、Curve-on-Surface 偏差、相邻曲面分离、法向/曲率连续性和三档显示三角化裂纹。

四张 replacement Face 以唯一共享 Edge 直接组成单一 `TopoDS_Shell`，并逐 Shell、Face、Wire、Edge 保存具体 `BRepCheck` 状态。缝合且拓扑水密只表示诊断链完成。只要低秩支撑面有限、一般 pcurve 闭合且满足 poles 真门，即使 Jacobian、Curve-on-Surface 或 SameParameter 产品门失败，也以 `IndependentLowRankBoundaryViolationDiagnostic` 保留实际面片并继续 B-Rep 构造、区域替换装配和固定 `0.001 mm` 容差诊断缝合；距离、法向、连续性或产品门失败不会提高容差。布局与曲面质量评价共享拓扑传播后的三角形方向；UV 拓扑、支撑曲面 Jacobian、Face/Wire 方向为独立状态，验证器冲突必须报告 `ValidatorInconsistent`。运行先写暂存目录，完成后只发布单一 `final/`：五个状态文件始终存在，曲线网络、未裁剪支撑面、裁剪 Face 和最终整件 STEP 只在阶段确实完成时出现，`final_sewn.step` 与 `diagnostic_sewn.step` 互斥；若没有缝合几何，则发布 `sewing_attempt.json`。manifest 最后生成并记录除自身外各文件的角色、大小和 SHA-256。`StepFaceLineageReport` 分别核对原件、被替换区、replacement-only STEP 与整件 STEP 的 `ADVANCED_FACE`，所以显示等参线不会再被误报成拓扑面。缓存二进制版本为 17，算法指纹为 `native-regional-refit-v48-stage1-closure-initialization`；正缓存保存投影后完整布局和闭合阶段合同，而不是只保存原候选 hash。只有全部产品门和 Creo 均通过时 `canApply` 才能为 true。

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
