# 已完成里程碑

本文只保留里程碑级完成事实、退役路线摘要和证据入口。当前状态见 implementation_status.md，当前任务见 TODO.md，精确实验数据见 data/runs。

## 固定曲线网拟合的局部距离预算闭环

统一入口已集成逐面检测、共享支撑身份核对、局部回退或有界重拟合，以及重新装配后的新采样验证。保留原距离预算、曲线网和分区归属，修复失败不发布通过产物；支持已完成主曲线网缓存直接验收。真实样例自动恢复既有人工补救后的几何，相关回归和证据见 [native-v8](../data/runs/whole-partition-brep-v1/native-v8/result_v9/RUN.md)。此能力不代表全局可见质量或 G1/G2 已通过。

## 给定分区的 STL→B-Rep 实证后端

已完成网格参数化、共享三维接缝、XYZ 样条拟合及真实 STEP 导出的可复跑实验入口，并以文件读取隔离重跑和闭合夹具验证纯网格链。该里程碑不表示 candidate-179 已通过产品门；A/B 的成功范围、失败与原输入对照见 [双路线报告](../data/runs/20260929_mesh_brep/RUN.md)。

## 自适应布局试验收束与五片对照恢复

已将未成功的自适应布局实现和候选模型退出当前状态，保留五片直接对照、历史四片及独立校验修正、计时和精确合面辅助工具；没有形成合格自适应替换能力。失败结论和保留工具的实测见 [回退记录](../data/runs/20260928_baseline_retention/RUN.md)。

## MVP：STEP 检查与可撤销编辑

已建立桌面应用基础能力：

- STEP 读取、显示和导出。
- 面、边和拓扑索引。
- 模型树、检查面板、参数面板与日志。
- 特征边检测、锁定和解锁。
- 候选区域生成与检查。
- 命令历史、撤销和重做。
- 形状验证和错误报告。

该阶段确定了长期事务边界：修改只能在隔离候选通过验证后提交，失败不能改变当前文档。

证据入口：

- tests/
- src/command/
- src/brep/
- src/gui/

## Geomagic 显式后备基线

已完成可复现的 Geomagic AutoSurface 后备链：

- 从 STEP 采样或显式 STL 输入生成 fitting mesh。
- 通过配置和环境变量调用外部 Geomagic 脚本。
- 解析 patch STEP、日志和状态。
- 区分 patch 预览与 Apply。
- 使用原始 STEP 边界重建替换区域。
- 执行 PatchReplacementRepair、StrictTopologyGate、商业 CAD 类门和 STEP 往返。
- 记录 trim、closure、free-edge 和外部 CAD 路由诊断。
- 支持 Creo 批处理导入与 ModelCHECK 结果归档。

这条路线证明了外部拟合、受控导入和质量门可以组成完整链路，但真实样例也表明“第三方工具成功”不能替代产品质量门。因此它已退居高级菜单中的显式后备，不是默认主线。

当前合同见 geomagic_patch_workflow.md。历史 B 系列阶段和精确指标由 Git、data/runs 与既有 baseline 产物保存。

## 冻结实验：原生单曲面重拟合

NSR 实验完成了：

- SurfaceRefitter 与 native_surface_fit_probe。
- 边界采样、参数化、受限拟合和诊断报告。
- G1、留出误差、多图参数化等方向的真实候选验证。
- 接受/拒绝原因和可复现产物。

实验结论是：把复杂多面区域稳定压成一个自由曲面，不能仅凭局部拟合误差建立可靠产品合同。真实候选仍受到参数化、边界、角部和跨面覆盖限制。

该路线已冻结，代码保留用于诊断和历史复现。边界及重新启用条件见 native_surface_refitting_guide.md。

## 冻结实验：原生局部 AutoSurface

AS 实验完成了局部拓扑分析、四边布局、多图参数化和 B-spline 网络拟合的探索性实现与诊断。

实验未建立在真实候选上稳定通过的端到端接受链，继续扩大局部算法会与当前 Surface Reduction 主线重复投入。因此该路线冻结，不作为产品入口。

结论与重新启用条件见 native_autosurface_guide.md。

## Surface Reduction 里程碑

### SR-0～SR-2：产品合同与区域发现

完成：

- 定义“输出面数减少且全部质量门通过”的成功合同。
- 从 STEP 拓扑和特征边发现候选区域。
- 使用原始 FaceId 集合表示输入和候选分组。
- 建立 STEP-only 与扫描 STL 软参考的输入边界。

### SR-3～SR-5：受限拟合与真实基线

完成：

- RestrictedBSplinePatchFitter。
- 边界约束、参数化、留出验证和重裁剪。
- 几何、接缝、BRepCheck、STEP 回读和商业 CAD 类门。
- 12 区域真实回归基线。

证据：

data/runs/20260715_surface_reduction_benchmark_final12b/

### SR-6～SR-8：搜索与最终候选验证

完成：

- 确定性候选扩展、去重、排序和预算。
- 精确同域合并与受限拟合路径统一。
- 搜索状态与最终候选验证分离。
- 有限档案和阶段化拒绝原因。
- 保持公开入口、报告字段和既有真实回归结果。

### SR-9～SR-10：自适应搜索与可解释报告

完成：

- 对原始 FaceId 分区执行束搜索。
- 使用法向、曲率、共享边长度和空间分散信息排序提案。
- 缓存精确合并和受限拟合结果。
- 报告搜索展开、生成、去重、预算、缓存和阶段拒绝原因。

### SR-11 与 SR-11.1：跨内核纠偏

首次 candidate-179 49→46 结果通过 Open CASCADE 检查，但 Creo 暴露超大支撑面和裁剪裂缝，因此原结果退役。

SR-11.1 增加：

- 调和域外守卫。
- 支撑控制网包络门。
- 连续参数投影分支。
- 稠密 SameParameter 终验。

纠偏后同一 49 面选区重新得到 49→46，并通过 STEP 往返和 Creo 人工复核。该结果成为 TargetRatio 锁定回归，但不构成最少面证明。

证据：

data/runs/20260715_surface_reduction_candidate_0179_hybrid/

### SR-12.1～SR-12.2：性能与内容寻址缓存

完成：

- 分阶段计时和执行策略。
- 选区边预计算、空间索引和参数域索引。
- KKT 符号分解等可复用计算。
- 内容寻址的正负拟合缓存。
- 固定线程批量拟合。
- FitSet、Replay、Full 和 RefreshCache 分层脚本。

真实 candidate-179 回归证明缓存与无缓存路径保持相同接受组和 49→46 结果。精确耗时与缓存统计保留在运行产物中。

### SR-12.3～SR-12.5：GUI 产品化

完成：

- 原生 Surface Reduction 成为主工具栏流程。
- Geomagic 与旧 patch 流程移入高级菜单。
- STEP-only 与扫描 STL 软参考显式区分。
- 导入、特征边检测、候选生成、预览和 Apply 后台化。
- 预览/应用状态机、一键预览并应用。
- 心跳、阶段进度、活动分组和拒绝原因展示。

### SR-13：最少面预算模式

完成：

- MinimizeFaceCount 产品模式。
- 受保护边图理论下界。
- 组+组、组+单面和单面+单面提案。
- 真实三维空间分散种子与局部跳级。
- 拟合、最终验证、墙钟和档案预算。
- OptimalProven、BestFoundWithinBudget 和 NoValidatedResultFound 的独立结果语义。

真实 candidate-179 预算实验返回 49→47、BestFoundWithinBudget。它证明模式能够给出通过全部门禁的预算结果，同时明确没有达到理论下界、没有证明最优。

证据：

data/runs/20260721_055601_195__surface-reduction-minimize__03-clay__cand-0179__windows-msvc-debug/

## 保持不变的工程成果

以下能力跨里程碑保留：

- StrictTopologyGate 是交付硬门禁。
- PatchReplacementRepair 是受控修复边界。
- STEP 边界与拓扑高于 STL 软参考。
- Apply、撤销和重做保持事务性。
- 扩大的拟合分组从原始 STEP 面重新拟合。
- 单一内核成功不替代跨内核证据。
- run.json、RUN.md 和同目录产物是精确实验事实。

## 维护规则

只有形成稳定用户能力、明确冻结结论或锁定回归基线时，才在本文增加一项短摘要。阶段日志、逐次测试输出、完整指标表和未来计划不写入本文。
