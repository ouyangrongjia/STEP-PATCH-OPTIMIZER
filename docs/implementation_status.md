# 实现状态

本文是当前实现状态的唯一事实来源，只记录当前能力、限制和最近验证基线。里程碑见 completed_features.md，未完成任务见 TODO.md，精确参数与指标见 data/runs。

## 分支

本工作树分支为 `experiment/whole-mesh-brep`，继承 `experiment/mesh-brep-20260929` 的局部纯网格实验，推进固定 GUI 分区的逐区拟合与全模型装配。该实验通过独立分支归档，主工作树的 STL 工程修改保留，未合并主线。

当前产品主线是原生 Surface Reduction。Geomagic AutoSurface、原生单曲面重拟合和原生局部 AutoSurface 均不是默认路线。

## 给定分区的纯网格后端

历史基线为 candidate-179 局部面片及拼回原模型的整件，要求水密且不能有明显可见褶皱。其原始材料保留在原工作树和 Git 历史中；按用户要求，本工作树现在只保留 `data/runs/whole-partition-brep-v1` 这一运行产物根目录。

`tools/mesh_brep/compact.py` 已实际运行 STL、离散布局、网格 UV、不同基底的共享三维接缝、C2 曲面及 STEP 的完整链路，并完成原 CAD/旧候选/缓存不可读时的相同结果重跑。该原型保留给定五片布局，以较小径向和中央控制网配合自适应边界曲线。后续接口挂接只读取原 CAD 边界，不再修改网格拟合出的支撑曲面；整件已通过有效 Solid、STEP 往返和 Creo 实体导入，并消除了此前 Creo 中的额外拆面和该整件的 GEOM_CHECKS。最大方向预算、原邻面 G1、短边提示及局部曲率峰值仍有未通过或待优化项，保持 `DiagnosticOnly/can_apply=false`。参数与证据见 本轮 B 记录（历史路径：`../data/runs/20260929_mesh_brep/compact_b/RUN.md`），原 A/B 对照与旧接口版按各自冻结协议保留。C++ 产品与 GUI 未改变。

完整模型使用主 GUI 中与 candidate-179 对应的已有分区，已通过原 C++ 分区代码和冻结 FaceId 列表核对。上游 STEP 仅用于分区定位；拟合输入保留原始 STL 坐标、三角形及离散区域标签。当前任务明确要求完全自主重建，且最终面数小于对应参考 STEP，并同时满足可见光顺、边界保持和 Creo 水密实体。新的实验入口为 `skeleton_reconstruct.py`：先构造共用平滑骨架，再逐区拟合和装配；参考 CAD 只在独立计数阶段读取，不向拟合器提供曲面或曲线。`graph_reconstruct.py` 保留先拟合后合并路线，`whole_reconstruct.py` 保留早期原生多项式路线；商业旧入口 `legacy_wrap_reconstruct.py` 只用于历史复现。

共用骨架路线已把 candidate179 的少面布局思想推广到固定分区全模型。当前采用有界最小弯曲公共曲线、锐边分离法向、物理尺度下的支撑面公平项、连续边界和域内鼓包检查；变化的公共曲线使两侧缓存失效。既有小邻域共用支撑修复保留原区域裁剪和三角形归属；本轮另增加按邻接分块的整件联合优化。

上版过渡区实验修正了双三次公平项的欠积分，增加面积加权、公共曲线优化和保留标签的源折角支撑分域。数字 3 周围大块鼓包减轻，整件通过原距离抽样预算，并在安装配置下形成 Creo 闭合活动实体、GEOM_CHECKS 为零。该已验证基线及原失败记录保留在 [native-v4/result_v4](../data/runs/whole-partition-brep-v1/native-v4/result_v4/RUN.md)。

历史 [native-v5/result_v5](../data/runs/whole-partition-brep-v1/native-v5/result_v5/RUN.md) 使用有限源三角形距离管、几何转角与弯曲量守卫、裁剪绕序检查、双链窄域协调修复及局部硬法向重拟合，保留原 359 分区和完整源三角形覆盖。实际 STEP 的牌框长边波浪、192 区已知法向异常和重点接缝极值改善；独立逐面加密采样通过原距离预算，85 项后端回归通过。然而 Creo 虽仍形成闭合活动实体，该候选的 GEOM_CHECKS 为 1，未通过替代验收；原文件和失败证据保留。

前版 [native-v6/result_v6](../data/runs/whole-partition-brep-v1/native-v6/result_v6/RUN.md) 增加公共曲线与交点联合优化、交点回退后的逐边几何检查、相容支撑复用和实际三维单位法向联合优化。保持原分区、源三角形归属及距离预算，移动过交点的缓存禁止经旧骨架入口重建。最终整件、逐面加密采样及安装配置 Creo 检查分别保留，Creo 形成闭合活动实体、GEOM_CHECKS 为零；96 项后端回归通过。凹字重点接缝与变化边链的实测指标改善，但同视角图仍有局部起伏，逐面法向仍有异常，全局距离和法向统计并非全面改善。

曲线网络基线 [native-v7/result_v7](../data/runs/whole-partition-brep-v1/native-v7/result_v7/RUN.md) 采用跨人工面片交点的主曲线重构，同时处理区域间接口和区域内部的光滑切分线；公共边是主曲线的精确子段，交点不再逐个锁在旧三角链上。同区域内吸收细碎拟合域，并删除没有分支意义的二度节点；原始 STL 坐标、三角形和区域标签不变，未执行切网格。移动边界按有限源表面距离管和源链位移分别约束，再重拟合两侧支撑并联合优化实际法向。增加病态 QP 的可行候选验证和同控制点预算下的均匀节点重试。实际曲线图及同视角 STEP 图显示重点牌框波浪线改善；最终独立逐面距离检查通过，Creo 形成闭合活动实体、GEOM_CHECKS 为零，后端回归通过。数字内弧仍有局部起伏，接缝法向极值和部分支线转角有退化；闭环、多分支及全件未覆盖接口尚未全部重构。边界移动预算的调整、独立距离和逐面法向测量、早期 Creo 及距离超限候选均在运行记录中保留。纯 STL 新分区、一般过渡带布局、G1/G2、体积积分差异和其他模型泛化仍未通过，保持 `DiagnosticOnly/can_apply=false`；C++ 产品与 GUI 未改。

基线 [native-v8/result_v8](../data/runs/whole-partition-brep-v1/native-v8/result_v8/RUN.md) 将局部距离超限的检测、候选回退、收紧预算重拟合与新采样复验纳入 `skeleton_reconstruct` 默认流程。新增 `--surface-fit` 接受已完成主曲线网的缓存，核对原 STL、分区及完整覆盖后直接进入闭环，不重新生成移动过的交点。逐面保留异常位置，自动选择接近预算的面，核对实际 STEP 的支撑和裁剪身份，共享支撑组整体修复；次数耗尽或无合格候选时保留失败并停止交付。实件从人工补救前的缓存自动恢复了上一版最终几何系数，共享曲线与原分区不变，新逐面采样通过原距离预算。后端回归通过，具体测量及独立 Creo 证据见运行记录。本轮证明修复流程自动化，未新增视觉光顺、G1/G2 或跨模型成功声明。

前版 [native-v9/result_v9](../data/runs/whole-partition-brep-v1/native-v9/result_v9/RUN.md) 按当轮要求改为基于 v8 原生拟合的整件边界增量优化。`boundary_reconstruct` 检查全部公共边界，`boundary_refine` 在原支撑面上使用受约束弯曲能与跨面片端点切线锥，保持共享交点和曲面系数。新旧边界扫过的曲面窄带、连续落面、法向及形算子退化检查进入同一自动流程；失败直接保留原曲线，不做曲面重拟合和追加细分。装配复用本次逐数组核对一致的连续误差，使用二进制中间件及同进程形体交接，之后进行实际 STEP、区域导出和 Creo 验证。

该增量结果已生成完整模型、同视角图片及跨版本测量，大部分区域的边界指标改善，保留了少数区域指标增加和残余波纹。实际 STEP 通过位置抽样和闭合实体检查；Creo ModelCHECK 进程达到时限，但完整 XML 已写出，报告为 PASS、无几何提示，随后原生体查询确认闭合活动实体。原超时及恢复执行记录保留，完整报告读取已加入自动入口；更新后的异常恢复分支通过相关测试，未另跑一次不中断的全件性能基准。完整耗时目标和全件 G1/G2 尚未通过，保持 DiagnosticOnly/can_apply=false。当前主要时间成本已转到 CAD 交换和独立验收，不能把曲线优化时间写成完整交付时间。相关 10 项测试通过，精确参数、累计执行时间、采样限制和退化项以最终运行目录为准。

此前 `automatic_reconstruct` 冷启动实验的耗时与光顺不合格，不作为 v9 结果；其代码、失败结论和产物保留。旧交付移至 `data/.native-v9-rejected-cold-delivery`，失败摘要见 v9 的 `metrics/prior_attempts.json`。历史 native-v2 至 native-v8 原文件保持。各版沿用简洁的整件、分区件、Creo、metrics、images 结构；v9 重建包包含 v8 原生输入、原 STL/标签、最终曲线与冻结代码，明确区分增量复现和 STL 冷启动。

当前 [native-v10/result_v10](../data/runs/whole-partition-brep-v1/native-v10/result_v10/RUN.md) 在 v9 原生输入上通过 `transition_reconstruct` 统一调整过渡曲面与共享边界。光滑接口使用共同法向和世界坐标形算子，源锐边保留两侧法向；位移同时受毫米上限及源拟合域尺度限制，边界加入源棱边吸引。每张面和每条曲线仅一次有界求解，至多一次半步检查，不新增细分或反复拟合；原生裁剪预检可自动触发一次一环输入回退。复用进程池与源网格查询，将体积积分移至独立质量检查，完整拓扑和 STEP 回读仍保留。整条边交换未通过本件 Creo，最终采用源边对应的精确样条子段。

v10 已从输入不中断地完整重跑，实际 STEP 与安装配置下 Creo 均形成闭合实体，ModelCHECK 无几何提示，独立逐面采样通过原距离预算；最终未触发裁剪回退。后端回归通过，原源三角形归属和支撑基底保持。多数区域的接口及边界指标改善，少数区域退化，源法向尾部仍有异常；同视角图的局部反光起伏减轻，但全件 G1/G2 和一分钟目标未达到，保持 `DiagnosticOnly/can_apply=false`。精确阶段时间、逐区域改善及退化、失败试验、环境和冻结重建包均见该版运行目录。C++ 产品与 GUI 未变。

下段保留此前先拟合后合并路线的实现与历史验收状态：

本轮固定分区高度曲面后端已加入共用法向、分区内部合并、原始顶点身份装配、投影自交检查及原 STL 内腔的包含关系。统一入口可以根据失败裁剪面，或 Creo 再导出的自由边与无效面自动收紧局部拟合；自生成 STEP 先核对完整支撑身份，唯一匹配时只修复对应域，诊断 CAD 不作拟合目标。折角支撑划分补上不能同组约束，防止两侧绕端点重新聚合；过度切分仍会超预算。已知异常可按固定源三角形位置在实际 STEP 中跨版本复核。支持显式共享边界平滑及局部回退。已经在三个实际模型上使用各自产物保存的导入配置取得少于参考面的 Creo 原生实体，包括带内腔模型；安装自带配置复核中，主样例仍无法形成活动实体。可见边缘波纹、Creo 几何检查及交换稳定性仍未全面解决，保持 `DiagnosticOnly`。可选有界二次规划新增按 STL 折角划分的角点法向场，消除分区标签对光滑边目标法向的影响；后续法向滤波仅为未采用的对照。直接原生面距离复核与离散化距离分开保留。C++ 产品与 GUI 未改变；旧输出已按要求清理，保留 [历史对照指标与失败结论](../data/runs/whole-partition-brep-v1/native-v2/result_v2/metrics/previous_experiments.md)。

## 当前几何对照与保留工具

candidate-179 的直接对照恢复为 `aligned_inner_cubic`：5 面、4324 个实际支撑控制点；历史四片继续保留。两者仍按各自原质量门报告，基线不因回退而变为合格替换。后续自适应探索继续开放，当前没有采用新的自适应布局结果。

`tools/candidate179_research/` 保留周期 B-spline 评估、闭合缝有向使用与双 pcurve 检查、独立裁剪域距离核对、实际 STEP 表达统计、五片分阶段计时，以及显式调用的精确支撑合并辅助算子。它们不改变基线拟合策略或产品 Apply 门。失败方案、其余区域输出和缓存已按用户要求清理，精简结论与本次验证见 回退记录（历史路径：`../data/runs/20260928_baseline_retention/RUN.md`）。

## 产品入口

GUI 主流程：

1. 导入 STEP。
2. 检测或锁定特征边。
3. 生成并选择候选区域。
4. 选择 STEP-only 或 STEP 加扫描 STL 软参考。
5. 运行原生重拟合预览。
6. 检查面数、偏差、拒绝原因和质量门。
7. 对通过候选执行 Apply。
8. 导出并执行往返或外部 CAD 验证。

主工具栏提供当前原生流程；旧候选、patch 与 Geomagic 入口保留在高级菜单中，只能显式启用。

## Surface Reduction 能力

### 候选与搜索

已实现：

- 从当前选择和拓扑图建立受保护边约束。
- 按原始 FaceId 集合表示搜索状态。
- 生成组+组、组+单面和单面+单面提案。
- 对候选做确定性去重、排序和预算控制。
- 以有限档案保留可继续扩展或可终验状态。
- 对硬接缝不可行状态尽早拒绝。
- 支持 TargetRatio 和 MinimizeFaceCount 两种产品模式。

TargetRatio 用于兼容锁定比例回归。MinimizeFaceCount 在拟合、终验和墙钟预算内寻找当前最好结果，并分别报告：

- OptimalProven：有效结果达到受保护边图给出的可证明下界。
- BestFoundWithinBudget：存在通过结果，但未证明最优。
- NoValidatedResultFound：预算内没有通过全部门禁的结果。

搜索中出现的低面数状态若未通过最终接缝、拓扑或往返门禁，不计为有效结果。

### 拟合

已实现：

- RestrictedBSplinePatchFitter 受约束 B-spline 拟合。
- STEP B-rep 采样作为默认输入。
- 原始扫描 STL 可作为显式软参考。
- 混合参数化、边界约束、留出验证和重裁剪诊断。
- 支撑曲面包络和参数域守卫。
- 连续参数投影与稠密 SameParameter 检查。

新增或扩大的候选分组始终从原始 STEP 面重新拟合，不从父候选拟合面继续合并。

### 缓存与执行策略

已实现：

- 以原始面集合、拟合配置和算法指纹为键的内容寻址缓存。
- 正结果和带原因的负结果缓存。
- 运行内与持久缓存统计。
- 固定数量工作线程的批量拟合。
- FitSet、Replay、Full 和 RefreshCache 分层执行。
- 分阶段计时、拟合计数和活动分组进度回调。

Replay 只允许重放相同算法指纹和配置的拟合事实。任何缓存模式都不会跳过最终候选装配、StrictTopologyGate 或 STEP 往返门。

### 替换与质量门

最终候选依次接受：

- 边界与接缝检查。
- 距离、法向和保留光顺接缝检查。
- 参数化和支撑曲面包络检查。
- 边界约束重裁剪。
- PatchReplacementRepair 受控修复。
- StrictTopologyGate。
- 全局 BRepCheck。
- STEP 写出和重新读入。
- 适用时的 CommercialCadQualityGate 与外部 CAD 检查。

任一门禁失败时 Apply 不提交，当前 ShapeDocument 与 CommandHistory 保持不变。

## GUI 与后台执行

当前 GUI 已支持：

- STEP 导入、特征边检测、候选生成、预览和 Apply 后台化。
- 长阶段心跳和 Process Status 同步。
- 显示当前面数、目标面数、拟合进度和活动分组。
- 区分 STEP-only 与扫描 STL 软参考；加载 STL 不会隐式改变请求。
- 预览、应用、一键预览并应用。
- 撤销、重做和 STEP 导出。
- 失败报告中保留阶段化拒绝原因。

自动 Apply 只在预览仍对应当前候选且全部质量门通过时执行。选择、模型或参数变化会使旧预览失效。

窗口级自动化覆盖仍不完整；日常交互需按 run_gui.md 的冒烟步骤人工确认。

## 最近锁定验证基线

### 12 区域回归

证据目录：

data/runs/20260715_surface_reduction_benchmark_final12b/

该基线覆盖 12 个动态发现区域，结果为 3 个 TargetAchieved、9 个 NoAcceptableReduction。成功区域均完成参数化、BRepCheck、STEP 回读和商业 CAD 类边界门。它用于证明重构和性能改动没有改变既有接受/拒绝合同。

### candidate-179 比例回归

证据目录：

data/runs/20260715_surface_reduction_candidate_0179_hybrid/

对象为 49 面选区。锁定配置使用 TargetRatio、扫描 STL 软参考、目标比例 0.95 和最多二面提案，最终得到 49→46。三个接受组通过几何、接缝、支撑曲面包络、稠密 SameParameter、B-rep、STEP 往返和 Creo 人工复核。

这是跨内核纠偏后的回归事实，不是最少面数证明，也不是 GUI 默认配置回放。

### candidate-179 最少面预算实验

证据目录：

data/runs/20260721_055601_195__surface-reduction-minimize__03-clay__cand-0179__windows-msvc-debug/

同一 49 面选区在 MinimizeFaceCount、512 次拟合预算和 64 次最终验证预算下返回 49→47、BestFoundWithinBudget。理论图下界为 1，但有效结果未达到该下界，因此不能声称最优。

搜索期间更低面数的未通过状态不计入最终结果。

### 分层缓存与性能基线

candidate-179 已完成 FitSet、Replay、Full 与 RefreshCache 的真实链路验证。缓存运行与无缓存 Full 路径保持相同的已接受分组和 49→46 结果。精确耗时、缓存命中和逐组 JSON 只在对应 data/runs 目录维护，不在本文复制。

## 路线状态

### Surface Reduction

状态：当前产品主线。

SR-0～SR-13 的产品化能力已进入主线，涵盖区域发现、受限拟合、质量门、确定性搜索、缓存、GUI 后台执行和最少面预算模式。阶段摘要见 completed_features.md。

### Geomagic AutoSurface

状态：显式后备。

保留 fitting STL 生成、Geomagic 调用、patch 导入、原始边界约束 Apply、诊断和外部 CAD 路由。默认 GUI 不自动进入该路线。合同见 geomagic_patch_workflow.md。

### 原生单曲面重拟合

状态：冻结实验。

SurfaceRefitter 与 native_surface_fit_probe 保留用于诊断和历史复现，但真实候选未形成可作为当前产品主线的稳定接受链。重新启用条件见 native_surface_refitting_guide.md。

### 原生局部 AutoSurface

状态：冻结实验。

拓扑分析、四边布局和多图参数化实验提供了失败边界，但没有建立稳定产品能力。结论与重启条件见 native_autosurface_guide.md。

### 原生区域级多曲面重拟合

状态：独立实验分支，命令行纵向链已改为曲线网络先行，尚未产品验收。

当前采用的结果基线由用户选定为 `data/runs/20260729_candidate_0179_geomagic_reverse_optimization_visualization/final/` 中的四面片结果，实际曲面位于 `evidence/previous_full_diagnostic_20260729_032156/`。流程为先构造曲线骨架和分片布局，再拟合曲面、裁剪与缝合。该选择基于用户对形状效果的评估；结果仍为 `DiagnosticOnly`、`can_apply=false`，面内褶皱和完整质量门未通过的问题仍未解决。

远程交付入口见四面片结果归档（历史路径：`../data/runs/20260729_candidate_0179_geomagic_reverse_optimization_visualization/README.md`），包含可直接查看的面片 STEP、指标与可视化，以及完整原始证据压缩包。

该汇总包另含尚未生成新曲面的参数布局实验，应与上述四面片几何按运行编号区分。后续单面片自适应减面实验保留为探索证据，不替代当前采用的结果基线；以下实现说明反映当前代码状态。

当前实现先生成显式 `RegionCurveNetwork`，内部界面仍使用唯一三维曲线和共同参数 `t`。每张 patch 先独立拟合 6×6 三次低秩延展支撑面，原始 STEP Edge 和内部曲线投影为一般 pcurve；第二轮起交替更新样本参数与 pcurve，并使用 Cauchy 稳健权重、二阶控制网公平项、法向项和 Jacobian barrier。残差驱动 knot 只在失败 patch/方向局部插入，实际 U/V poles 硬限为 `min(配置上限, 25)`。当独立支撑面未达到构造门时，不再把联合迭代直接置零，而是先执行独立的闭合可行性预求解：外部与内部 Curve-on-Surface / `SameParameter` 使用 `0.001 mm` 硬门，内部曲线相对原区域的 `0.03 mm` 投影门保持独立；预求解只启用最小位移、弱公平和 Jacobian barrier，交替更新一般 pcurve 与控制点，并仅对失败 patch 的主导方向局部插 knot。只有闭合预求解通过后，才允许进入 G1/G2、数据、法向与公平项的四面联合系统。STEP Face 的局部绕序继续通过拓扑方向传播处理。

拟合仍分为双轨：`GeomagicEquivalent` 在恢复规格为 `ResearchPartial` 时明确返回 `UnsupportedFittingSpecification`；`FeaturePreserving` 保持原 STEP Edge、共享 Edge/pcurve、`SameParameter`、G0 和固定质量门不变。报告除正式公平求解和最终来源外，还记录闭合预求解是否进入、是否通过、阶段路由、逐 patch/逐 seam 的 `residual-vs-poles`、最坏 `t`/UV 和 Jacobian barrier 接受比例。探针可显式导入阶段 1 的 `untrimmed_support_surfaces.step` 作为闭合初值，并把文件 SHA-256 写入 `CarryForwardFromStage1Artifact` provenance；若阶段 2A 未形成新的可构造闭合面，最终面片必须明确携带阶段 1 产物，不能冒充新结果。Coons 只能作为初始化或 `BoundaryLockedDiagnostic`，不能取得 `ProductFit`。UV 拓扑、支撑曲面 Jacobian、Face/Wire 方向分别判定；验证器冲突仍以 `ValidatorInconsistent` 阻断产品资格。缓存二进制版本为 17，算法指纹为 `native-regional-refit-v48-stage1-closure-initialization`。

P0 锁定诊断证据位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_212344_628__native-region-refit__03_配件_Clay__cand-0179__target\final\`。旧 `4.96138 mm` seam 距离来自 mesh-vertex 配对污染；共同参数下双侧支撑面最大分离为 `4.36e-14 mm`，Curve-on-Surface 偏差为 `4.80e-14 mm`。7 个原始三角形符号变化经拓扑方向传播后判定为 STEP Face 局部绕序，不是 UV 翻转。4 张裁剪 Face 的 38 次 `SameParameter` 检查无失败，内部自由边和意外交线为 0；固定 `0.001 mm` 诊断缝合与 STEP 回读得到 1 个 Solid、0 条自由边和 0 条多重边。Creo 导入成功，但 Face/Wire 仍有 `BadOrientationOfSubshape` 且 ModelCHECK 失败，因此结果为 `DiagnosticOnly`、`canApply=false`。

阶段 1 当前证据位于 `data/runs/20260730_104812_470__native-region-refit__03_配件_Clay__cand-0179__target/final/`。本次明确记录 `formalFairnessSolveEntered=true`、`internalControlPointsSolved=true`、`coonsDiagnosticFallbackUsed=false` 和 `resultProvenance=IndependentLowRankBoundaryViolationDiagnostic`。四张实际低秩支撑面均有限、一般 pcurve 闭合、正 Jacobian 并满足 25 poles 真门，实际为 9×12、11×10、11×10、9×12，共 436 poles / 68 knots；外部 Curve-on-Surface 最大误差为 `0.160706 mm`，38 次 SameParameter 检查全部失败，最大偏差 `0.160708 mm`。尽管如此，固定 `0.001 mm` 强制诊断缝合和 STEP 回读仍得到 1 个 Solid、0 条自由边和 0 条多重边。这证明边界不合格的实际低秩面可以被当前拓扑链强制缝成水密实体，但不证明曲线落面、SameParameter、Face/Wire 方向或产品质量合格；Face/Wire 仍为 `BadOrientationOfSubshape`，ModelCHECK 失败，故仍为 `DiagnosticOnly`、`canApply=false`。

阶段 2A 当前证据位于 `data/runs/20260730_151521_082__native-region-refit__03_配件_Clay__cand-0179__target/final/`。闭合预求解已进入，G1/G2/法向软约束行均为 0；11 次迭代中局部单方向尝试最大达到 15×16 poles，最低硬残差为 `0.0164339 mm`，仍高于 `0.001 mm`，继续求解后出现 trim 域内 Jacobian 翻转。最坏热区为 patch 1、edge 30、`t=0`、UV=`(1, 0.4002505)`，Curve-on-Surface 偏差 `0.161957 mm`、内部双侧分离 `0.163864 mm`。因此 `closure_feasibility_passed=false`、`formal_fairness_solve_entered=false`，阶段 2B 未执行并路由 `Stage3`。最终四张面以阶段 1 SHA-256 provenance 明确携带，38 次 `SameParameter` 全部失败且有 1 条约 `0.369118 mm` 的意外交线；固定 `0.001 mm` 缝合与 STEP 回读虽得到 1 个 Solid、0 条自由边和 0 条多重边，Creo PRT 也生成，但 ModelCHECK 失败，故仍为 `DiagnosticOnly`、`canApply=false`。

旧 P1 的固定 `TrimmedConvexSupportChart`、全局网格加密，以及旧 P2 在冻结曲线网络上的事后 split 均未恢复。下一入口是阶段 3：根据阶段 2A 闭合热区和 Jacobian 翻转，在曲线网络最终冻结前枚举 K=4 角点、seam 和 U/V 方向；只有全部合理 K=4 变体明确局部不可行时才定向 K=5，选定布局后重新生成曲线网络并回到阶段 2A，硬闭合通过后才进入阶段 2B。P0 只修正验证真值和错误分类，仍是不可覆盖的对照基线。

## 当前限制

candidate-179 独立研究以原外边界硬保持和整件水密作为前置门。基线机制与自由重建见 首轮实证（历史路径：`../data/runs/20260921_candidate179_evidence/RUN.md`），冻结旧曲线的四片闭合对照见 水密优先补充实证（历史路径：`../data/runs/20260921_candidate179_hard_boundary/RUN.md`），上一轮直接对照见 布局与共享接缝实证（历史路径：`../data/runs/20260921_candidate179_designer_layout/RUN.md`）。最新 连续性与节点传播实证（历史路径：`../data/runs/20260921_candidate179_continuity/RUN.md`） 在保持原边和整件闭合的前提下，使用显式外围 XY 映射及同几何起点的节点兼容对照，进一步改善局部法向、节点跳变和参数尺度分布，并减少控制点。最大法向、方向控制点预算和 Creo 几何检查仍未通过；CAD 等几何分段与加密探针将后续重点缩小到复合拼接的实际连续性/表示。原始 CAD 报告及修正前的证据均保留。所有结果仍为 DiagnosticOnly，无产品 Apply 资格，不代表现有 C++ 能力或既有回归基线变化。

- MinimizeFaceCount 是预算搜索；除非达到可证明下界，否则不证明全局最优。
- GUI 默认参数与锁定的 candidate-179 49→46 比例回归不同。
- 单一 Open CASCADE 结果不足以证明跨 CAD 可交付。
- 原始扫描 STL 是软参考；缺失时仍可使用 STEP-only，但两种运行不可混为同一基线。
- 商业 CAD 自动检查依赖本机工具与许可，默认单元测试不会启动外部应用。
- 大型真实候选仍可能受拟合和终验预算限制。
- 项目序列化和完整窗口级自动化不是当前已完成能力。

## 验证入口

默认单元测试：

~~~powershell
./scripts/test.ps1
~~~

完整仓库验证：

~~~powershell
./scripts/verify_spo.ps1
~~~

底层构建与测试：

~~~powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
~~~

正式回归应复用对应脚本和独立 run-id。不得覆盖锁定证据目录，也不得用一次新运行的摘要替换原始 run.json。

## 状态维护规则

- 本文只维护当前快照，不追加日期流水账。
- 精确实验参数和指标写入 data/runs。
- 已完成阶段的短结论写入 completed_features.md。
- 未完成任务与验收条件写入 TODO.md。
- 架构不变量写入 module_design.md。
- 详细开发过程写入 Obsidian，历史差异由 Git 保存。
