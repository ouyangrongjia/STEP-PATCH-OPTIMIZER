# 实现状态与待办清单

> 文档定位：本文件用于记录当前工程实现进度、已完成功能、部分完成模块、待办事项与验证方式。  
> 长期模块规划放在 `module_design.md`，本文件随开发进度持续更新。

---

## 1. 当前版本概览

### 2026-07-16 结构优化阶段 1

当前开发分支为 `refactor/structure-hardening`，基线为 `native-surface-reduction` 的 `dc76425`。阶段 1 已删除无引用空占位类型、空编译单元和无功能的 `batch_optimize` 目标；工具层共用 JSON/UTF-8 路径辅助与确定性 FeatureBoundedRefit 候选选择已集中，Geomagic 配置和结果 JSON 写入共用同一文件辅助实现。算法、报告字段、CLI 和 GUI 行为保持不变。

### 2026-07-16 结构优化阶段 2

阶段 2 已增加 FeatureBoundedRefit 候选查找、过滤、排序和未设置统计字段回退的具名测试；`PatchReplacementCommand` 的 repair、closure、trim、external CAD 与 gate 报告断言已按合同分组；`StrictTopologyGate` 增加“只从补片报告读取 replacement face count”的解耦前基线。默认测试不包含真实 candidate 或固定 FaceId。

### 2026-07-16 结构优化阶段 3

阶段 3 已提取共享 `PatchReplacementRepairMetrics`，repair report 与最终 replacement report 通过同一基类保留原字段访问和输出合同，命令侧不再逐字段复制修复指标。`StrictTopologyGateInput` 改为直接接收 `replacementFaceCount`，验证模块不再包含或前置声明补片报告。`BoundaryWireBuilder` 从 B-rep 目录移动到 merge 目录，消除底层拓扑模块对区域分析类型的反向依赖；类名、方法和行为不变。

### 2026-07-16 结构优化阶段 4

阶段 4 已从 `PatchReplacementCommand` 提取 `PatchReplacementAssembler` 和 `PatchClosureDiagnostics`：前者负责原面移除、补片面重建与 replacement shape 组装，后者负责修复前后闭合统计及自由边诊断。命令类只保留输入校验、执行顺序、repair、质量门和报告汇总；公开接口、失败顺序、报告字段、Apply、undo/redo 语义保持不变。

### 2026-07-16 结构优化阶段 5

阶段 5 已将 `StpSampledFittingMeshBuilder::build` 整理为基础面采样、candidate surface over-cover、adjacent-face support collar、角点采样诊断和最终网格验证五个内部阶段。稳定入口、over-cover 先于 support collar 的追加顺序、基础三角形快照、默认参数、三角形方向、警告顺序与报告字段映射保持不变。`verify_spo.ps1 -RealGeomagic` 已分别在本阶段代码和 `c2d9477` 对照基线上运行：两者都因默认 candidate 29 的 Geomagic 补片含 45 个 B-rep 面而在既有高风险预览门退出；两次 fitting STL 均为 480084 字节且 SHA-256 同为 `E060E6E883D8B17D92F574711F305CF51D14CF9423F6CE6CD3EECCE2AAA75B4A`，三角形数 9600、边界样本数 180、内部样本数 5292 完全一致，因此该非零退出不是本次结构拆分引入的回归。

### 2026-07-16 结构优化阶段 6

阶段 6 已增加非 `QObject` 的 `PatchPreviewWorkflow`，承接 STL crop、STP sampled fitting 与 Geomagic 调用，`AppController` 的公开静态入口保留并只做委托；补片 artifact/import/preview/apply/process 状态集中到 `AppPatchWorkflowState`。主窗口候选、动作忙碌标记、进度和 fitting mode 集中到 `MainWindowWorkflowState`，进度报告与状态栏文本由纯 `PatchWorkflowText` 格式化。既有信号槽、按钮刷新方法、进度回调顺序和候选清理入口未改名；新增结构测试固定清理默认值和进度文本字段顺序。GUI 可执行文件已通过启动后持续存活 5 秒的进程级冒烟；由于当前会话没有 Windows 交互执行工具，打开、预览、Apply、undo、redo 和导出的人工点击复核未自动执行，保留为提交后的人工检查项。

### 2026-07-16 结构优化阶段 7

阶段 7 已将 `RestrictedBSplinePatchFitter::fit` 整理为数据准备、模型求解和结果验证三个文件内阶段，公开入口仍负责输入预检、阶段编排和既有异常映射。采样与参数化顺序、边界约束细化、KKT 求解、UV 回溯、模型选择、拒绝顺序、容差和报告字段保持不变。全量单元测试耗时 113.43 秒；12 选区真实基准为 3 个 `TargetAchieved`、9 个 `NoAcceptableReduction`，选区、输入输出面数、结果类型、距离、法向、接缝、BRepCheck 和 STEP 回读字段与 `20260715_surface_reduction_benchmark_final12b` 逐项一致。

### 2026-07-16 结构优化阶段 8

阶段 8 已从 `SurfaceReductionEngine::reduce` 提取文件内的确定性减面搜索和最终候选质量验证：公开入口保留输入预检、边界分类、阶段调用及结果报告汇总；搜索阶段独占状态扩展、精确合并与拟合缓存、预算、去重和束排序，终验阶段依次执行接缝、Commercial CAD、全局 B-rep 与 STEP 回读质量门。公开接口、状态签名、缓存键、扩展与拒绝顺序、预算和报告字段保持不变。全量单元测试耗时 106.43 秒；阶段 7 与阶段 8 的 12 选区 CSV 全部 19 个字段一致，并继续得到 3 个 `TargetAchieved`、9 个 `NoAcceptableReduction`，计划验收字段与 `20260715_surface_reduction_benchmark_final12b` 逐项一致。

### 2026-07-16 任意选区自适应曲面减面

当前开发分支切换为 `native-surface-reduction`，基点为 `native-surface-refit` 的 `1076082`。`native-autosurface` 保留三项后续实验提交但不合并；AS-3 原全局三角网络路线取消。

| 阶段 | 状态 | 当前实现 |
|---|---|---|
| SR-0 | 已完成 | 新分支建立；NSR 和 AS 路线冻结并文档化 |
| SR-1 | 已完成第一版 | 采样只使用各原始面的局部 UV 三角细分分配点位，点、法向均由原始 `Geom_Surface` 精确求值；训练、模型选择、终验空间互斥 |
| SR-2 | 已完成第一版 | 原始 B-rep 邻接、锁边/选区边界/大于 `15°` 明显折角保护、确定性成对队列、动态目标面数、Eigen `SparseQR` 三次非有理 B-spline 求解、UV 更新、节点插入和数据支持的控制点上限 |
| SR-3 | 已完成第一版 | 原始边界重新裁剪、参数曲线重建、保留共享三维边；精确投影参数曲线失败时只接受连续投影分支后备，并用 97 点 `SameParameter` 复核；保守 repair、`StrictTopologyGate` 和 STEP 回读失败时整次不产生可 Apply 结果 |
| SR-4 | 已完成第一版 | GUI 使用 `selectedFaceIds()`，后台预览、结构化报告、独立 Apply 动作，以及缓存式撤销/重做 |
| SR-5 | 已完成第一版 | 合成测试覆盖 3、7、20 动态输入、人工切分 B-spline、孔洞、锁边、尖锐边、部分减少和命令事务；属性发现基准在四个真实 STEP 上得到 12 个选区，其中 3 个由 2 面减为 1 面，9 个明确拒绝 |
| SR-6 | 已完成第一版 | STEP 修剪面域裁剪原 STL，单图调和参数化只提供参考，最终拟合 UV 被局部 PCA 覆盖且每个节点布局只求解一次；STL 以弱位置项参与，原 STEP 边界在拟合阶段为软约束，重裁剪和独立终验按 `0.03 mm` 硬拒绝 |
| SR-7 | 已完成 | STEP 参数域成为唯一 UV 真值；原始边界通过 KKT 等式约束固定，并以每边 97 个独立样本和最多 8 次最差点细化执行边界质量门；调和图外区域使用低权重守卫，支撑控制网超出原始选区包络时拒绝 |
| SR-8 | 已完成 | STEP-only 和扫描参考路径统一执行最多 5 轮内部 UV 迭代、8 次回溯；边界 UV 不移动，接受步必须降低目标并保持 STEP 域和 Jacobian 有效 |
| SR-9 | 已完成 | 以原始 FaceId 分区为状态执行确定性束搜索：束宽 8、每状态最多输出 16 个展开、最多 512 个唯一受限拟合；法向/曲率/共享边长度聚合名次与空间分箱避免局部贪心，硬接缝可行状态优先，精确同域和受限拟合结果按原始 FaceId 集合缓存 |
| SR-10 | 已完成 | 报告搜索展开、生成、去重、缓存、预算、参数来源、扫描覆盖、边界约束、UV 迭代以及 Parameterization / Boundary / Holdout / Retrim / Topology 等阶段化拒绝原因 |
| SR-11 | 已完成并由 SR-11.1 纠偏 | candidate 179 首轮得到 `49→46`，但 Creo 发现 OCCT BRepCheck 与 STEP 回读未覆盖的超大底层支撑面和裁剪裂缝，原产物不再作为跨 CAD 合格结果 |
| SR-11.1 | 已完成 | 增加调和域外守卫、支撑控制网包络门、连续参数投影分支和 97 点 `SameParameter` 终验；candidate 179 重新得到水密 `49→46`，并单独导出 46 面选区供 Creo 观察 |
| SR-12.1 | 已完成 | 共享核心新增执行策略、分阶段计时、选区边预计算、整件 STL 三维空间索引、参数图 BVH/UV 索引、边界终验点复用、KKT 符号分解复用、内容寻址正负拟合缓存、固定线程批量拟合和运行内接缝角缓存；probe 输出逐组 JSON，分层脚本提供 `FitSet` / `Replay` / `Full` / `RefreshCache` |
| SR-12.2 | 已完成 | candidate 179 六组 FitSet 通过；渐进提案二面阶段通过精确分组基线并成为该回归脚本默认。`RefreshCache` 写入 170 项；`Replay` 170 命中、0 缺失、0 实际拟合，端到端 277 秒；禁用磁盘缓存的 `Full` 端到端 1632.8 秒。两者均保持 `49→46` 和三个锁定接受组，几何与拓扑门全部通过 |
| SR-12.3 | 已完成 | GUI 顶层菜单和主工具栏切换为原生 B-rep 曲面减面主流程；拟合输入显式区分 STEP-only 与 STEP 加扫描 STL 软参考，默认 STEP-only，加载 STL 不再隐式改变请求；后台预览固定使用 4 个拟合工作线程并显示总耗时。旧候选、Patch 与 Geomagic 流程只保留在“高级”菜单作显式后备 |
| SR-12.4 | 已完成 | GUI 主线纠正为“导入 STEP → 检测特征边 → 生成并选择候选区域 → 原生重拟合 → 显示/应用效果”。候选选择模式不再依赖会被清空的普通 face 选择，而是把当前 `MergeCandidate::faces` 传入 `SurfaceReductionEngine`；主工具栏使用一个状态化“原生重拟合”按钮承接预览/应用，并增加“一键预览并应用”。自动应用仍以当前候选匹配和全部质量门通过为前提 |
| SR-12.5 | 已完成 | GUI 当前主线的 STEP 导入、特征边检测、候选生成、原生重拟合预览和 Apply 全部后台化并复用 2 秒心跳；`SurfaceReductionEngine` 通过执行期回调报告拓扑准备、束搜索层、唯一分组拟合计数、接缝候选终验、商业 CAD 类门、整件组装、`StrictTopologyGate` 和 STEP 回读，报告页与 Process Status 同步显示当前面数、目标面数、拟合进度和活动分组 |

产品成功门：输出面数小于输入且所有质量门通过即成功；输出不大于 `floor(N/2)` 时标记达到目标。任何距离、法向、接缝、参数化、支撑曲面包络、稠密 `SameParameter`、B-rep 或 STEP 回读门失败都不能 Apply。

SR-5 最终 12 区域报告位于 `data/runs/20260715_surface_reduction_benchmark_final12b/`。区域由平滑内部边和现有候选属性发现，不使用固定 candidate ordinal 或 FaceId；三个 2 面减为 1 面的成功结果均写出 STEP，且参数化、BRepCheck、STEP 回读和商业 CAD 类边界门全部通过。

`03_配件_Clay.stp` 的 candidate 179 已完成 SR-11.1 跨内核纠偏。选区动态发现为 FaceId 1702～1750，共 49 面，目标比例 `0.95`。首轮产物虽通过 OCCT BRepCheck 和 STEP 回读，但 Creo 显示超大面片与裂缝；诊断确认新支撑面控制网达到 `39.5225 mm`，边面参数曲线偏差达到 `0.00391089 mm`。修复后混合路径使用 290 个唯一受限拟合，接受 `1727+1732`、`1742+1745`、`1746+1747`，得到 `49→46`、`TargetAchieved`；最大距离 `0.00134683 mm`、最大法向差 `1.77073°`、最大保留光顺接缝角 `2.93862°`。最终 STEP 为 2736 面、1 壳、1 实体，曲线在曲面上的最大偏差 `0.000942836 mm`，无超过 `0.001 mm` 的关联；Creo 手动复核为无超大面片、无可见裂缝的灰色水密实体。最终接受组使用 144 个扫描参考样本；唯一结果目录为 `data/runs/20260715_surface_reduction_candidate_0179_hybrid/`。

GUI 默认参数与上述锁定回归并不相同。GUI 默认使用 `StepBrepOnly`、`targetOutputRatio=0.5`、`maximumProposalGroupSize=0`、禁用持久拟合缓存和 4 个工作线程；candidate 179 的 `49→46` 锁定回归则使用原始扫描 STL 软参考、`targetOutputRatio=0.95`、最多二面提案和分层脚本缓存策略。两者复用同一个 `SurfaceReductionEngine` 和同一套最终质量门，但不能把 GUI 默认运行视为 `49→46` 回放。

以下 NSR 状态为保留的实验基线历史。

当前项目已形成 MVP 工程闭环：

```text
STEP 读取
→ B-rep 拓扑索引构建
→ OCCT Viewer 显示
→ face/edge 选择与多选
→ 特征边检测
→ 用户锁边/解锁
→ same-domain 合并
→ undo/redo
→ 基础合法性检查
→ STEP 导出
```

当前开发分支为 `native-surface-refit`。该分支独立于 `feature-bounded-refit` 的 Geomagic 稳定基线，NSR-1.3 的 0 / 1 / 4 / 8 局部 G1 有限周期已完成，但尚无可接受 patch，当前重点已经转向多候选复核证据：

```text
1. NSR-1 内的局部切平面约束证据已完成 candidate 179 单例实验。
   - 原始有序边界作为 G0 约束。
   - 原始 STEP 候选面内部点作为拟合约束。
   - 训练样本与独立验证样本分离。
   - 第一版继续使用 OCCT BRepFill_Filling，不自研通用 B-spline 求解器。
   - NSR-1 / NSR-1.1 工程探针、全部失败门、样本级诊断和固定三档证据已经完成。
   - NSR-1.2 已补齐 BRepCheck 子形状状态、方向反转前后检查、STEP roundtrip 对照、每张源面覆盖和法向样本级诊断。
   - 固定 96 点分层面积加权消除了内存态 BRep 失败和零训练源面，但 interior 8 / 192、normal 1 / 192 仍超限。
   - 局部 G1=4/8 可把独立 holdout interior 超限数从 8 降到 2，但 normal 仍超限，并重新引入内存态 UnorientableShape / SelfIntersectingWire。
   - 下一步只在至少三个受支持真实候选上复核同类失败；NSR-2、NSR-1.4、NSR-4 均未启动。
2. 当前只支持单外轮廓、无孔、连通 FeatureBoundedRefit 候选区域。
3. 当前只输出一张拟合面、STEP patch 和 JSON 报告，不改 GUI，不做多曲面布局。
4. 原生结果通过自身误差和曲面有效性检查后，再复用现有 Apply / repair / StrictTopologyGate / STEP roundtrip。
5. Geomagic patch generation / import / preview / Apply 保留为后备和对照路线，不再是本分支默认拟合入口。
6. `feature-bounded-refit` 保留 Geomagic 稳定基线；原生路线未通过 NSR-1 / NSR-2 前不回合。
```

完整方案和失败分流见 `docs/native_surface_refitting_guide.md`。

### 2026-07-10 测试与真实实验链路去重

本轮只调整测试编排和重复验证，不改变几何算法、单位对齐或 Creo 验收边界：

```text
1. corner_baseline_probe 新增 --generate-only：完成 STP sampled fitting、Geomagic、补片导入和 preview 后停止，不再提前执行一次质量门和 PatchReplacementCommand。
2. corner_baseline_probe 新增 --apply-only：复用已有补片时仍读取源 STEP、发现候选并执行质量门和 Apply，但不再重建 fitting STL、over-cover 诊断文件或补片副本。
3. run_boundary_trim_fill_experiments.ps1：
   - Route 2 生成阶段使用 --generate-only；归一化后只执行一次 --apply-only。
   - Route 1 已有补片的 Apply 同样使用 --apply-only。
   - step_unit_normalizer 及 --target-step/--source-step/--candidate-id 坐标对齐输入完整保留。
   - Creo Distributed Batch + ModelCHECK 和两条 Creo Toolkit sewing 对照路线完整保留。
4. 独立 StrictTopologyGate 已经包含 STEP 读取、ShapeStats 和 BRepCheck；实验脚本直接复用其 after_stats，不再用 step_stats.exe 二次读取同一大 STEP。兼容报告中的 step_stats.bbox_valid 明确为 null，其余统计来自 StrictTopologyGate。
5. spo_tests 默认只运行不依赖仓库真实大文件的单元测试；真实文件和真实 Geomagic 分别改为显式 spo_real_file_tests、spo_real_geomagic_tests 目标。
6. verify_spo.ps1 -RealGeomagic 只运行一次完整 scripted baseline，不再先在 spo_tests 内重复调用 Geomagic。
```

本机验证中，默认 `spo_tests` 从此前约 93.58 秒降至 11.45 秒；真实文件和外部程序测试未删除，只是不再混入日常默认测试。

### 2026-07-10 测试与实验运行产物目录统一

本轮只调整运行产物的命名、归档和查询，不改变几何算法、Geomagic staging、Apply、repair 或 Gate 语义：

```text
1. 新增 scripts/run_artifacts.ps1：
   - 默认 run id：<yyyyMMdd_HHmmss_fff>__<workflow>__<model>__cand-<id>__<variant>。
   - 数字 candidate 统一四位补零；支持 cand-auto / cand-na。
   - 每轮创建 run.json、RUN.md、artifacts/、diagnostics/、logs/。
   - run.json 记录 Git commit/branch/dirty、源文件 SHA-256、入口参数、阶段状态和关键产物。
2. 新增 scripts/list_runs.ps1：按 workflow、model、candidate、status 和 Latest 查询 data/runs。
3. scripts/test.ps1 每次运行生成独立 unit-tests run，并保存 build.log / ctest.log。
4. run_corner_baseline_gate.ps1、run_boundary_trim_fill_experiments.ps1、run_creo_step_diagnostic.ps1、run_creo_toolkit_baseline_probe.ps1、run_creo_toolkit_phase1_repair.ps1 已接入统一 run context。
5. corner_baseline_probe 的 run-local 产物进入 artifacts/，over-cover 诊断进入 diagnostics/candidate_surface_over_cover/，applied STEP 进入 artifacts/。
6. Route 2 紧凑产物进入 artifacts/，Geomagic 日志进入 logs/，over-cover 诊断进入 diagnostics/；结果 JSON 中既有 route2_artifacts 字段保持兼容。
7. data/crop_stl / crop_stp / crop_igs 继续作为共享 staging，不作为历史验证来源。
8. 旧 data/baseline_runs 保持只读历史，不批量迁移或删除；新默认运行写入 data/runs。
```

验收：

```powershell
.\scripts\test.ps1
.\scripts\list_runs.ps1 -Workflow unit-tests -Status passed -Latest 3
```

自动测试覆盖统一目录/清单静态契约，以及 Creo ModelCHECK parse-only 对 `run.json`、`RUN.md` 和标准子目录的动态落盘。

### 1.0 2026-06-26 Route 2 candidate/source-face parallel over-cover 落地

本轮按 2026-06-25 Route 2 失败截图和指标调整扩宽主路线：

```text
原主路线：adjacent-face support collar 主导扩宽
新主路线：Candidate / Source Face Parallel Over-Cover 主导扩宽
```

已落地：

```text
1. StpSampledFittingOptions / Report 新增 candidateSurfaceOverCover* 参数和报告字段。
2. StpSampledFittingMeshBuilder 沿原候选 STP face 边界生成 2~3 圈平行 over-cover，并桥接回主体 STL。
3. GUI 默认开关改为“启用 STP 候选面外扩带”，默认 width=0.25、rings=3、samples=64、corner miter max scale=1.25。
4. corner_baseline_probe 支持 --candidate-surface-over-cover、--candidate-over-cover-width、--candidate-over-cover-rings 等参数，并输出 candidate_surface_over_cover_* JSON 字段。
5. run_boundary_trim_fill_experiments.ps1 Route 2 默认启用 candidate over-cover；adjacent-face support collar 降级为显式 A/B 辅助项，只有 `-EnableAuxiliarySupportCollar` 时才叠加，默认不启用。
6. run_corner_baseline_gate.ps1 的 B2 / B2.2 改为 candidate over-cover；B2.3 为 candidate over-cover + corner-safe auxiliary support collar。
7. GUI Apply 路径显式启用 strict original-boundary re-trim。
8. 显式 strict retrim 的 replacement face 使用 face-compound assembly 后再进入 repair / sewing，避免新重裁边直接替换旧 solid face 后产生 BRepCheck 失败。
```

已验证：

```text
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
```

真实样例已跑（保守 tangent-plane 修复前的 candidate over-cover 结果）：

```text
output: data/baseline_runs/route2_candidate_overcover_real_20260626_1632
status: AllRoutesFailed

STP sampled fitting 输入已恢复为干净单组件：
  Geomagic pre-repair components=1
  boundaryCycles=1
  nonManifoldEdges=0
  nonManifoldVertices=0
  degenerateTriangles=2
  candidate_surface_over_cover coverage=1
  rejected=0
  fallback=0
  triangle_count=12168

Geomagic 成功输出 patch：
  patch faces=13
  patch edges=57
  preview high_risk=false
  raw patch unit=meter
  normalized patch unit=millimeter

Apply 仍失败：
  failure_reason=BuildFailed
  message=Multi-surface replacement face edges produced open wires. Fallback wire-connect tolerance was also insufficient.
  built_faces=4
  closed_wires=6
  open_wires=1
  failed_patch_face_index=3
  failed original boundary edge ids=1591
  endpoint_gap=0.6898656017706906
  pcurve rebuild failure=0
  SameParameter failure=0

CommercialCadLikeQualityGate 仍失败：
  boundary max/p95=0.0606937588/0.0338442211
  corner max=0.0392015215
  seam max_abs=0.0435434508

Creo sewing 仍失败最终验收：
  exported STEP solids=0
  shells=2
  BRepCheck=false
  StrictTopologyGate failure=BRepCheckFailed
  after_free_edges=45
  ModelCHECK diagnostic_passed=false
  GEOM_CHECKS=1
  SHORT_EDGES=1497
```

判读：

```text
本轮修正已经把 Route 2 失败点从“输入 STL 多组件 / Geomagic 初始化失败”推进回 Apply / multi-surface open-wire closure。
在该轮 Geomagic 基线中，candidate over-cover 方向和连通性已不再是 blocker；当时的下一步是定位 failed_patch_face_index=3 与 original boundary edge 1591 的 segment / internal seam closure。
```

### 1.1 2026-06-26 candidate/source-face over-cover 保守稳定修复

本轮针对 2026-06-26 截图中出现的局部下翻、侧壁方向生长和角点尖刺做保守修复，不改 OCCT 曲面重建、不恢复旧 guard-band / old over-cover strip。

已落地：

```text
1. reverseEdge 遍历时，pcurve D1 tangent 会先反向，再参与 left/right 外侧判断。
2. over-cover 点优先用 source face 切平面 3D offset：P_offset = P + D * distance。
3. surface UV 外推只作为 normal leakage 诊断参考；leakage > 0.20 计入 fallback。
4. corner miter 默认 max scale 从 2.0 降到 1.25；相邻 offset 方向突变或近似反向时只 clamp，不做 miter 放大。
5. candidate over-cover bridge / quad strip 增加长三角保护，按 boundary ring 平均采样间距设置局部阈值。
6. StpSampledFittingReport / corner_baseline_probe JSON 新增：
   candidate_surface_over_cover_normal_leakage_max
   candidate_surface_over_cover_direction_fallback_count
   candidate_surface_over_cover_long_triangle_count
   candidate_surface_over_cover_max_triangle_edge_length
   candidate_surface_over_cover_source_face_count
   candidate_surface_over_cover_direction_flip_count
```

已验证：

```text
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
.\scripts\build_debug.ps1
```

真实样例复测：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
output: data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810
status: AllRoutesFailed

fitting STL 本体质量：
  components=1
  boundaryCycles=1
  nonManifoldEdges=0
  degenerateTriangles=0

candidate over-cover 诊断：
  boundary_coverage=1
  normal_leakage_max=0.7464534307
  direction_fallback_count=3935
  direction_flip_count=0
  long_triangle_count=9984
  rejected_count=9984
  max_triangle_edge_length=0.3965649168
  triangle_count=2184
  max_offset=0.3125

Geomagic patch：
  faces=68
  edges=326
  preview high_risk=true
  boundary max=0.0101759896
  corner max=0.0101759896

normalized patch：
  raw patch unit=meter
  normalized patch unit=millimeter
  normalized_patch_unit_is_millimeter=true

Apply 失败：
  failure_reason=BuildFailed
  message=Multi-surface replacement face edges produced open wires.
  built_faces=14
  closed_wires=19
  open_wires=2
  failed_patch_face_index=7
  failed original boundary edge ids=1585
  endpoint_gap=0.7593067905601413
  pcurve rebuild failure=0
  SameParameter failure=0

Creo sewing 失败：
  exported STEP solids=0
  shells=2
  BRepCheck=false
  StrictTopologyGate failure=BRepCheckFailed
  after_free_edges=52
  ModelCHECK diagnostic_passed=false
  SHORT_EDGES item count=1593
```

判读：

```text
reverseEdge 方向翻转不再是当前证据（direction_flip_count=0）。
但 normal leakage 和 longTriangle rejection 明显过高，导致有效 over-cover 三角过少，Geomagic patch 又碎化为 68 faces/high-risk。
下一步应先把 longTriangleCount/rejectedCount 分解到 bridge vs quad strip，并修正 ring 对应或局部 bevel/fan 补洞；在 over-cover 输入稳定前，不应继续把主要精力放到 Apply / sewing 调参。
```

### 1.2 2026-06-27 raw patch UnifySameDomain 诊断

本轮先不改 Apply / sewing，也不放宽 StrictTopologyGate，只对 Geomagic 输出的 normalized raw patch 做独立诊断：

```text
normalized_patch_mm.stp -> stats -> UnifySameDomain -> stats
```

新增工具：

```text
tools/raw_patch_unify_probe.cpp
```

命令：

```powershell
build\windows-msvc-debug\Debug\raw_patch_unify_probe.exe `
  --input-step data\baseline_runs\route2_candidate_overcover_conservative_20260626_1810\route2_expanded_strict_trim\normalized_patch_mm.stp `
  --unified-step data\baseline_runs\route2_candidate_overcover_conservative_20260626_1810\route2_expanded_strict_trim\raw_patch_unify_probe\normalized_patch_mm_unified.stp `
  --report data\baseline_runs\route2_candidate_overcover_conservative_20260626_1810\route2_expanded_strict_trim\raw_patch_unify_probe\raw_patch_unify_report.json
```

结果：

```text
before normalized patch:
  faces=68
  edges=326
  shells=1
  free_edges=26
  BRepCheck=true

after UnifySameDomain in memory:
  faces=59
  edges=306
  shells=1
  free_edges=26
  BRepCheck=false

after unified STEP roundtrip:
  faces=59
  edges=306
  shells=1
  free_edges=26
  BRepCheck=true
```

判读：

```text
UnifySameDomain 可降低 raw patch 的碎面和碎边数量，但 free_edges 完全不变。
内存态 BRepCheck=false，说明不能直接把该结果当作 Apply 修复输入；写出 STEP 再读回后 BRepCheck=true，只能证明 STEP roundtrip 对该 shape 有恢复/规整作用。
该轮 Geomagic 基线的 blocker 仍是 over-cover 有效外扩带不足导致输出碎化，以及后续 multi-surface open-wire closure，不是单纯的 same-domain 合并缺失。
```

已验证：

```text
cmake --build --preset windows-msvc-debug --target raw_patch_unify_probe -- /m:1
raw_patch_unify_probe on normalized_patch_mm.stp
build\windows-msvc-debug\Debug\step_stats.exe data\baseline_runs\route2_candidate_overcover_conservative_20260626_1810\route2_expanded_strict_trim\raw_patch_unify_probe\normalized_patch_mm_unified.stp
```

### 1.3 2026-06-28 Candidate over-cover rejected 可定位诊断

本轮只补 over-cover 诊断，不改变三角生成策略，不改 Apply / PatchReplacementRepair / StrictTopologyGate。

已落地：

```text
1. StpSampledFittingReport 新增 candidate over-cover rejected 分桶：
   candidateSurfaceOverCoverBodyBridgeRejectedCount
   candidateSurfaceOverCoverBoundaryToRingRejectedCount
   candidateSurfaceOverCoverRingToRingRejectedCount
   candidateSurfaceOverCoverEdgeBuildRejectedCount

2. StpSampledFittingReport 新增可定位记录：
   candidateSurfaceOverCoverProblemEdges
   candidateSurfaceOverCoverRejections
   candidateSurfaceOverCoverLeakagePoints
   candidateSurfaceOverCoverMaxNormalLeakageEdgeId
   candidateSurfaceOverCoverMaxNormalLeakageSourceFaceId

3. corner_baseline_probe JSON 新增：
   candidate_surface_over_cover_body_bridge_rejected_count
   candidate_surface_over_cover_boundary_to_ring_rejected_count
   candidate_surface_over_cover_ring_to_ring_rejected_count
   candidate_surface_over_cover_edge_build_rejected_count
   candidate_surface_over_cover_problem_edges
   candidate_surface_over_cover_max_normal_leakage_edge_id
   candidate_surface_over_cover_max_normal_leakage_source_face_id

4. corner_baseline_probe artifact 导出目录：
   <output-dir>/candidate_surface_over_cover_diagnostics/

   导出文件：
   overcover_rejections.csv
   overcover_rejected_body_bridge.stl
   overcover_rejected_boundary_to_ring.stl
   overcover_rejected_ring_to_ring.stl
   overcover_worst_points.obj
```

artifact-only 验证：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
patch: data/baseline_runs/route2_candidate_overcover_conservative_20260626_1810/route2_expanded_strict_trim/normalized_patch_mm.stp
output: data/baseline_runs/overcover_diagnostic_probe_20260628

artifact 已落盘：
  overcover_rejections.csv
  overcover_rejected_body_bridge.stl
  overcover_rejected_boundary_to_ring.stl
  overcover_rejected_ring_to_ring.stl
  overcover_worst_points.obj

CSV 分桶：
  body_bridge=208
  boundary_to_ring_quad=3328
  ring_to_ring_quad=6656

max normal leakage:
  category=ring_to_ring_quad
  edge_id=1576
  source_face_id=1710
  sample_index=16
  ring_index_a=2
  ring_index_b=3
  normal_leakage=0.74645343073406678
  edge_length_ratio=7.182430623008532
```

注意：

```text
本次 artifact-only probe 为验证诊断文件落盘，运行到后续 Apply 路径时超时并已手动停止；
因此它不是完整 Route 2 验收结果，也不替代下一轮真实样例实验。
```

判读：

```text
单独 edge/source face id 仍不足以定位问题；现在 CSV/STL/OBJ artifact 能把 id 映射到三角、centroid、sample/ring 和可视化局部。
当前验证显示 ring-to-ring rejected 占主导，下一步优先检查 ring 对应 / miter 后环顺序，而不是先进入 Apply / sewing 调参。
edge 1576 / source face 1710 只能作为本轮真实样例线索，不能写死进默认测试或修复逻辑。
```

已验证：

```text
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
cmake --build --preset windows-msvc-debug --target corner_baseline_probe -- /m:1
corner_baseline_probe artifact-only run produced candidate_surface_over_cover_diagnostics files, then timed out in later Apply path and was stopped.
```

### 1.4 2026-06-28 Route 2 真实样例精简产物目录

`scripts/run_boundary_trim_fill_experiments.ps1` 已补充每轮 Route 2 精简产物目录：

```text
route2_artifacts/
  ROUTE2_SUMMARY.md
  GEOMAGIC_RUN.md
  crop_stl_candidate_<id>.stl
  raw_geomagic_crop_stp_candidate_<id>.stp
  normalized_patch_mm.stp
  base_removed_candidate.stp
  geomagic_fit_region.log
  geomagic_generation.stdout.txt
  geomagic_generation.stderr.txt（仅非空时保留）
  occt_applied_merged.stp（若生成）
  creo_api_merged.stp（若生成）
```

不再保留：

```text
run_exit_code.txt
顶层 run_boundary_trim_fill_experiments.stdout.txt / stderr.txt
run_invocation.txt
artifact_inventory.json
空的 route1_g1_boundary_fill 目录
```

如果 OCCT 或 Creo 某一路未生成 STEP，不允许从别处补伪产物；`ROUTE2_SUMMARY.md` 应保留该行并标记 `exists=false`，同时记录失败原因。

本轮复跑：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
output: data/baseline_runs/route2_candidate_overcover_conservative_rerun_20260628_015653
参数对齐 route2_candidate_overcover_conservative_20260626_1810：
  candidate_over_cover_samples=64
  candidate_over_cover_width=0.25
  candidate_over_cover_rings=3
  candidate_over_cover_miter_max_scale=1.25
  enable_auxiliary_support_collar=false
```

产物状态：

```text
crop STL: exists=true
raw Geomagic crop STP: exists=true
normalized patch STP: exists=true
base_removed_candidate: exists=true
Geomagic run log / command record: exists=true
OCCT applied/merged STEP: exists=false
Creo API merged STEP: exists=true
```

本轮目录已清理为只保留：

```text
data/baseline_runs/route2_candidate_overcover_conservative_rerun_20260628_015653/route2_artifacts/
```

OCCT 未导出 applied STEP 的原因：

```text
Multi-surface replacement face edges produced open wires.
Fallback wire-connect tolerance was also insufficient.
```

### 1.5 2026-07-02 conservative over-cover baseline 恢复

本轮只修改 STP sampled fitting STL 的 candidate/source-face parallel over-cover 输入，不改 Apply、PatchReplacementRepair、StrictTopologyGate，也不恢复旧 guard-band / adjacent collar 主路线。

当前本地最新状态：

```text
1. 放弃 2026-07-01 global wire-ring 和 2026-07-02 direction-field follow-up 作为主实现。
2. StpSampledFittingMeshBuilder 恢复 conservative per-edge over-cover：
   每条 original boundary edge 独立生成 boundary ring 与 ring1/ring2/ring3，
   只有该 edge 的所有 ring sample 成功后才按 original wire 顺序并入全局 ring。
3. boundary -> ring1、ring1 -> ring2、ring2 -> ring3 仍使用一一对应 closed quad strip；
   不保留上一轮为压低 longTriangle false rejection 加入的 radial/edge subdivision。
4. 保留并扩展诊断：
   ring_point_count
   ring_closure_max_gap / ring_closure_max_ratio
   index_mismatch_count
   source_face_transition_count
   edge_source_distribution
5. corner_baseline_probe 保留原 overcover_rejections.csv / rejected STL / worst points OBJ，并新增 overcover_ring_diagnostics.csv。
```

已验证：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
cmake --build --preset windows-msvc-debug --target corner_baseline_probe -- /m:1
```

本轮真实复跑：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
output: data/baseline_runs/route2_conservative_restored_baseline_20260702_134331
compact artifacts:
  data/baseline_runs/route2_conservative_restored_baseline_20260702_134331/route2_artifacts/

artifact roles:
  crop STL exists=true
  raw Geomagic crop STP exists=true
  normalized patch STP exists=true
  base_removed_candidate STP exists=true
  OCCT applied/merged STEP exists=false
  Creo API merged STEP exists=true

candidate over-cover:
  rejected_count=9984
  boundary_to_ring_rejected_count=3328
  ring_to_ring_rejected_count=6656
  body_bridge_rejected_count=208
  long_triangle_count=9984
  triangle_count=2184
  ring_point_count=1664
  index_mismatch_count=0
  direction_flip_count=0
  source_face_transition_count=22
  ring_closure_max_ratio=9.21257873636548
  max_triangle_edge_length=0.39656491680174

Geomagic patch:
  faces=68
  edges=326
  high_risk=true
  boundary max/p95=0.0101759896178517/0.000647815619539007
  seam max_abs/p95_abs=0.0028804812728491/0.000567814618038171

Apply:
  success=false
  failure_reason=BuildFailed
  message=Multi-surface replacement face edges produced open wires. Fallback wire-connect tolerance was also insufficient.
  built_faces=14
  closed_wires=19
  open_wires=2
  failed_patch_face_index=7
  failed original boundary edge ids=1585
  endpoint_gap=0.759306790560141

Creo / external validation:
  Creo Toolkit runner success=true
  StrictTopologyGate on Creo exported STEP=false
  failure=BRepCheckFailed
  after_free_edges=52
  after_multiple_edges=0
  ModelCHECK diagnostic_passed=false
  SHORT_EDGES item count=1593
```

失败实验留档（不作为当前基线）：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
full run output:
  data/baseline_runs/route2_candidate_overcover_wire_rings_20260701_230918
reuse-patch final-code probe:
  data/baseline_runs/overcover_wire_rings_reuse_patch_20260701_233801

candidate over-cover:
  candidate_surface_over_cover_rejected_count=0
  boundary_to_ring_rejected_count=0
  ring_to_ring_rejected_count=0
  body_bridge_rejected_count=208
  long_triangle_count=0
  triangle_count=12168
  ring_point_count=1664
  index_mismatch_count=0
  direction_flip_count=0
  source_face_transition_count=22
  ring_closure_max_ratio=9.21257873636548
  normal_leakage_max=0.719130368301623

Geomagic patch:
  faces=63
  edges=295
  high_risk=true

Apply:
  success=false
  failure_reason=BuildFailed
  message=Multi-surface replacement face edges produced open wires. Fallback wire-connect tolerance was also insufficient.
  built_faces=2
  closed_wires=6
  open_wires=2
  failed_patch_face_index=2

Creo / external validation:
  Creo Toolkit runner succeeded and exported creo_api_merged.stp.
  StrictTopologyGate on Creo exported STEP failed: BRepCheckFailed.
  after_free_edges=58
  after_multiple_edges=0
  after_brep_check_valid=false
  ModelCHECK diagnostic_passed=false
  SHORT_EDGES item count=1582
```

失败实验对比 2026-06-26 conservative over-cover 结果（只说明 rejection 下降，不说明视觉质量收敛）：

```text
旧结果：
  rejected_count=9984
  boundary_to_ring_rejected_count=3328
  ring_to_ring_rejected_count=6656
  long_triangle_count=9984
  triangle_count=2184
  Geomagic patch faces=68
  high_risk=true

新结果：
  rejected_count=0
  boundary_to_ring_rejected_count=0
  ring_to_ring_rejected_count=0
  long_triangle_count=0
  triangle_count=12168
  Geomagic patch faces=63
  high_risk=true
```

2026-07-02 boundary direction-field follow-up:

```text
fresh Geomagic output:
  data/baseline_runs/overcover_direction_field_fix_fresh_geomagic_20260702_003000
reuse-patch probe:
  data/baseline_runs/overcover_direction_field_fix_reuse_patch_20260702_002500

candidate over-cover:
  rejected_count=0
  boundary_to_ring_rejected_count=0
  ring_to_ring_rejected_count=0
  body_bridge_rejected_count=208
  long_triangle_count=0
  triangle_count=12194
  ring_point_count=1664
  direction_flip_count=0
  source_face_transition_count=22
  ring_closure_max_ratio=1.05507330248097
  max_triangle_edge_length=0.187747056537918
  normal_leakage_max=0.719130368301623

fresh Geomagic patch:
  faces=71
  edges=325
  high_risk=true
```

判读：

```text
wire-ring 与 direction-field 两次实验降低了 rejected/longTriangle 计数，但真实截图与 mesh 质量显示外扩方向和长三角形态仍不如 conservative baseline。
因此当前本地代码恢复 conservative per-edge over-cover 作为新基线，并只保留有助于定位的 ring/source-face 诊断。
后续 Route 2 测试目录继续以 data/baseline_runs/route2_candidate_overcover_conservative_rerun_20260628_015653/route2_artifacts/ 的紧凑分布为参考。
下一步不应改 Apply / repair / Gate；face-count A/B 已将主要嫌疑收敛到 body-to-ring bridge 的人造三角化 transition。
```

### 1.6 2026-07-02 Route 2 / Geomagic face-count A/B 诊断

本轮只加诊断层，不改变默认 conservative candidate/source-face over-cover，不改 Apply、PatchReplacementRepair、StrictTopologyGate 或 CommercialCadLikeQualityGate 判定语义。

已落地：

```text
1. StpSampledFittingOptions / Report 新增：
   candidateSurfaceOverCoverDisableOuterRings
   candidateSurfaceOverCoverBodyBridgeMode
   candidateSurfaceOverCoverSmoothStrip
   generated/emitted/disabled ring count
   emitted sample count
   body bridge mode / body bridge triangle count
   smooth strip / smooth transition 标记和 displacement 统计

2. corner_baseline_probe 新增显式实验参数：
   --candidate-over-cover-disable-outer-rings <n>
   --candidate-over-cover-body-bridge-mode <normal|none|smooth>
   --candidate-over-cover-disable-body-bridge
   --candidate-over-cover-smooth-transition
   --candidate-over-cover-smooth-strip
   --geomagic-no-sharpen-contours

3. candidate_surface_over_cover_diagnostics 新增：
   overcover_experiment_summary.json
   overcover_experiment_summary.csv
   overcover_ring_diagnostics.csv 扩展 emitted ring / body bridge / smoothing 字段

4. corner_baseline_probe 新增 probe_artifacts：
   fitting STL
   raw patch STEP/IGES
   fit_region log
   这些是 run-local copy，避免后续 candidate 179 复跑覆盖 data/crop_stl / data/crop_stp 下的共享文件。

5. run_corner_baseline_gate.ps1 与 run_boundary_trim_fill_experiments.ps1 透传上述 A/B 参数。
   Route 2 compact artifacts 会额外复制 candidate_surface_over_cover_diagnostics。
```

已验证：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests -- /m:1
ctest --preset windows-msvc-debug --output-on-failure --timeout 180
cmake --build --preset windows-msvc-debug --target corner_baseline_probe -- /m:1
```

真实 A/B 输出：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
output: data/baseline_runs/route2_facecount_ab_20260702_180826/
comparison: data/baseline_runs/route2_facecount_ab_20260702_180826/comparison.csv
```

| variant | emitted rings | bridge mode | over-cover tris | rejected | patch faces | patch edges | high risk | boundary max/p95 | seam max/p95 abs | result |
|---|---:|---|---:|---:|---:|---:|---|---:|---:|---|
| A_conservative_no_sharpen | 3 | normal | 2184 | 9984 | 68 | 326 | true | 0.010176 / 0.000648 | 0.002880 / 0.000568 | Apply gate failed |
| B_minus_outer_ring | 2 | normal | 2184 | 6656 | 68 | 326 | true | 0.010176 / 0.000648 | 0.002880 / 0.000568 | Apply gate failed |
| C_smooth_transition | 3 | smooth | 5096 | 9984 | 633 | 3054 | true | 0.007553 / 0.000129 | 0.001282 / 0.000106 | Apply gate failed |
| D_sharpen_contours | 3 | normal | 2184 | 9984 | 68 | 326 | true | 0.010176 / 0.000633 | 0.002880 / 0.000563 | Apply gate failed |
| E_disable_body_bridge | 3 | none | 0 | 9984 | 5 | 20 | false | 0.123518 / 0.044745 | 0.065219 / 0.026933 | Quality gate failed |
| F_smooth_strip | 3 | normal | 2196 | 9984 | 0 | 0 | false | n/a | n/a | Geomagic failed: Initialization of surface data failed |

判读：

```text
1. A -> B：
   禁用最外 ring 后 rejected_count 9984 -> 6656，
   但 actual emitted over-cover triangle_count 仍是 2184，
   Geomagic faces/edges/high_risk 仍是 68/326/true。
   这说明最外 ring 的 rejected long triangles 没进入 STL，不是 68-face patch network 的直接诱因。

2. A -> D：
   sharpenConstrainedContours 开关不改变 68/326/high_risk。
   当前 face count 不是 Geomagic sharpen contour 开关主导。

3. A -> C：
   smoothed body-to-ring transition 把 accepted over-cover triangles 提到 5096，
   Geomagic faces/edges 增到 633/3054。
   说明 Geomagic 对 body bridge / transition 的人造三角结构高度敏感，增加可见过渡三角会诱导更多 contour。

4. A -> E：
   禁用 body bridge 后 faces/edges 降到 5/20，high_risk=false，
   但 boundary/corner/seam drift 大幅超限，CommercialCadLikeQualityGate 失败。
   body bridge 同时是覆盖质量所必需，也是当前 68-face segmentation 的最可能输入来源。

5. F：
   smoothing strip 触发 Geomagic AutoSurface Initialization of surface data failed。
   该方向不能作为默认修复。
```

当前结论：

```text
conservative 输入视觉够用的主要原因是 body bridge 让主体 STL 覆盖到原 boundary 附近。
Geomagic 仍输出 68 faces / 326 edges / high_risk=true，最可能由 body-to-ring bridge 的人造特征线 / 三角化 transition 诱导。
外圈 ring 数量和 sharpenContours 参数不是本轮真实样例的主因。
后续应围绕 body bridge 的几何简化、contour 降噪或更少特征线的过渡策略做 A/B；不应继续加宽 ring / width / adjacent collar。
```

### 1.7 2026-06-25 Route 2 support collar 真实样例结果

本轮真实样例：

```text
source: data/stp/03_配件_Clay.stp
candidate: 179
output: data/baseline_runs/route2_support_collar_real_20260625_0020
```

结论：

```text
Route 2 仍失败，但失败点已经更清楚。

support collar 输入 STL 是干净的：
  Geomagic pre-repair components=1
  boundaryCycles=1
  nonManifoldEdges=0
  nonManifoldVertices=0
  degenerateTriangles=0
  effective support collar width min/mean/max=0.25/0.25/0.25

normalized patch 单位正确：
  source=millimeter
  raw patch=meter
  normalized patch=millimeter

patch 几何偏差明显改善：
  CommercialCadLikeQualityGate passed=true
  boundary max=0.0117107
  corner max=0.0108314
  seam max_abs=0.0117086

但 Geomagic 输出 patch face count=72，preview high_risk=true。
strict original-boundary multi-surface shell 仍失败：
  failure_reason=BuildFailed
  message=Multi-surface replacement face edges produced open wires.
  built_faces=6
  closed_wires=11
  open_wires=2
  failed_patch_face_index=2
  failed original boundary edge ids=1540,1543,1546
  failed open-wire endpoint gap=0.879063

Creo Toolkit sewing 仍不能救回灰色水密实体：
  runner_success=true
  stage1_toolkit_acceptance_passed=true
  exported STEP stats: solids=0, shells=2, faces=2762, BRepCheck=false
  StrictTopologyGate failed: BRepCheckFailed
  after_free_edges=60
  ModelCHECK diagnostic_passed=false
  SHORT_EDGES item count=1608
```

判读：

```text
本轮不再是 support collar 断开或 adaptive width 被压小的问题。
该轮 Geomagic 基线的 blocker 是 72-face patch 下的 strict multi-surface boundary shell open-wire closure，以及 Creo sewing 后仍为 shells=2 / solids=0。
当时的下一步不应恢复 guard-band / over-cover，也不应继续盲目加宽 support collar；应优先定位 failed_patch_face_index=2 上的 original-boundary segments 和 internal seams 为什么无法闭合。
```

### 1.2 当前关键诊断（2026-05-27）

```text
GUI 显示”看起来连续” ≠ STEP/B-rep 拓扑合法。

平面重建后，boundary wire / pcurve / orientation / 闭合关系不正确
→ 外部软件（非 OCCT）重新解析 STEP 时暴露为缺面、飞面、无限平面、开壳。

这不是渲染问题，而是 B-rep 拓扑 / 裁剪边界不稳定问题。
```

因此旧 OCCT 近似平面路线的安全收口项是 Stage 3A-Fix：
PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild。

---

### 1.2 Geomagic AutoSurface 当前关键诊断（2026-06-02）

```text
Geomagic AutoSurface “运行成功”不等于 patch 可接受。

已确认两个独立问题：

1. STL 局部裁剪结果若带内部孔洞 / 非流形顶点，会传递到 AutoSurface 输出。
   当前脚本默认执行 RepairMesh / RemoveNonManifoldVertices / FillSmallHoles。

2. AutoSurface.numPatches=1 只是 Geomagic 的近似 NURBS patch 目标，
   不保证导出的 STEP 只有 1 个 B-rep face。
   对当前 crop STL，Mechanical + autoMerge=true 明显优于 Organic。
```

真实样例验证结果：

```text
输入：data/crop_stl/<step文件stem>/<step文件stem>_candidate_0179.stl
输出：data/crop_stp/<step文件stem>/<step文件stem>_candidate_0179.stp
日志：data/crop_stp/<step文件stem>/<step文件stem>_candidate_0179_fit_region.log

修复前：boundaryCycles=11, nonManifoldVertices=2
修复后：boundaryCycles=1, nonManifoldVertices=0, FillSmallHoles numFilled=22

Mechanical + autoMerge=true + numPatches=1:
faces=12, edges=50, BRepCheck valid=true

对比：
Organic + autoMerge=true: faces≈273
Organic + autoMerge=false: faces≈494
Organic detail/tolerance 调参：face count 仍≈273
```

---

## 2. 最新进度同步

本次状态更新确认以下事项已经完成：

```text
1. UnlockEdgeCommand 已补充 document 校验。
2. 用户锁边进入 protectedEdges 的测试已补充。
3. GUI 手动流程验证已通过。
4. MergePatchCommand undo 时清空锁边状态是当前确认的正确语义。
5. Stage 2 Generic Merge Candidate Framework 已完成并在 Cleanup-6 收口：`MergeCandidate` / `MergePlanner` 当前只生成 `FeatureBoundedRefit` 候选；旧 `MergeRegionGrower` 和 PlaneLike 等 analytic candidate 检测已删除。
6. Stage 2.5 Candidate GUI Preview 已完成：支持 Top N、显示全部非隐藏候选、按 ID 高亮和清除候选高亮。
7. Stage 2.6 Candidate Selection / Rejection 已完成：支持候选区域点击选择、接受、拒绝、隐藏、恢复和状态统计。
8. Stage 3-0 Analytic RegionMerger Framework 是历史阶段：旧 stub merger、RegionMerge result/options 和 analytic candidate type 已在 Cleanup-1 / Cleanup-5 / Cleanup-6 删除。
9. Stage 3A PlaneRegionMerge 是历史阶段：旧 Plane 真实合并入口、Command、AppController API、后端和测试已在 Cleanup-2 / Cleanup-3 / Cleanup-4 删除。
10. PlaneRegionMerge 的历史修复记录只作为背景保留；当前仓库不再提供旧 PlaneRegionMerger 执行路径。
11. Stage 2.7 Face / Candidate Inspect 已完成：点击 face 可查看 surface type、候选归属、candidate type/status/risk/metrics，并支持未命中原因提示。
12. Stage 2.8 Analytic Primitive Candidate Detection 是历史阶段：PlaneLike / SphereLike / CylinderLike / ConeLike / TorusLike / Freeform 类型和检测实现已在 Cleanup-6 删除。
13. Stage 2.9 Multi-type Candidate Preview 已收口：报告、模型树、按类型筛选、Viewer 配色和 Face Inspect 当前只面向 `FeatureBoundedRefit` / `Unknown`。
14. Stage 2.8 Enhancement A 的 CylinderLike 近似检测已删除；当前不再生成 CylinderLike 候选。
15. Stage 2.8 Enhancement B 的 ConeLike 近似检测已删除；当前不再生成 ConeLike 候选。
16. Stage 3-S Shared Primitive Result Fields 曾完成：RegionMergeResult 当时新增通用 primitive 参数字段。Cleanup-5 后旧 `RegionMergeResult` / `RegionMergeOptions` 已删除；该阶段只保留为历史记录。
17. Stage 3D SphereRegionMerge 是历史阶段：旧 Sphere 真实合并入口、Command、AppController API、后端、测试以及 SphereLike 候选类型 / 检测 / 过滤 / 显示已全部删除。
18. SphereRegionMerge 一键批量合并防护只作为历史记录；当前仓库不再提供旧 Sphere 合并路径。
19. GUI 主工具栏已按功能收敛为下拉入口：选择、候选显示、候选状态、合并、检查/导出；旧 PlaneLike / SphereLike 专用候选显示入口已在 Cleanup-6 删除。
20. GUI 旧严格平面候选显示入口已删除；当前候选显示主线是 FeatureBoundedRefit preview 和状态筛选。
21. Stage 3A-Fix T3 Unsafe Candidate Rejection Report 是历史阶段：当时 `RegionMergeFailureReason` 提供稳定字符串转换；Cleanup-5 后该枚举已随旧 RegionMerge result 类型删除。
22. Stage 3A-Fix T4 RegionBoundaryAnalyzer 已完成：新增独立边界分析器，检查单连通区域、单一闭合外环、无内环/洞、无 non-manifold 边；复杂 boundary 先明确拒绝，不执行修复。Cleanup-5 后它使用独立 `BoundaryAnalysisFailureReason`，不再依赖旧 RegionMerge result 类型。
23. Stage 3A-Approx A1 是历史阶段：当时 PlaneRegionMergeOptions 新增 `allow_approximate_planar_surfaces=false` 和 `approximate_plane_max_deviation=0.01`；Cleanup-5 后相关 options 类型已删除。
24. Stage 3A-Approx A2 已完成：`allow_approximate_planar_surfaces=true` 时低误差 B-spline backed PlaneLike 可复用拟合平面、T4 边界分析、planar trimmed face 构造、ShapeValidator/BRepCheck 和 STEP roundtrip gate 完成近似平面重建；高偏差候选返回 `DeviationTooLarge`，失败时 document/stats 保持不变；GUI 实验入口仍留给 A4。
25. Stage 3A-Approx A3 已完成：strict native Plane 与 approximate B-spline planar rebuild 统一使用 `RegionBoundaryAnalyzer`；构造 boundary wire 时只使用 `analysis.ordered_boundary_edges`，不依赖原始 `candidate.boundary_edges` 顺序；open/disconnected/multiple-loop/hole/non-manifold/branching boundary 仍按 strict 策略拒绝，不做 ShapeFix 或 pcurve 深度修复。
26. Stage 3A-Approx A4 历史阶段已完成：当时 GUI 新增“实验性合并当前近似平面候选”和“实验性合并全部近似平面候选”入口；Cleanup-2 后这些旧 Plane GUI 真实合并入口已下线，Cleanup-3 后对应 AppController API / Command / command 测试已删除，Cleanup-4 后 `PlaneRegionMerger` 后端和后端测试也已删除。
27. Stage 3A-Approx A5 已完成：补齐 strict/approx 自动测试和阶段收口验证；覆盖 B-spline backed PlaneLike strict 拒绝、approx 低误差成功、高误差失败、RegionBoundaryAnalyzer invalid boundary failure、batch approx mixed valid/invalid 跳过语义、全部 invalid failure、STEP roundtrip failure、失败 rollback 和 command undo/redo 既有路径。
28. Geomagic AutoSurface T4 路径已改为真实 `wrapCore.exe --script` + `FIT_REGION_*` 环境变量；真实脚本不再要求 `config.json` 作为调用输入。
29. Geomagic Python 脚本已对齐标准 `fit_region.py` 风格：只输出一个 fit_region log，不再写大量 result JSON；中间 IGES keepTemp 时作为 `<output>_autosurface.igs` sidecar 保留。
30. Geomagic Python 脚本已加入默认网格修复：`RepairMesh`、`RemoveNonManifoldVertices`、`FillSmallHoles`；默认 `FIT_REGION_REPAIR_MESH=1`。
31. Geomagic AutoSurface 默认参数已改为 `geometry=Mechanical`、`autoMerge=true`、`adaptiveFit=false`、`numPatches=1`；真实 crop 样例 face count 从 Organic 的约 273 降到 Mechanical 的 12。
32. GeomagicAutoSurfaceBackend 已支持无 result JSON 兜底：真实脚本产出 STEP 即可判定成功，避免 wrapCore.exe 非零 exit code 误判。
33. PatchImportService 已完成：可导入 STEP/STP/IGS/IGES，返回 shape、face/edge/shell/solid 统计、bbox、BRepCheck；不接受或修改 ShapeDocument。
34. PatchImportService 真实接入测试已拆分：默认测试只导入已有 crop_stp / crop_igs 文件；设置 `SPO_ENABLE_REAL_GEOMAGIC_TESTS=1` 时跑通 crop STL → Geomagic → STEP → PatchImportService。
35. `tools/step_stats` 已可用于导入 STEP/IGS 并输出 face/edge/shell/solid/bbox/BRepCheck 统计，用于判断 Geomagic 输出是否适合继续 overlay / Apply。
36. Geomagic AutoSurface T5.2.0 PatchArtifactLocator 已完成：可从 local STL / GeomagicAutoSurfaceResult 动态定位 patch STEP、IGES sidecar 和 fit_region log；生产逻辑不写死当前真实样例文件名；Windows 中文目录 / 中文 stem 查找使用 native `std::filesystem::path` 拼接，避免 UTF-8 narrow string 重建文件名导致同名 patch 查找失败。
37. Geomagic AutoSurface T5.2 patch overlay 已完成基础版：GUI 支持“从 local STL 定位并导入 Patch”和手动 CAD patch 文件导入；前者选择 local STL 作为 artifact key 并自动定位同 stem patch，后者直接选择 STEP/STP/IGS/IGES。Viewer 以独立 AIS_Shape 叠加显示，清除或重复导入不会修改主 ShapeDocument。
38. Geomagic AutoSurface T5.3 PatchPreviewReport 已完成基础版：报告 candidate/source 统计、artifact 路径、patch 拓扑统计、bbox deviation、BRepCheck、warning 和 recommended action；multi-face patch 仅作为 warning，明显异常标记 HighRisk。
39. Geomagic AutoSurface T5.3.1 一键 Patch cutout overlay preview 已完成：GUI 可对当前 FeatureBoundedRefit candidate 自动裁剪 local STL、运行 Geomagic、导入 patch，并在 Viewer 中以 visual-only 方式隐藏 source faces 后叠加 patch；主 ShapeDocument 不修改。
40. 仓库脚本已补齐：`scripts/verify_spo.ps1` 作为本地统一验证入口，`scripts/run_geomagic_patch.ps1` 作为手动复现 Geomagic patch 生成入口；`verify_spo.ps1` 会在 Geomagic/Patch 相关代码或仓库脚本变更但进度文档未同步时失败，避免后续遗漏文档同步。
41. Geomagic AutoSurface T5.4 Patch Apply 状态机与按钮门控已完成：新增 `RegionPatchStatus` / `PatchApplyDecision`，GUI 新增“应用当前 Patch（T6 占位）”；只有 PreviewReady 且非 HighRisk、bbox/BRepCheck/face count 合法时允许请求 Apply。T5.4 当时点击后仅进入 ApplyPending 并提示 T6 未实现，不修改 ShapeDocument，不执行 replacement / sewing / ShapeFix / StrictTopologyGate；该占位入口已在 T6.5 被真实 Apply 流程替换。
42. Geomagic AutoSurface T5.4.1 patch preview 生命周期安全修正已完成：打开新 STEP、undo/redo 成功、旧 SameDomain/Plane/Sphere 合并成功修改主模型、GUI refreshDocumentViews 刷新主模型时会清空旧 patch preview 状态和 viewer overlay，避免旧 patch 被误用于新模型。
43. Geomagic AutoSurface T6.0 PatchReplacement 输入结构与 MultiFacePatchAnalyzer 已完成：新增 `PatchReplacementInput` / `PatchReplacementReport` / `PatchReplacementFailureReason` / `MultiFacePatchAnalyzer`，在真实 replacement 前统一校验 document、candidate、boundary、imported patch 和 preview report，并分析 TopoDS_Face / Shell / Solid / Compound 形态下的 imported patch 多面片拓扑；`patchFaceCount > 1` 不作为失败条件，multi-face patch 标记为主路径输入；本阶段不修改 ShapeDocument，不实现 Command / GUI / sewing / ShapeFix / StrictTopologyGate。
44. Geomagic AutoSurface T6.2 StrictTopologyGate 最小可用版已完成：新增 `StrictTopologyGateInput` / `StrictTopologyGateReport` / `StrictTopologyFailureReason` / `StrictTopologyGate`，在 PatchReplacementCommand 提交前评估 before/after ShapeDocument；Gate 检查 after BRepCheck、free edge / multiple edge 增量、solid/shell 一致性、bbox、STEP export 和 STEP roundtrip；`replacementFaceCount > 1` 不作为失败条件，multi-face replacement 只记录 warning；本阶段不修改 ShapeDocument，不实现 PatchReplacementCommand / GUI / Geomagic 调用 / STL 重新裁剪。
45. Geomagic AutoSurface T6.1 BoundaryConstrainedPatchBuilder 已完成：新增 `BoundaryConstrainedPatchBuilder` / `BoundaryConstrainedPatchBuildOptions` / `BoundaryConstrainedPatchBuildResult`，可基于 `PatchReplacementInput` 与 `MultiFacePatchAnalysis` 构造 one-face fragment 或 multi-face compound replacement fragment；multi-face patch 是主路径，不因 `patchFaceCount > 1` 返回 unsupported；内部 patch seam 会保留到 `internalPatchEdges`；boundary mismatch 只记录 warning，最终可提交性留给 StrictTopologyGate / 后续 sewing；本阶段不修改 ShapeDocument，不实现 Command / GUI / Geomagic 调用。
46. Geomagic AutoSurface T6.3 PatchReplacementCommand 最小可用版已完成：新增 `PatchReplacementCommand`，通过 Command 层执行 `PatchReplacementInput` 校验、`MultiFacePatchAnalyzer` 分析、`BoundaryConstrainedPatchBuilder` 构造 replacement fragment、`StrictTopologyGate` 验证、Gate 成功提交 afterDocument、Gate 失败 rollback，并支持 undo/redo；redo 只复用缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新读取固定 patch 路径；multi-face replacement fragment 是主路径，`patchFaceCount > 1` 不返回 unsupported。当前 T6.3 未真正删除 / 替换 candidate source faces，未执行 sewing / ShapeFix / SameParameter，未接入 GUI；最小 afterDocument 策略是将 imported patch top-level shape 作为 gated afterDocument candidate，普通局部 patch 若无法满足 StrictTopologyGate 会失败并保持主 ShapeDocument 不变。
47. Geomagic AutoSurface T6.4 Sewing / ShapeFix / SameParameter 最小可用版已完成：`PatchReplacementCommand` 现在使用 `BRepTools_ReShape` 对 candidate source faces 执行真实替换尝试，第一个 source face 替换为 `BoundaryConstrainedPatchBuilder` 生成的 replacementShape，其余 source faces 移除；replacementShape 可为 one-face 或 multi-face compound/shell fragment，`patchFaceCount > 1` 和 internal patch seams 不作为 unsupported。临时 after shape 会执行 `BRepLib::SameParameter`、`ShapeFix_Wire`、`ShapeFix_Face` 和 `BRepBuilderAPI_Sewing` 尝试，并把 repair 前后 face/edge/shell/solid、free edge、multiple edge 统计写入 `PatchReplacementReport`；若 sewing 结果为空或丢失 solid 拓扑，会记录 warning 并保留 pre-sewing shape。`StrictTopologyGate` 仍是最终提交门，Gate 成功才提交 afterDocument，Gate 失败 rollback 且主 ShapeDocument 不变；redo 只复用缓存 afterDocument，不重新运行 repair、不运行 Geomagic、不裁剪 STL、不读取固定 patch 路径。本阶段仍未接入 GUI，未绕过用户确认，也不信任 STL crop boundary 或 Geomagic patch outer boundary 作为最终 CAD boundary。
48. Geomagic AutoSurface T6.5 AppController / GUI 真实 Apply 接入已完成：新增 `AppController::applyCurrentPatchToCurrentCandidate`，GUI “应用当前 Patch 到候选区域”会获取当前 candidate 并通过 `CommandHistory` 执行 `PatchReplacementCommand`。Apply 前会验证 document、preview ready、preview/candidate 一致性、FeatureBoundedRefit 类型、candidate 状态和当前 document 上重新分析得到的 single closed outer boundary；成功后提交 afterDocument 到主 `ShapeDocument`、清除 patch overlay / cached artifact / imported patch / preview report、状态置为 `Applied`，失败后主 `ShapeDocument` 不变且保留 overlay / preview state 便于复盘，状态置为 `ApplyFailed`。GUI Apply 路径启用 `StrictTopologyGate` 严格水密选项：before 为 solid 时 after/STEP roundtrip solid count 必须保持，after 与 roundtrip 都必须 BRepCheck 通过、free edges=0、multiple edges=0，STEP export/readback 必须成功。`PatchReplacementCommand` 在 one-face path 中会用 imported patch face 的 surface 加原 STP candidate boundary wire 重建 trimmed face；multi-face replacement fragment 仍为主路径，不因 `patchFaceCount > 1` 或 internal seams 返回 unsupported，最终是否提交由 repair + StrictTopologyGate 决定。undo/redo 已接入，redo 只复用缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新导入 patch、不重新运行 repair。
49. Geomagic AutoSurface T6.5.1 Apply failure diagnostics 已完成：`PatchReplacementReport` 现在记录 `StrictTopologyGateReport` 的 before / after / STEP roundtrip face/edge/shell/solid、free edge、multiple edge、BRepCheck、STEP export、STEP roundtrip 和 watertight 选项；`PatchReplacementCommand` 在 Gate 成功或失败时都会把 gate failure reason、message、warning 和 stats 写入 report；GUI Patch Apply report 会显示 repair 前后 face/edge/shell/solid、repair free/multiple edge、Gate before/after/roundtrip stats 和 solid/watertight 判断。失败后仍保持主 `ShapeDocument` 不变且保留 overlay / preview state；本阶段未放宽 `StrictTopologyGate`，未重新运行 Geomagic，未重新裁剪 STL，未写死真实样例路径；可选 rejected after debug STEP artifact 未实现。
50. Geomagic AutoSurface T6.6 Industrial Adaptive Sewing 路线已根据 `scripts/industrial_sew.py` 确定：将三阶段策略迁移到 C++ repair pipeline，包括 `ShapeFix_Shape` / face orientation 修复、`ShapeUpgrade_UnifySameDomain`、多 tolerance `BRepBuilderAPI_Sewing` loop、`ShapeFix_Shell`、`BRepBuilderAPI_MakeSolid`、`ShapeFix_Solid`、collapsed guard 和 best-result selection。该路线不放宽 StrictTopologyGate，不绕过 rollback，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary，multi-face patch 仍是主路径，redo 仍只复用缓存 afterDocument。
51. Geomagic AutoSurface T6.6 Industrial Adaptive Sewing 已完成最小 C++ 集成：新增 `PatchReplacementRepair` 模块并由 `PatchReplacementCommand` 调用，repair 顺序覆盖 `ShapeFix_Shape`、`SameParameter`、`ShapeFix_Wire`、`ShapeFix_Face`、`ShapeUpgrade_UnifySameDomain`、adaptive `BRepBuilderAPI_Sewing` tolerance loop、`ShapeFix_Shell` / `BRepBuilderAPI_MakeSolid` shell-to-solid、`ShapeFix_Solid` 和 final unify。`PatchReplacementReport` 会记录 selected sewing tolerance、sewing attempt count、best sewing face/edge/shell/solid、best free/multiple edge、best BRepCheck、collapsed 状态、shapeFixShape/unify/shellToSolid/adaptiveSewing 是否执行；GUI Apply report 已展示这些字段。结果选择保持严格：preferred tolerance=0.007 的 valid non-collapsed solid 优先，否则选择 valid solid、未塌缩、free edge 最少的结果；没有 valid non-collapsed solid 时只保留 best attempt 诊断，不把塌缩或丢 solid 的 sewing result 当成功提交，最终仍由 `StrictTopologyGate` 决定提交或 rollback。redo 仍只复用缓存 afterDocument，不重新运行 repair / adaptive sewing / Geomagic / STL crop / patch import。
52. Geomagic AutoSurface T6.6.1 Crop Boundary Diagnostics 已完成第一版：新增 `CropBoundaryDiagnostics` / `CropBoundaryDiagnosticsReport`，基于 `RegionBoundaryAnalysis` 与 `BoundaryWireBuilder` 对原 STP candidate outer boundary loop 按 edge 采样，记录 edge id、sample count、3D point、edge length 和 single closed outer loop 状态；将原 boundary sample 分别与 local STL crop mesh、imported patch outerEdges 做最近距离统计，输出 min / max / average distance、missing point count、连续超限 segment、suspected gap edge ids、message / warning。GUI patch preview / import 成功后会自动显示 T6.6.1 overlay：黄色原 STP boundary loop、青色 imported patch outer boundary、红色 local STL crop coverage issue、洋红 patch boundary mismatch；Patch preview report 同步显示 T6.6.1 数值字段。本阶段不修改 `ShapeDocument`，不调用 Geomagic，不重新裁剪 STL，不改变 `PatchReplacementCommand` / `StrictTopologyGate` / redo 语义，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary。
53. Geomagic AutoSurface T6.6.2 Process Status Panel 已完成第一版：新增 `ProcessStatusSnapshot` / `ProcessStage` 和 GUI 底部“进程”页，显示当前阶段、candidate、local STL / patch STEP / IGS / fit_region log 路径、selected sewing tolerance、sewing attempt count、best sewing free/multiple edge、best face/edge/shell/solid、repair/adaptive sewing 状态、StrictTopologyGate evaluated/passed/failure、message 和 warning。`AppController` 会在 patch import、PreviewReady、Apply early failure、replacement build、PatchReplacementCommand 成功/失败、undo/redo 后同步快照；`MainWindow` 会在 crop / Geomagic preview pipeline / import / Apply / clear overlay / undo / redo 后刷新面板。ApplyFailed 后状态保留最后的 repair / adaptive sewing / Gate 参数；undo/redo 显示 CachedUndo / CachedRedo，不伪装成重新运行 Geomagic / crop / import / repair。第一版是阶段级状态；adaptive sewing 每个 tolerance attempt 的实时逐步刷新留给后续 job/progress callback。
54. Geomagic AutoSurface T6.6.3 STL Crop Boundary-Band Diagnostics 已完成第一版：`CropBoundaryDiagnostics` 现在在原 boundary sample 外增加 candidate 内侧 boundary-band sample，并统计 band sample 到 local STL crop mesh 的 min / max / average distance、missing point count、missing edge ids 和 suspected crop hole edge ids；同时对已打开的 source STL 做只读 triangle rejection audit，复现当前 `StlRegionExtractor` 的 bbox intersects + centroid-inside 判据，并记录 centroid outside 但 vertex / edge midpoint / near-boundary-band 命中的 conservative keep candidate、rejected near-boundary triangle count 和有限数量 overlay triangle。GUI Patch preview report 已显示 boundary-band 与 triangle audit 数值，overlay 新增橙色 boundary-band missing area 和紫色 near-boundary rejected triangles；Process Status Panel 同步显示 crop band/audit 摘要。裁剪 bbox 线框仍可手动打开，但默认隐藏，避免遮挡 STL、原 STP loop 和诊断 segment。本阶段不改变正式 crop 输出，不调用 Geomagic，不重新生成 patch，不修改 `ShapeDocument`，不放宽 `StrictTopologyGate`，不改变 redo 语义。
55. Geomagic AutoSurface T6.6.4 STL Region Extractor 保守裁剪修复已完成：`StlRegionExtractorOptions` 新增 vertex-inside、edge-midpoint-inside、boundary-band inclusion 与 leak guard 参数；在 `ConservativeBoundaryBand` 模式下，crop 对 triangle bbox 与 expanded candidate bbox 相交的三角片，除 centroid-inside 基线外，会保守保留 vertex inside、edge midpoint inside 或靠近原 STP candidate boundary band 的三角片。`StlCropReport` 记录 centroid / vertex / edge-midpoint / boundary-band keep count、conservative keep total、outside bbox / outside candidate reject count 和 warning；GUI “STL 裁剪完成”报告同步显示这些统计。实现保留 output bbox guard 与 output/centroid triangle ratio guard，避免无约束扩大 local STL。本阶段不调用 Geomagic，不修改 `ShapeDocument`，不绕过 `StrictTopologyGate`，不改变 redo 语义，不把 STL crop boundary 当最终 CAD boundary。
56. Geomagic AutoSurface T6.6.4.1 STL Crop Mode Switch 已完成：`StlRegionExtractorOptions::mode` 默认回到 `CentroidOnly`，等价旧 crop；T6.6.4 的 vertex / midpoint / boundary-band inclusion 保留在 `ConservativeBoundaryBand` 模式。GUI `STL -> 启用保守边界裁剪` 默认关闭，并同时控制“裁剪当前候选 STL”和“生成并预览当前 Patch”；裁剪报告和自动 Patch preview report 显示 crop mode。手动验证显示 conservative-boundary-band 能补齐 local STL，但可能让 AutoSurface 生成过多 patch faces 并扩大 patch boundary mismatch，因此当前生产默认不启用保守裁剪。本阶段不修改 `ShapeDocument`，不改变 Apply / `StrictTopologyGate` / redo 语义。
57. Geomagic AutoSurface T6.6.5 Regenerate Patch + Apply Verification 已完成手动路线验证：legacy centroid-only crop 仍能得到较简单的 AutoSurface patch，但 patch outer boundary 不能可靠闭合原 CAD candidate boundary；conservative-boundary-band crop 可改善 local STL 覆盖，但会把边缘褶皱 / 邻接特征带入 AutoSurface，导致 patch face count 和 patch boundary mismatch 恶化。Apply 失败形态仍集中在 replacement seam free edges 与 BRepCheck failure，说明问题不在 sewing tolerance 或 StrictTopologyGate 过严，而在“direct AutoSurface patch outer-boundary replacement”路线本身不成立。
58. Geomagic AutoSurface T6.7 Boundary-Constrained Surface Re-trim 第一版已完成：新增 `BoundaryConstrainedSurfaceRetrim`，`BoundaryConstrainedPatchBuilder` 默认从 imported Geomagic patch 选择可用 surface，并使用原 STP candidate outer boundary wire 构造 trimmed replacement face；Geomagic patch outer boundary 不再作为默认 replacement boundary，legacy direct patch fragment 只保留为显式测试/兼容路径。单一 surface 无法覆盖原 boundary 时先输出最佳单 surface 投影统计与 all-surface 最近投影 coverage，成功构造后的 replacement 仍必须经过 `PatchReplacementRepair` 与 `StrictTopologyGate`，redo 仍不得重跑 Geomagic / crop / import / repair。
59. Geomagic AutoSurface T6.6.4.2 STL Boundary Loop Repair Connectivity Guard 已完成：boundary-loop repair 只允许加入与当前已保留 crop mesh 顶点近似连通的三角片；仅满足边界距离但不连通的 repair candidate 会计入 `boundary_loop_orphan_repair_candidate_count` 并被拒绝，GUI “STL 裁剪完成”报告同步显示 orphan repair candidates。`missing after=0` 现在只代表边界采样距离满足阈值，不再被视为充分成功条件；如果缺口只能靠漂浮碎片补齐，则正确结果是拒绝该碎片并保留 missing/警告。
60. Geomagic AutoSurface stale output cleanup 已完成：`GeomagicAutoSurfaceBackend` 和 `scripts/geomagic_wrap/autosurface_pipeline.py` 在新运行前删除旧 STEP / IGS 输出，IGES→STEP 转换每次 WriteFile 前也会删除旧 STP，避免 AutoSurface 失败或未覆盖时把上一次结果误判为新 patch。backend 现在通过 `FIT_REGION_*` 环境变量传递 log、mesh repair、remesh、smooth/relax 与 AutoSurface 参数。
61. Geomagic AutoSurface Remesh opt-in 修正已完成：真实日志显示 Remesh 自动 target edge length 可低到不合理量级，并导致 AutoSurface `Initialization of surface data failed`。`GeomagicAutoSurfaceConfig::skipRemesh` 默认改回 `true`，脚本默认 `FIT_REGION_SKIP_REMESH=1`，示例配置同步为 `skip_remesh=true`；GUI Patch 菜单新增“启用 Geomagic Remesh”显式实验开关。脚本执行 Remesh 失败时只写 warning 并继续使用原 mesh；如果 Remesh 成功但随后 AutoSurface 全部失败，脚本会 retry pre-remesh mesh，避免实验 Remesh 阻断可生成的 no-remesh 路径。
62. Geomagic AutoSurface T6.7.4 Strict Multi-surface Boundary-Constrained Shell 已完成增强版：新增 `BoundaryConstrainedMultiSurfaceShellBuilder`，在单 surface retrim 失败但 all-surface coverage 无 uncovered sample 时，将原 STP candidate boundary edges 整段优先分配给最佳 Geomagic surface；若某条 edge 没有单一 surface 能覆盖整条 edge，但每个采样点均有 surface 覆盖，则只对该失败 edge 按 surface ownership 切分参数区间。T6.7.4 用原 STP edge 参数区间构造最终外边界段，并保留 imported patch 内部 seam edges 连接闭合 wire；若同一 patch face 形成多个 closed wire，现在全部构造成 replacement face，不再丢弃额外闭合环。`BoundaryConstrainedPatchBuilder` 默认按 single-surface retrim → multi-surface shell → legacy fallback-disabled 的顺序执行；T6.7.4 失败时返回 BuildFailed 并报告 boundary sample / assigned segment / split edge / built face / closed wire / open wire / failed patch face / failed edge diagnostics，不回退到 patch outer boundary。成功结果仍进入 `PatchReplacementRepair` 与 `StrictTopologyGate`；redo 语义不变。
63. T6.7.4 真实 Apply 诊断工具与阶段性结论已补齐：新增参数化命令行工具 `patch_apply_probe`，可加载 source STEP、按 candidate id 选择 FeatureBoundedRefit candidate、导入 patch STEP/IGES，并执行与 GUI Apply 对齐的 replacement / repair / StrictTopologyGate 路径；工具不运行 Geomagic、不裁剪 STL、不写死真实样例路径，并输出阶段进度用于定位真实大样例耗时。当前真实 GUI / Apply report 结论显示 multi-surface shell 已不再停在单 surface coverage 阶段，后续主要失败转为 split boundary 与邻接旧拓扑/桥接闭合收口；不应回退到 Geomagic patch outer boundary。
64. STP-sampled synthetic fitting STL input mode 已完成：新增 `StpSampledFittingMeshBuilder` / `StpSampledFittingOptions` / `StpSampledFittingReport`，可从当前 STP candidate faces、outer boundary 和 boundary band 采样生成 synthetic fitting STL，再交给 Geomagic AutoSurface。`GeomagicFittingInputMode::StpSampledCandidateSurface` 已接入 `AppController::generatePatchPreviewForCandidateData`，生成的 STL 仍写入 `data/crop_stl/<step-stem>/candidate_<id>.stl`，后续 patch artifact 定位、Geomagic 输出、PatchImportService 和 Apply 流程保持一致。该路径不需要源 STL，适合作为当前默认生产输入。
65. GUI 默认 fitting input mode 已切到 STP sampled：`MainWindow::fittingInputMode_` 默认值和对应 QAction 都是 `StpSampledCandidateSurface`；“生成并预览当前 Patch”在该模式下不会要求先打开原始 STL。Legacy STL crop 与 Conservative Boundary Band STL crop 仍保留在 Patch 菜单用于对照验证，报告会输出 `fitting input mode` 字符串，避免把不同输入路线混在一起。
66. STL Global Cut Chain crop 已完成第一版并接入 GUI：新增 `StlCropMode::GlobalCutChain` 与 `StlCutChainCutter`，从原 STP candidate ordered boundary edges 采样 boundary loop，在源 STL 上投影、跟踪 cut chain、按约束重三角化并 flood-fill 选出 patch mesh；GUI `STL -> 启用全局切链裁剪 (Global Cut Chain)` 可显式开启，单独裁剪当前候选 STL 时会走该路线。它是 STL 全局切链裁剪器，不是当前默认 Geomagic fitting input mode。
67. Global Cut Chain boundary snap 已接入：`StlCutChainOptions` 默认 `snapBoundaryToRed=true`、`snapMaxDist=0.2`，切链后的 patch outer boundary 可向原 STP red boundary loop 回贴，降低投影到 STL 后的 green boundary 漂移。该 snap 只改善 local STL / fitting STL 输入质量，不改变最终 CAD boundary；Apply 仍必须使用原 STP candidate outer boundary wire，并通过 PatchReplacementRepair 与 StrictTopologyGate。
68. GUI 后台任务与状态显示优化已按 `1f0c3d7` 可缝合基准完成移植：`openStepFile()`、`exportStepFile()`、`previewMergeCandidates()` 和 Patch Apply 的计算 / 读取 / 导出进入 worker，viewer / model tree / report / Process Status 更新回 GUI 线程；长任务期间禁用会修改 document / controller 的入口；Process Status 与 Patch Apply report 展示 Gate before / after / STEP roundtrip 的 BRepCheck、free edge、multiple edge 和 face / edge / shell / solid 计数。`d1c00e4` Improve boundary constrained Geomagic patch flow 与 Sharp Contours 实验不进入本轮主线。
69. 文档入口与下一阶段方向已同步到 `a57695d` 主线：仓库根目录恢复 `AGENTS.md`，保证新对话会先读全局 Workspace、项目记忆和仓库文档；当前后续研究问题明确为 Geomagic 在拐角 / 特征交汇处圆角化导致商业 CAD 出现缝隙。下一轮应优先评估 corner-aware / curvature-aware 采样、STP boundary guard-band 外扩采样、feature/corner anchors、原 STP boundary 重裁剪和 Creo-like 高密度偏差门控，而不是继续 Sharp Contours 或重新合入 `d1c00e4`。
70. corner preservation 最终方案已确定为“双层闭环”：前端通过 corner-aware / curvature-aware sampling、feature/corner anchors 和 STP boundary 外 guard-band 采样改善 Geomagic fitting input；后端在 `StrictTopologyGate` 之外新增 commercial-CAD-like 高密度几何门控，测量 boundary deviation、corner anchor drift、feature edge drift、sharpness preservation、surface COPS-like deviation 和 STEP roundtrip 后 geometry drift。第一轮实验分支为 `experiment/corner-preservation-ab`，A/B 矩阵为 baseline、corner 加密、guard-band、corner+guard-band；成功不能只看 GUI 或 OCCT BRepCheck。
71. A0 baseline 自动化入口已落地：新增 `corner_baseline_probe` CLI，流程为 source STEP + candidate id → STP-sampled fitting STL → Geomagic AutoSurface 或 `--patch` 复用已有 patch → Patch Apply + `StrictTopologyGate` → `CommercialCadLikeQualityGate` → JSON 报告。新增 `CommercialCadQualityGate` 独立模块，第一版测量原 STP boundary、corner anchor 和 feature boundary samples 到 imported patch 的 max/mean/RMS/p95 drift；它不替代 `StrictTopologyGate`，只补足 OCCT 拓扑 gate 看不到的 commercial-CAD-like 几何偏差。
72. `corner_baseline_probe` 的 Geomagic staging 已对齐 GUI 路径：输入 STL / 输出 STP / fit log 写入仓库 `data\crop_stl`、`data\crop_stp`、`data\crop_igs`，报告写入 `data\baseline_runs`。真实样例 `03_配件_Clay.stp` candidate 179 的 A0 脚本已复现 GUI 后端：Geomagic STP 生成成功，Patch Apply 与 `StrictTopologyGate` 通过，但 `CommercialCadLikeQualityGate` 失败，boundary/feature max drift 0.123574、p95 0.044766，corner p95 0.086309。
73. 旧无用代码 Cleanup-1 已完成第一批无损瘦身：删除未接入 GUI / AppController 的 `CylinderRegionMerger`、`ConeRegionMerger`、`TorusRegionMerger`、`RegionMergeStub` 和 `tests/test_region_merge_stubs.cpp`，并从 CMake 移除对应源文件 / 测试。Cleanup-1 当时保留的旧候选类型和检测通道已在 Cleanup-6 删除。同步修正 CTest 超时为 300 秒，并让 `scripts/test.ps1` 传播 native command 失败码。
74. 旧无用代码 Cleanup-2 已完成：`MainWindow` 旧 Plane / Sphere 真实合并菜单、action、connect 和 handler 已下线；候选按类型显示、严格平面候选显示、same-domain `applyMerge()`、Geomagic Patch preview / Apply、undo/redo 保留。Cleanup-2 阶段 Plane / Sphere 的 AppController API、Command、后端 merger 和测试暂时保留，作为后续删除对象。
75. 旧无用代码 Cleanup-3 已完成代码侧清理：删除 Plane / Sphere 的 AppController merge API、`PlaneRegionMergeCommand` / `PlaneRegionBatchMergeCommand` / `SphereRegionMergeCommand` / `SphereRegionBatchMergeCommand` 及对应 command 测试，并从 CMake 与 `tests/test_validation.cpp` 解绑。`PlaneRegionMerger` / `SphereRegionMerger` 后端和后端测试暂保留，作为 Cleanup-4 单独处理对象。
76. 旧无用代码 Cleanup-4 已完成：删除 `PlaneRegionMerger` / `SphereRegionMerger` 后端和 `tests/test_plane_region_merger.cpp` / `tests/test_sphere_region_merger.cpp`，并从 CMake 与 `tests/test_validation.cpp` 解绑。`tests/test_analytic_candidate_detection.cpp`、`tests/test_candidate_type_statistics.cpp`、`tests/test_region_boundary_analyzer.cpp` 已改为测试候选检测、候选过滤和 boundary analyzer 本身，不再实例化 PlaneRegionMerger。
77. 旧无用代码 Cleanup-5 已完成：新增 `BoundaryAnalysisFailureReason`，`RegionBoundaryAnalyzer` 和对应测试不再依赖旧 `RegionMergeFailureReason`；删除 `src/merge/RegionMergeResult.h` 与 `src/merge/RegionMergeOptions.h`。
78. 旧无用代码 Cleanup-6 已完成：删除旧 analytic candidate enum 值、`MergeRegionGrower` 和 `tests/test_analytic_candidate_detection.cpp`；`MergePlanner` 只通过 `FeatureBoundedRegionBuilder` 生成 `FeatureBoundedRefit` 候选；CandidateFilters、MainWindow、Viewer、ModelTree、Face Inspect 和候选统计测试已同步到 FeatureBoundedRefit / Unknown 当前主线。
79. Patch preview run log 已完成第一版：GUI 每次一键 Patch preview 都会在仓库根目录 `log/` 下创建 `patch_preview_<timestamp>_candidate_<id>.log`。该日志记录 output path 解析、STP sampled / source STL fitting mesh 生成、STL 写出、`RunningGeomagic` 调用、patch import、viewer overlay 和 crop boundary diagnostics 的 elapsed / duration；Process Status、阶段事件、失败报告和状态栏同步显示 root run log 或 elapsed。`fit_region.log` 仍只代表 Geomagic Wrap 脚本内部步骤；root run log 用于判断 GUI 卡在 Geomagic backend 等待、import，还是后处理。
80. STEP export background flow 已完成：`MainWindow::exportStepFile()` 使用 `QFutureWatcher<ExportStepUiResult>`，文件选择仍在 GUI 线程，`AppController::exportStepFile()` 与 `verifyStepFileReadable()` 在 worker 执行；完成后回 GUI 线程更新 report / log / status。`ProcessStage::ExportingStep` 用于区分 STEP/STP 导出后台状态，避免导出和二次读取校验期间主窗口未响应。
81. Commercial-CAD-like A0 sampling report 已补齐：`CommercialCadQualityGateReport` 现在输出 `sampling_report`，记录 boundary / feature-edge samples per edge、corner anchor source、boundary sample count、corner anchor count、feature boundary edge count、feature edge sample count，以及 FeatureEdgeDetectionResult 是否参与评估；`corner_baseline_probe` 的 baseline JSON 同步输出该字段。此项只增强 A0 报告可重复性，不改变 gate 阈值、不改变 pass/fail 语义、不实现 B1/B2/B3 fitting input 增强。
82. 自动 baseline / probe 工具构建覆盖已补齐：Cleanup-6 删除旧 analytic `MergePlannerOptions` 字段后，`corner_baseline_probe` 和 `patch_apply_probe` 仍保留旧字段赋值，导致这两个工具单独构建失败；现已改为只设置当前 `FeatureBoundedRefit` 相关选项，并把两个工具纳入 `scripts/verify_spo.ps1` 默认构建。注意这仍是 CLI 自动 baseline/probe，不是窗口点击级 GUI 自动化。
83. 脚本化 A0 baseline gate 已补齐：新增 `scripts/run_corner_baseline_gate.ps1`，用于构建并运行 `corner_baseline_probe`，支持 `-CandidateId auto`、显式 `-Patch` 复用已有 patch、`-RealGeomagic` 触发真实 Geomagic、`-AllowQualityGateFailure` 保留已知 A0 质量门失败报告；`corner_baseline_probe` 现在会输出 `candidate_selection_mode` 和 `generated_candidate_count`。`verify_spo.ps1 -RealGeomagic` 会调用该 gate，但普通 `verify_spo.ps1` 不依赖真实 Geomagic。
84. B1 corner / feature edge 加密采样已完成第一版：`StpSampledFittingMeshBuilder` 新增显式 `enableCornerFeatureDenseSampling` 路径，默认 A0 不变；B1 会沿原 STP candidate outer boundary / feature-boundary edges 生成 dense samples 作为报告采样集，同时提高 STP-sampled fitting STL 的连接 surface grid 密度。真实 Geomagic 覆盖测试证明孤立 anchor micro facets 会让输入 STL 变成大量离散组件并导致 AutoSurface initialization failure，因此 B1 不再生成孤立锚点三角片。`corner_baseline_probe` / `run_corner_baseline_gate.ps1 -Experiment B1` 会输出 `corner_feature_dense_sampling_enabled`、`feature_edge_dense_sample_count`、`corner_anchor_sample_count` 和 `corner_feature_surface_division_count`。该项只改变 Geomagic fitting input，不改变 Apply、original STP boundary re-trim、redo 或 `StrictTopologyGate` 语义。
85. B2 原 STP boundary guard-band 外扩采样已完成第一版：`StpSampledFittingMeshBuilder` 新增显式 `enableBoundaryGuardBandSampling` 路径，默认 A0/B1 不变；B2 会在 B1 连接 surface grid 基础上沿原 STP candidate outer boundary 采样一圈外扩带，优先将 guard samples 投到相邻非候选 STP face，并用桥接三角形接回现有采样面，避免恢复 B1 早期孤立 micro-facet 问题。`corner_baseline_probe` / `run_corner_baseline_gate.ps1 -Experiment B2` 会输出 `boundary_guard_band_*` 统计字段；脚本默认使用保守参数 `GuardBandSamples=16`、`GuardBandRings=1`、`GuardBandSpacing=0.10`。真实样例 `03_配件_Clay.stp` candidate 179 的 B2 重新生成验证显示 fitting STL 仍为 `components=1`，Patch preview 可进入 apply 路径，boundary/feature max drift 从 B1 的约 `0.123518` 降到 `0.057990`，corner max drift 从约 `0.123518` 降到 `0.043502`；但 `StrictTopologyGate` 仍因 after BRepCheck/free edges 失败，`CommercialCadLikeQualityGate` 仍未通过。本阶段只证明 B2 外扩采样和报告链路落地，不宣称商业 CAD 门控已通过。
86. B2.1 over-cover strip 已完成基础版并完成真实样例验证：`StpSampledFittingMeshBuilder` 新增显式 `enableBoundaryOverCoverStrip` 路径，默认 A0/B1/B2.0 不变；B2.1 从当前生成的 fitting STL 三角网格提取真实 boundary loop，在其外围生成连续、小幅、连通的 over-cover strip；外扩方向来自原 CAD boundary pcurve / candidate surface 的外侧采样，并按局部 surface normal 修正 STL triangle winding，避免反法向 strip。`corner_baseline_probe` 支持 `--b2-over-cover-strip`、`--over-cover-width`、`--over-cover-rings`，`run_corner_baseline_gate.ps1 -Experiment B2.1` 会启用 B1 加密采样和 B2.1 over-cover，但不会启用 B2.0 邻接 STP face guard-band；默认 `OverCoverWidth=0.05`。默认测试覆盖单连通、单 boundary cycle、bbox 外扩、normal/winding、report 字段和脚本/CLI 合同。`corner_baseline_probe` 现在在 Patch Apply + StrictTopologyGate 通过后导出 applied STEP 并立即二次读取，baseline JSON 输出 `applied_step_export`。真实样例 `03_配件_Clay.stp` candidate 179 默认 B2.1：fitting STL 导入 Geomagic 时为 `components=1`、`boundaryCycles=1`、`nonManifoldVertices=0`、`degenerateTriangles=0`，`boundary_over_cover_sample_count=728`、`boundary_over_cover_triangle_count=1456`、`fallback=0`、`rejected=0`、`coverage=1`；AutoSurface 输出 `patch_face_count=5`、`patch_edge_count=20`、preview 非 high-risk；Patch Apply 成功，`StrictTopologyGate` 通过，applied STEP export/readback 成功；`CommercialCadLikeQualityGate` 仍失败，boundary / feature / corner max drift=0.088487。该阶段证明 B2.1 输入和 Apply/export 验收链路落地，但几何质量尚未收口。
87. B2.2 adjacent-face support collar 已完成最小实现并完成真实样例参数扫：`StpSampledFittingMeshBuilder` 新增显式 `enableAdjacentFaceSupportCollar` 路径，默认 A0/B1/B2.0/B2.1 不变；B2.2 使用当前 fitting STL mesh boundary 作为内环，外侧 support rail 来自原 STP 非候选邻接 face 的 pcurve 面内采样，目标是给 Geomagic 提供 seam 高度上下文而不是继续扩大 B2.0 guard-band。`corner_baseline_probe` 支持 `--b2-adjacent-face-support-collar`、`--support-collar-width`、`--support-collar-samples`、`--support-collar-rings`，`run_corner_baseline_gate.ps1 -Experiment B2.2` 默认启用 B1 加密采样和 B2.2 collar，不启用 B2.0 guard-band 或 B2.1 over-cover。`CommercialCadLikeQualityGate` 新增 `seam_continuity`，测量原 boundary samples 到 patch 最近点在原 STP 邻接 face normal 方向上的 signed normal offset，专门暴露边界高低差。默认测试覆盖 B2.2 单连通、单 boundary cycle、normal/winding、report 字段和脚本/CLI 合同。真实样例 `03_配件_Clay.stp` auto-selected candidate 179 的 B2.2 初版：collar sample=728、triangle=1456、fallback=0、rejected=0、coverage=1，Geomagic 输入仍为 `components=1`、`boundaryCycles=1`、`nonManifoldVertices=0`，AutoSurface 输出 5-face patch 且非 high-risk；但 `SupportCollarWidth=0.03/0.04/0.05` 均未通过 `StrictTopologyGate` / `CommercialCadLikeQualityGate`。其中 `0.05` 的 boundary max/p95=0.090959/0.041232、seam max_abs/p95_abs=0.069443/0.040150、corner max/p95=0.090959/0.057764，Gate 因 STEP roundtrip 后 1 条 free edge 失败。本阶段只能证明 B2.2 输入结构和 seam 指标落地，不能宣称几何质量收口。
88. B2.3 corner-safe support collar 与 SharpenContours A/B 已完成最小实现并完成真实样例验证：`StpSampledFittingOptions` 新增 `enableAdjacentFaceSupportCollarCornerClamp`、`adjacentFaceSupportCollarCornerSmoothingIterations`、`adjacentFaceSupportCollarMaxOffsetScale`，默认 A0/B1/B2.0/B2.1/B2.2 不变；B2.3 保留 B2.2 adjacent-face support rail，但对当前 fitting STL mesh boundary 的角点、support offset direction jump 和超出目标宽度的 offset 执行局部平滑与 clamp，避免角点处生成异常凸起。`corner_baseline_probe` 支持 `--b2-corner-safe-support-collar`、`--support-collar-max-offset-scale` 和 `--geomagic-sharpen-contours`，`run_corner_baseline_gate.ps1 -Experiment B2.3` 默认启用 B1 + adjacent-face support collar + corner-safe clamp，`-SharpenContours` 是显式 A/B 开关且默认关闭。JSON 新增 `adjacent_face_support_collar_corner_clamp_enabled`、`adjacent_face_support_collar_corner_clamp_count`、`adjacent_face_support_collar_max_offset` 和根字段 `geomagic_sharpen_contours`；Geomagic backend 将其转成 `FIT_REGION_SHARPEN_CONTOURS`，脚本写入 `sharpenConstrainedContours`。默认测试覆盖 B2.3 单连通、单 boundary cycle、normal/winding、max offset clamp、report 字段、脚本/CLI 合同和 Geomagic backend 环境变量。真实样例 `03_配件_Clay.stp` auto-selected candidate 179：B2.3 默认 `SupportCollarWidth=0.05` 的 clamp count=16、max offset=0.0625、boundary/corner/feature max drift=0.095701，`StrictTopologyGate` 因 `FreeEdgeIncreased` 失败；B2.3 + `SharpenContours` 的 max drift 降到 0.070672、p95=0.038436，但 `StrictTopologyGate` 仍因 `FreeEdgeIncreased` 失败。由于 gate 未通过，本轮没有 applied STEP；这是严格门控阻止提交，不是导出链路缺失。
89. B2.6 Local Free-edge Closure Fix 已完成最小诊断、repair best-result 退化惩罚和 validator 分类修正版：`PatchReplacementReport` 增加 pre/post degenerated free edge count、appeared-after-repair degenerated free edge count 和 best sewing degenerated free edge count；每条 free edge diagnostic 输出 length、tolerance、degenerated、nearest original boundary edge 的 length/tolerance/parameter range/endpoints/midpoint/adjacent old face/surface type/pcurve availability、same-edge split segment count、owner switch count、degenerated split segment count、nearest split segment 和 fitted patch projection distance 分布。`ShapeValidator` 的 free-edge 统计现在排除 closed seam edge 与 OCCT degenerated edge，普通 open face 仍计为 free edge；这是修正拓扑分类，不放宽 `StrictTopologyGate`。真实样例 `03_配件_Clay.stp` auto-selected candidate 179 复用 B2.3 + SharpenContours patch 后，报告为 `data\baseline_runs\scripted_b2_6_apply_reuse_b2_3_sharpen_w005_auto_free_edge_gate_fix\baseline_report.json`；`StrictTopologyGate` 通过，`gate_after_free_edges=0`、`gate_roundtrip_free_edges=0`、`gate_step_roundtrip_ok=true`，applied STEP write/readback 成功。repair 后仍有 1 条点状/退化 diagnostic edge（`length=0`、`tolerance=1e-7`、`appeared_after_repair=true`），但不再被视为真实 open boundary。当前结论是拓扑 gate 已收口，`CommercialCadLikeQualityGate` 仍因 boundary/corner/feature drift 约 0.070672 未通过。
90. B2.7 Trim / over-cover / seam failure localization 已完成最小诊断版：新增 `PatchTrimDiagnostics` 并接入 `PatchReplacementCommand`，`PatchReplacementReport`、`corner_baseline_probe` baseline JSON、`patch_apply_probe` 和 GUI Apply report 均输出 `trim_diagnostics`。字段覆盖 replacement face count、trim wire invalid count、over-cover / under-cover sample count、boundary gap max/p95/RMS、internal seam gap max/p95/RMS、worst boundary/internal edge id、roundtrip compared/changed。默认测试使用 synthetic fixture 覆盖 matching replacement、over-cover、under-cover、internal seam 和无真实路径硬编码；诊断实现有采样上限，避免真实样例全量距离计算超时。真实样例 `03_配件_Clay.stp` auto-selected candidate 179 复用 B2.3 + SharpenContours patch 后，报告为 `data\baseline_runs\scripted_b2_7_apply_reuse_b2_3_sharpen_w005_auto_trim_diagnostics\baseline_report.json`，applied STEP 为 `data\baseline_runs\scripted_b2_7_apply_reuse_b2_3_sharpen_w005_auto_trim_diagnostics\03_配件_Clay_candidate_0179_applied.stp`；`StrictTopologyGate` 通过、applied STEP readback 通过、`CommercialCadLikeQualityGate` 仍失败。B2.7 判读为：over-cover=0，under-cover=81/256、max=2.03433，boundary_gap max/p95/RMS=0.0706854/2.38984e-14/0.0118242，internal_seam_gap=0，roundtrip_changed=false；当前主因更偏向局部 surface coverage / boundary projection / owner split 稳定性，而不是 STEP roundtrip 破坏或大面未裁掉。
91. B2.8 External CAD Diagnostic Routing 已完成最小路由版：新增 `PatchExternalCadDiagnosticsReport` 并挂入 `PatchReplacementReport`，`PatchReplacementCommand` 在输入校验后记录原始 Geomagic 补片 STEP 的 `raw_patch_preflight_*` 路由，Gate 失败时跳过最终外部 CAD 诊断，Gate 通过但尚未导出 STEP 时标记 `WaitingForAppliedStepExport`。`corner_baseline_probe` 会在已合并 STEP 导出和二次读取后更新 `final_applied_step_diagnostic_*` 字段，并把 `external_cad_diagnostics` 写入 baseline JSON；`patch_apply_probe` 和 GUI Apply report 同步显示摘要。B2.8 明确区分补片预诊断与最终合并模型诊断：原始补片 STEP 只能用于判断 Geomagic 输出自身是否可导入或是否有补片缺陷，最终外部 CAD 诊断只能接 Apply 成功后导出的已合并 STEP。真实样例报告为 `data\baseline_runs\scripted_b2_8_apply_reuse_b2_3_sharpen_w005_auto_external_cad_route\baseline_report.json`，已合并 STEP 为 `data\baseline_runs\scripted_b2_8_apply_reuse_b2_3_sharpen_w005_auto_external_cad_route\03_配件_Clay_candidate_0179_applied.stp`；`StrictTopologyGate` 通过、已合并 STEP 导出和二次读取通过、`CommercialCadLikeQualityGate` 仍失败，`final_applied_step_diagnostic_eligible=true`、`final_applied_step_diagnostic_status=PendingExternalRunner`。默认测试只覆盖字段和路由，不调用 Creo / Geomagic 外部 API，不依赖真实样例路径，也不改变 `PatchReplacementRepair`、`StrictTopologyGate` 或 redo 语义。
92. B2.8 后台 Creo 检查入口已跑通并输出真实诊断：`scripts\run_creo_step_diagnostic.ps1` 显式读取 applied STEP，使用 Creo Distributed Batch 的 `-nographics -process` 路线先执行 `step_3d_import.ttd` 导入 `.stp/.step` 到 `.prt`，只有生成 `.prt` 后才继续执行 `modelcheck.ttd`。脚本输出 `creo_step_diagnostic_result.json`、`.dxc`、stdout/stderr、`generated_prt`、`prt_path`、`modelcheck.report_files`，并解析 ModelCHECK XML 到 `modelcheck.summary`：`diagnostic_passed`、pass/info/warning/error count、`key_checks`、`failed_checks`、`warning_checks` 和 `import_validation`。默认测试只做静态契约检查，不启动 Creo、不依赖本机安装、不写死真实样例。真实样例 B2.8 已合并 STEP 的有效结果为 `data\baseline_runs\creo_b2_8_modelcheck_probe\scripted_runner_modelcheck_summary\creo_step_diagnostic_result.json`：`status=CreoModelCheckReportReady`、`success=true`、`generated_prt=true`、`prt_path=...\step_import\input.prt.1`、`modelcheck.summary.parsed=true`、`modelcheck.diagnostic_passed=false`；ModelCHECK 统计为 PASS=26、INFO=33、WARNING=3、ERROR=2，失败项为 `GEOM_CHECKS` answer=1 和 `SHORT_EDGES` answer=1491，导入校验为 `PTC_VAL_IMP_PART_STATUS=SOLID_FAILED`、`PTC_VAL_IMP_SCORE=FAIL`、`PTC_MP_VAL_IMP_AREA=1074.923639`。这说明后台诊断链路已可用，但当前已合并 STEP 没有通过 Creo 检查。
93. B2.9 Creo ModelCHECK 诊断关联与短边定位已完成最小解析/关联版：`scripts\run_creo_step_diagnostic.ps1` 现在会展开 ModelCHECK XML 的 `check.item` 明细，保留 `info1/info2/raw_text`，并从文本中提取可用的 Creo `creo_edge_id` / `creo_feature_id`。结果 JSON 新增 `creo_diagnostic_correlation`：汇总 failed checks 的 name/status/answer/item_count、`SHORT_EDGES` item 摘要、导入 feature id、`import_validation`、报告文件路径，以及可选 `-BaselineReportPath` 指向的 B2.7/B2.8 baseline 中 `trim_diagnostics` 阶段指标。脚本新增 `-ParseModelCheckOnly` 和 `-ModelCheckXmlPath`，默认测试通过 synthetic XML fixture 验证解析和 JSON 合同，不启动 Creo、不依赖真实样例。真实文件辅助验证复用 B2.8 XML 与 baseline JSON，得到 `short_edge_item_count=1491`、`imported_feature_ids=[4]`，并阶段级关联到 `under_cover_sample_count=81`、`under_cover_max_distance=2.03433`、`boundary_gap_max=0.0706854`、`worst_boundary_edge_id=1540`。当前 XML 只给 Creo feature/edge id，不给空间坐标或 OCCT edge id，因此 `modelcheck_spatial_mapping_status=CreoIdsOnlyNoCoordinates`、`occt_edge_mapping_available=false`；后续若要空间定位，必须通过 Creo API 或导出失败选区/几何报告获取坐标后再与 B2.7 owner/split/边界缝隙诊断比对。
```

其中，`MergePatchCommand` 的撤销语义当前定义为：

```text
撤销合并曲面片时，取消合并后产生或保留的锁边状态。
```

这是一个明确设计决策，不再作为待修复问题记录。

---

## 3. 已实现功能清单

### 3.1 GUI 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| 主窗口布局 | 已完成 | 包含模型树、3D Viewer、参数面板、日志/检查/验证/报告区域 |
| STEP 模型显示 | 已完成 | 基于 OCCT Viewer |
| 鼠标中键旋转视角 | 已完成 | 支持基础视角浏览 |
| 鼠标滚轮缩放 | 已完成 | 支持放大 / 缩小 |
| face 选择 | 已完成 | 支持面选择模式 |
| edge 选择 | 已完成 | 支持边选择模式 |
| Shift 多选 face/edge | 已完成 | 支持多选与 toggle |
| Ctrl 点击移除选择 | 已完成 | 支持从选择集中移除 |
| 右键菜单 | 部分完成 | edge 模式已接入锁边/解锁；face/candidate 菜单多为预留 |
| 特征线显示 | 已完成基础版 | 显示自动检测出的特征边 |
| 锁定边显示 | 已完成 | 锁定边以高亮方式显示 |
| 合并候选预览 | 已完成基础版 | 后端 MergePlanner 已接入，可高亮候选区域 |
| 候选区域点击选择 | 已完成基础版 | 选择候选区域模式下点击面片可选中所属候选 |
| 候选区域接受/拒绝/隐藏/恢复 | 已完成基础版 | 管理运行时候选状态，不修改 B-rep |
| Crop Boundary Diagnostics overlay | 已完成基础版 | Patch preview/import 成功后显示原 STP boundary、patch outer boundary、STL coverage issue、boundary-band missing area、patch mismatch 和 near-boundary rejected triangles 诊断线；crop bbox 默认隐藏 |
| undo/redo 按钮 | 已完成 | 已接入 Ctrl+Z / Ctrl+Y |
| GUI 手动验证 | 已完成 | 当前主流程手动验证通过 |

### 3.2 AppController 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| 打开 STEP/STP | 已完成 | `openStepFile` |
| 导出 STEP | 已完成 | `exportStepFile` |
| 导出后二次读取校验 | 已完成 | `verifyStepFileReadable` |
| 特征边检测 | 已完成基础版 | `detectFeatureEdges` |
| same-domain 合并 | 已完成 | `unifySameDomain` |
| 合法性检查 | 已完成基础版 | `validateShape` |
| 锁边 / 解锁边 | 已完成 | `lockEdges` / `unlockEdges` |
| undo / redo | 已完成 | `undo` / `redo` |
| undo/redo 状态查询 | 已完成 | `canUndo` / `canRedo` |
| Crop Boundary Diagnostics | 已完成基础版 | `diagnoseCropBoundaryForCurrentPatch` / `diagnoseCropBoundaryData` 输出 T6.6.1 + T6.6.3 report，包含 boundary-band coverage 与 source triangle audit，不修改文档 |

### 3.3 Command 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `Command` 基类 | 已完成 | 支持 `execute`、`undoable`、`undo`、`redo` |
| `CommandContext` | 已完成基础版 | 保存当前文档、检测结果、验证报告、锁边状态等 |
| `CommandHistory` | 已完成 | 维护 undoStack / redoStack / executedCommands |
| `LoadStepCommand` | 已完成 | 加载 STEP；不可撤销 |
| `DetectFeatureCommand` | 已完成基础版 | 检测特征边；不可撤销 |
| `ValidateShapeCommand` | 已完成基础版 | 生成验证报告；不可撤销 |
| `ExportStepCommand` | 已完成 | 导出 STEP；不可撤销 |
| `MergePatchCommand` | 已完成基础版 | 支持 same-domain 合并和 undo/redo |
| `LockEdgeCommand` | 已完成 | 支持多边锁定和 undo/redo |
| `UnlockEdgeCommand` | 已完成 | 支持多边解锁、document 校验和 undo/redo |
| `LockedEdgeRef` | 已完成 | 保存锁边几何签名，用于合并后边 ID 重映射 |

### 3.4 IO 模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| STEP 读取 | 已完成 | 支持读取 `.step` / `.stp` |
| STEP 写出 | 已完成 | 支持导出 STEP；GUI 导出路径在后台执行 |
| STEP 二次读取验证 | 已完成 | 导出后可重新读取验证；GUI 导出后的二次读取校验在同一后台任务执行 |
| 项目文件保存 | 未完成 | 能力待实现；不保留空占位类型 |
| 项目文件恢复 | 未完成 | 需要保存锁边、参数、操作日志等 |

### 3.5 B-rep 拓扑模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `ShapeDocument` | 已完成基础版 | 保存 shape、source path、stats、topology |
| `FaceIndex` | 已完成基础版 | face 编号与查询 |
| `EdgeIndex` | 已完成基础版 | edge 编号与查询 |
| `TopologyGraph` | 已完成基础版 | 支持 face/edge 查询、邻接关系、面边关系 |
| face/edge GUI 命中查询 | 已完成 | 支持从 TopoDS_Shape 反查 ID |
| 面属性统计 | 待增强 | 面积、中心点、平均法向、曲面类型等仍可扩展 |
| 边属性统计 | 待增强 | 长度、曲线类型、曲率等仍可扩展 |

### 3.6 特征线模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| sharp edge 检测 | 已完成基础版 | 基于二面角阈值 |
| free edge 检测 | 已完成基础版 | 用于识别开口边 |
| multiple edge 检测 | 已完成基础版 | 用于识别异常拓扑边 |
| min edge length 过滤 | 已完成基础版 | 可过滤过短边 |
| 用户锁边 | 已完成 | 通过 `CommandContext.lockedEdges` 维护 |
| 用户锁边参与合并保护 | 已完成 | 合并时加入 protectedEdges，并已有测试覆盖 |
| 曲率特征线 | 未完成 | ridge / valley / weak feature 仍待实现 |
| 圆角起止线识别 | 未完成 | 边界分类能力待实现；不保留空占位类型 |
| 曲面类型变化识别 | 未完成 | 后续可结合 B-rep surface type |

### 3.7 合并模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| `SameDomainUnifier` | 已完成基础版 | 当前主合并能力 |
| 自动特征边保护 | 已完成 | sharp/free/multiple edges 进入保护边 |
| 用户锁边保护 | 已完成 | locked edges 进入 protectedEdges |
| 用户锁边保护测试 | 已完成 | 已补充 protectedEdges 测试 |
| 合并前后统计 | 已完成基础版 | before/after stats |
| 合并撤销/重做 | 已完成 | 通过 `MergePatchCommand` 快照实现 |
| 合并撤销时清空锁边 | 已确认 | 当前定义为正确交互语义 |
| 锁边重映射 | 已完成基础版 | 通过 `LockedEdgeRef` 几何签名尝试映射 |
| `MergeCandidate` | 已完成基础版 | 支持候选类型、风险、face/edge 集合、统计指标和运行时状态 |
| `MergePlanner` | 已完成基础版 | 基于特征边和锁边生成 FeatureBoundedRefit 候选区域 |
| `MergeRegionGrower` | 已删除 | Cleanup-6 删除旧 analytic candidate grower；当前由 `FeatureBoundedRegionBuilder` 承担候选区域构建 |
| `MergeCandidate` 预览 | 已完成基础版 | GUI 可预览 Top N、全部非隐藏候选和指定候选 |
| 候选状态管理 | 已完成基础版 | Pending / Accepted / Rejected / Hidden，仅运行时保存 |
| Face / Candidate Inspect | 已完成基础版 | 点击 face 可查看 surface type、候选归属、candidate type/status/risk/fit_error 等信息 |
| Analytic primitive candidate detection | 已删除 | Cleanup-6 删除旧 PlaneLike / SphereLike / CylinderLike / ConeLike / TorusLike / Freeform 候选检测 |
| Candidate Type Preview | 已完成基础版 | 当前只支持 FeatureBoundedRefit / Unknown 统计、按类型筛选、Viewer 配色、模型树同步和点击查看 |
| RegionMergeResult / RegionMergeOptions | 已删除 | Cleanup-5 删除；`RegionBoundaryAnalyzer` 已切换到 `BoundaryAnalysisFailureReason` |
| Plane/Sphere RegionMerger | 已删除 | Cleanup-4 删除 Plane / Sphere 后端和后端测试；旧候选类型、检测、过滤和显示通道已在 Cleanup-6 删除 |
| `PlaneRegionMerger` | 已删除 | Cleanup-4 删除旧 Plane 后端 |
| 平面候选批量合并 | 已删除 | 旧 Plane 真实合并入口 / Command / 后端均已删除 |
| 平面合并边界简化 | 已删除 | 随 `PlaneRegionMerger` 后端在 Cleanup-4 删除 |
| `PlaneRegionMergeCommand` | 已删除 | Cleanup-3 删除旧 Plane command 入口 |
| `PlaneRegionBatchMergeCommand` | 已删除 | Cleanup-3 删除旧 Plane batch command 入口 |
| Stage 3-S Shared Primitive Fields | 已删除历史实现 | Cleanup-5 删除旧 result/options 类型；只保留历史记录 |
| `SphereRegionMerger` | 已删除 | Cleanup-4 删除旧 Sphere 后端；SphereLike 候选过滤 / 统计已在 Cleanup-6 删除 |
| `SphereRegionMergeCommand` | 已删除 | Cleanup-3 删除旧 Sphere command 入口 |
| `SphereRegionBatchMergeCommand` | 已删除 | Cleanup-3 删除旧 Sphere batch command 入口 |
| `CylinderRegionMerger` | 已删除 | Cleanup-1 删除 stub-only 后端；CylinderLike 候选检测 / 显示通道已在 Cleanup-6 删除 |
| `ConeRegionMerger` | 已删除 | Cleanup-1 删除 stub-only 后端；ConeLike 候选检测 / 显示通道已在 Cleanup-6 删除 |
| `TorusRegionMerger` | 已删除 | Cleanup-1 删除 stub-only 后端；TorusLike 候选显示通道已在 Cleanup-6 删除 |
| `SurfaceRefitter` | 已完成 NSR-1.1 诊断与三档实验 | 已实现原始边界 G0 + 内部 STEP 点拟合、全部失败门、样本级 JSON/CSV/OBJ 和固定三档对照；所有真实档位仍为失败诊断结果，未接 Apply 或 GUI |

### 3.8 验证模块

| 功能 | 状态 | 说明 |
|---|---:|---|
| shape 是否存在 | 已完成 |
| BRepCheck | 已完成基础版 |
| stats 统计 | 已完成 |
| free edge 数量 | 已完成 |
| multiple edge 数量 | 已完成 |
| 导出后二次读取验证 | 已完成 |
| max deviation | 未完成 |
| mean / RMS deviation | 未完成 |
| feature preserve rate | 未完成 |
| locked edge preserve rate | 未完成 |
| 实验报告生成 | 未完成 |

### 3.9 测试模块

| 测试内容 | 状态 | 说明 |
|---|---:|---|
| STEP IO 测试 | 已完成基础版 |
| TopologyGraph 测试 | 已完成基础版 |
| FeatureEdge 测试 | 已完成基础版 |
| SameDomainMerge 测试 | 已完成基础版 |
| Validation 测试 | 已完成基础版 |
| CommandHistory 测试 | 已完成 |
| LockEdgeCommand 测试 | 已完成 |
| UnlockEdgeCommand 测试 | 已完成 |
| UnlockEdgeCommand 无文档校验测试 | 已完成 |
| MergePatchCommand undo/redo 测试 | 已完成 |
| 用户锁边进入 protectedEdges 测试 | 已完成 |
| MergePlanner / FeatureBoundedRegionBuilder 测试 | 已完成基础版 | 覆盖 FeatureBoundedRefit 候选生成、protectedEdges / locked edge 阻断、最小区域 face 数和预览不改模型 |
| MergeCandidate 状态测试 | 已完成基础版 | 覆盖 Pending 默认状态、状态切换、Hidden 过滤和 stats 不变 |
| RegionMerger stub 测试 | 已删除 | Cleanup-1 随 stub-only Cylinder/Cone/Torus 后端删除；不再编译 `tests/test_region_merge_stubs.cpp` |
| PlaneRegionMerger 测试 | 已删除 | Cleanup-4 随旧 Plane 后端删除 |
| PlaneRegionMergeCommand 测试 | 已删除 | Cleanup-3 随旧 Plane command 入口删除 |
| PlaneRegionBatchMergeCommand 测试 | 已删除 | Cleanup-3 随旧 Plane batch command 入口删除 |
| Face / Candidate Inspect 测试 | 已完成基础版 | 覆盖 surface type、候选归属、NotInCandidate 和 stats 不变 |
| Analytic Candidate Detection 测试 | 已删除 | Cleanup-6 删除旧 analytic candidate detection 测试，不再编译 `tests/test_analytic_candidate_detection.cpp` |
| Candidate Type Statistics 测试 | 已完成基础版 | 覆盖 FeatureBoundedRefit / Unknown 统计、Hidden 过滤和按类型筛选 |
| SphereLike 一键合并过滤测试 | 已删除 | SphereLike 类型和过滤路径已在 Cleanup-6 删除 |
| Geomagic backend mock 测试 | 已完成 | 覆盖 mock success/failure/timeout、缺输入、缺脚本、缺输出、result JSON 缺失但 STEP 存在的真实脚本兜底 |
| Geomagic pipeline script 静态契约测试 | 已完成 | 覆盖 FIT_REGION 输入输出、RepairMesh、RemoveNonManifoldVertices、FillSmallHoles、Mechanical 默认、无 result JSON 输出契约 |
| PatchImportService 测试 | 已完成 | 覆盖 STEP/STP/IGS/IGES 导入、bbox/face/edge/BRepCheck 统计、从 Geomagic result 导入、失败不修改 ShapeDocument |
| PatchImportService 真实文件测试 | 已完成 | 默认导入 data/crop_stp/data/crop_igs 中已有文件；`SPO_ENABLE_REAL_GEOMAGIC_TESTS=1` 时跑通真实 wrapCore.exe 链路 |
| Patch Apply / Controller Apply 测试 | 已完成 | 覆盖 NotGenerated、ApplyBlocked、PreviewHighRisk、PreviewReady、multi-face 不阻塞、clear 重置、无 preview Apply 失败、preview/candidate mismatch 阻断、真实 Apply 通过 CommandHistory 成功提交、失败保留 document 与 preview state、成功清 preview state、undo/redo、multi-face 进入 Apply path、open STEP / undo / redo / SameDomain 合并后的旧 preview 清理；T6.5.1 增加 PatchReplacementReport gate diagnostics、GUI report 诊断标签、STEP roundtrip stats 和 watertight stats 断言；T6.6 增加 selected sewing tolerance、adaptive sewing attempt count、best sewing stats、GUI report 标签和 redo 不重跑 repair 断言 |
| MultiFacePatchAnalyzer / PatchReplacement input validation 测试 | 已完成 | 覆盖 empty patch、one-face patch、multi-face box patch、compound patch、缺失输入、invalid boundary、multi-face accepted 和 highRisk rejected |
| StrictTopologyGate 测试 | 已完成 | 覆盖 identical valid box、missing after shape、free edge increase、multi-face replacement accepted / disallowed、face count non-reduction warning、STEP export + roundtrip、严格 watertight solid gate、roundtrip 后 BRepCheck / solid count / zero free edge / zero multiple edge 统计 |
| BoundaryConstrainedPatchBuilder 测试 | 已完成 | 覆盖 one-face fragment、multi-face box fragment、empty analysis、invalid boundary、synthetic patchFaceCount=12、internal seam retention 和 ShapeDocument 不变 |
| PatchReplacementRepair / T6.6 测试 | 已完成 | 覆盖 adaptive sewing 多 tolerance 尝试、preferred tolerance 优先、collapsed result 只作为诊断、closed shell 的 shell-to-solid path、Command report 字段、multi-face 不 unsupported、GateFailed rollback、redo 不重跑 repair；T6.5.1 的 rejected after debug artifact 仍未实现，若后续需要可补为可选诊断 artifact |
| T6.6.1 Crop Boundary Diagnostics 测试 | 已完成 | 覆盖原 STP boundary loop 采样、STL crop 最近距离覆盖检查、imported patch outer boundary 覆盖检查、suspected gap segment 报告、无 local STL 的 patch-only 诊断、GUI overlay 标签和禁止真实样例路径硬编码 |
| T6.6.2 Process Status Panel 测试 | 已完成 | 覆盖状态阶段字符串、PreviewReady、Apply success/failure 后参数保留、GateFailed 后 repair/Gate 诊断保留、undo/redo 显示 CachedUndo / CachedRedo 且不显示 Geomagic / crop / import / repair 重跑 |
| Patch preview run log 测试 | 已完成 | 覆盖 root `log/` 自动创建、时间戳 + candidate 文件名、elapsed_ms / duration_ms 和阶段消息写入 |
| STEP export background 测试 | 已完成 | 源码级回归覆盖 `ExportingStep`、`QFutureWatcher<ExportStepUiResult>`、后台导出文案和导出完成回 GUI 线程更新路径 |
| T6.6.3 STL Crop Boundary-Band Diagnostics 测试 | 已完成 | 覆盖 boundary point coverage 正常但 boundary-band coverage 报警、centroid-only 拒绝近边界三角片但 conservative criteria 命中、report/GUI 字段存在性和禁止真实样例路径硬编码 |
| T6.6.4 STL Region Extractor conservative crop / mode switch 测试 | 已完成 | 覆盖默认 centroid-only、显式 ConservativeBoundaryBand 下 centroid outside but vertex inside、edge midpoint inside、boundary-band inclusion、crop report keep/reject 字段和正式测试不调用 Geomagic |
| T6.6.5 GUI A/B 验证 | 已完成 | 验证 centroid-only 与 conservative-boundary-band 两条 crop mode 都不能让 Geomagic patch outer boundary 成为可靠 CAD replacement boundary，后续转向 T6.7 surface re-trim |
| T6.6.4.2 boundary-loop connectivity guard 测试 | 已完成 | 覆盖连通 boundary-loop 修补可加入、不连通漂浮修补候选会被拒绝并计入 orphan repair candidates |
| T6.7 Boundary-Constrained Surface Re-trim / T6.7.4 Multi-surface Shell 测试 | 已完成 | 覆盖 original-boundary surface re-trim、multi-face patch 默认不再直接信任 patch outer boundary、无覆盖 surface 时 BuildFailed、单 surface 与 all-surface coverage report 字段、strict multi-surface shell 成功、缺少内部 seam 时 BuildFailed、attempted / split edge / closed wire / Command report 字段和 Gate/rollback 语义；真实大样例继续以 GUI Apply report 作为手动验证主证据，`patch_apply_probe` 作为辅助定位工具 |
| Geomagic stale output / Remesh env 测试 | 已完成 | 覆盖 backend 删除 stale STEP/IGS、脚本删除旧输出、Remesh 默认跳过、显式 Remesh 环境变量传递、Remesh 失败 warning fallback 和 AutoSurface 失败后的 pre-remesh retry |
| STP-sampled fitting mesh 测试 | 已完成 | 覆盖 planar face / multi-face candidate 采样、boundary samples、boundary band 开关、STL roundtrip、bbox、非退化三角片、report 字段和 `GeomagicFittingInputMode` 字符串 |
| STL Global Cut Chain cutter 测试 | 已完成基础版 | 覆盖空 mesh / 短 loop 失败、拓扑构建、三角形面积、polyline 长度和点到线段距离；真实复杂 STL 切链仍以 GUI 手动验证为主 |
| PatchArtifactLocator 中文路径测试 | 已完成 | 覆盖中文 relative dir / 中文 stem 下 local STL 到同名 STEP、autosurface IGS sidecar 和 fit_region log 的定位 |
| A0 baseline / Apply probe 工具构建 | 已完成 | `verify_spo.ps1` 默认构建 `corner_baseline_probe` 和 `patch_apply_probe`，避免 CLI baseline / probe 因接口漂移失效 |
| A0 baseline 脚本 gate | 已完成 | `run_corner_baseline_gate.ps1` 可复用已有 patch 或在 `-RealGeomagic` 下运行真实 Geomagic；candidate 可用 `auto` 发现式选择，报告写出 candidate selection 字段 |
| B1 corner / feature edge 加密采样测试 | 已完成基础版 | 覆盖默认 A0 不生成 B1 加密、显式 B1 增加 dense edge samples / corner anchors / connected surface divisions，并防止回退到孤立 micro-facet 组件；脚本源码级覆盖 `-Experiment B1` 与 `--b1-corner-feature-sampling` 转发 |
| B2.0 boundary guard-band 外扩采样测试 | 已完成基础版 | 覆盖默认 A0 不生成 B2 guard-band、显式 B2 增加 guard-band 样本/三角形、扩展 fitting STL bbox 且保持单连通组件；脚本源码级覆盖 `-Experiment B2` 与 `--b2-boundary-guard-band` 转发 |
| B2.1 fitting STL over-cover strip 测试 | 已完成基础版，真实默认验证已跑 | 覆盖围绕当前 fitting STL patch boundary 的连续窄带生成、bbox 外扩、单连通、单 boundary cycle、normal/winding、report 字段、默认 A0 不启用 B2.1，以及脚本/CLI `-Experiment B2.1` / `--b2-over-cover-strip` / applied STEP export 合同；真实 Geomagic candidate 179 默认参数生成 5-face patch，StrictTopologyGate 与 applied STEP readback 通过，但 CommercialCadLikeQualityGate 未通过 |
| B2.2 adjacent-face support collar 测试 | 已完成最小版，真实参数扫已跑 | 覆盖当前 fitting STL mesh boundary 到邻接 STP face support rail 的 collar 生成、单连通、单 boundary cycle、normal/winding、report 字段、默认 A0 不启用 B2.2、脚本/CLI `-Experiment B2.2` / `--b2-adjacent-face-support-collar` 合同，以及 `CommercialCadLikeQualityGate.seam_continuity` JSON 输出；真实 Geomagic candidate 179 的 B2.2 输入干净但 StrictTopologyGate / CommercialCadLikeQualityGate 仍失败 |
| B2.3 corner-safe support collar / SharpenContours 测试 | 已完成最小版，真实 A/B 已跑 | 覆盖 B2.2 collar 上的 corner-safe clamp、最大 support offset 上限、单连通、单 boundary cycle、normal/winding、report 字段、默认 A0 不启用 B2.3、脚本/CLI `-Experiment B2.3` / `--b2-corner-safe-support-collar` / `--support-collar-max-offset-scale` 合同，以及 Geomagic `FIT_REGION_SHARPEN_CONTOURS` 环境变量；真实 Geomagic candidate 179 显示 SharpenContours 可降低 drift，但 StrictTopologyGate 仍因 FreeEdgeIncreased 失败 |
| B2.4 Boundary Edge Rebuild + Local Closure Probe | 已完成最小版，真实 Apply 验证已跑 | Apply 侧增量，不是新的 fitting STL experiment：strict multi-surface boundary shell 会用原 STP boundary 3D curve 作为最终边界源，向对应 Geomagic fitted surface 显式投影并重建 pcurve / SameParameter；报告 pcurve rebuild attempt/success/failure、SameParameter failure、max deviation、failed edge ids；真实样例 27/27 pcurve rebuild 成功但 StrictTopologyGate 仍因 FreeEdgeIncreased 失败，说明剩余主因不是 pcurve 缺失，而是局部几何/拓扑闭合仍有 1 条 free edge |
| B2.5 Pre-repair Closure Probe | 已完成最小版，真实复用 patch 验证已跑 | 不替代 repair，不放宽 Gate；在 PatchReplacementRepair 前后分别输出 assembled replacement shape 与 repaired after shape 的 free edge / multiple edge / BRepCheck / topology stats，并把 free edge 定位到 midpoint、端点、邻接 face、original boundary edge / split segment / patch face owner。真实样例显示 pre-repair 已有 66 条 free edge，post-repair 剩 1 条且 `appeared_after_repair=true`，StrictTopologyGate 仍正确失败 |
| B2.6 Local Free-edge Closure Fix | 已完成最小诊断/repair selection/validator 分类修正版，真实样例拓扑 gate 已通过 | Apply 侧任务，不是新的 `-Experiment` fitting input：已输出 free edge length/tolerance/degenerated、nearest original boundary edge context、split-owner context、projection distance 和 repair degenerated free edge stats；`ShapeValidator` 排除 closed seam / degenerated edge 的 free-edge 误计数后，真实样例 `StrictTopologyGate` 与 applied STEP readback 通过，但 `CommercialCadLikeQualityGate` 仍因 drift 超限失败 |
| B2.7 Trim / over-cover / seam failure localization | 已完成最小诊断版，真实样例验证已跑 | 诊断任务，不是修复任务：`PatchTrimDiagnostics` 输出 face trim envelope、over-cover / under-cover samples、boundary seam、internal seam、roundtrip 对照和 worst edge 定位；默认 synthetic 测试覆盖 over-cover、under-cover、internal seam 和无真实路径硬编码。真实样例 `StrictTopologyGate` 与 applied STEP readback 通过，但 `CommercialCadLikeQualityGate` 仍失败；B2.7 指标为 over-cover 0/29、under-cover 81/256 max 2.03433、boundary gap max 0.0706854、internal seam gap 0、roundtrip changed=false |
| B2.8 External CAD Diagnostic Routing | 已完成最小路由版，真实样例验证已跑 | 诊断编排任务，不是修复任务：`external_cad_diagnostics` 输出原始补片预诊断角色、最终已合并 STEP 诊断资格、输入路径、执行状态和跳过原因。真实样例 `StrictTopologyGate` 与已合并 STEP 导出/二次读取通过，最终外部 CAD 诊断 eligible=true，但 `CommercialCadLikeQualityGate` 仍失败；默认测试不调用外部 CAD API |
| B2.8 后台 Creo 检查入口 | 已完成显式脚本入口，真实后台诊断已跑通但模型未通过 Creo 检查 | `scripts\run_creo_step_diagnostic.ps1` 通过 Creo Distributed Batch 无图形模式先导入 STEP 到 `.prt`，再运行 ModelCHECK，并把 XML 摘要写入 JSON。真实样例已生成 `input.prt.1`、`input.p.html`、`input.p.xml`；`diagnostic_passed=false`，ModelCHECK 统计 PASS=26、INFO=33、WARNING=3、ERROR=2，失败项为 `GEOM_CHECKS=1` 与 `SHORT_EDGES=1491`，导入校验为 `SOLID_FAILED` / `FAIL` |
| B2.9 Creo ModelCHECK 诊断关联 | 已完成最小解析/关联版，真实 XML 辅助验证已跑 | `run_creo_step_diagnostic.ps1` 展开 ModelCHECK `item` 明细，输出 `creo_diagnostic_correlation`、短边 item 摘要、导入 feature id、import validation、报告路径和可选 baseline `trim_diagnostics` 阶段关联；默认测试只跑 XML fixture / parse-only，不启动 Creo。真实 XML 只含 Creo id，无空间坐标，暂不能直接映射 OCCT edge |
| NSR-1.1 失败诊断与固定三档实验 | 已完成 | 覆盖兼容首失败项与全部 `failed_gates`、1050 条 boundary/corner/interior 样本 JSON/CSV、超限向量 OBJ、失败诊断 patch 语义，以及 synthetic 多失败门和文件字段合同；candidate 179 三档只改变 training=24/48/96 |
| AppController 打开新文档清历史测试 | 已完成 |
| GUI 自动化测试 | 部分完成 | GUI 同源核心 pipeline 已有脚本 gate；窗口点击级 Qt/系统事件自动化仍未完成 |
| GUI 手动验证 | 已完成 | 当前主流程手动验证通过 |

---

## 4. 当前已完成的关键工程闭环

### 4.1 锁边保护闭环

```text
边选择模式
→ Shift 多选边
→ 右键锁定选中边
→ 锁边高亮显示
→ 执行 same-domain 合并
→ 用户锁定边进入 protectedEdges
→ 合并尽量不跨越锁定边
```

### 4.2 undo/redo 闭环

```text
执行可撤销命令
→ 命令进入 undoStack
→ Ctrl+Z 撤销
→ 命令进入 redoStack
→ Ctrl+Y 重做
```

当前可撤销命令：

```text
1. MergePatchCommand
2. LockEdgeCommand
3. UnlockEdgeCommand
```

当前不可撤销命令：

```text
1. LoadStepCommand
2. DetectFeatureCommand
3. ValidateShapeCommand
4. ExportStepCommand
```

### 4.3 STEP 处理闭环

```text
打开 STEP
→ 显示模型
→ 检测特征边
→ 锁定关键边
→ 执行合并
→ 合法性检查
→ 导出 STEP
→ 二次读取校验
```

### 4.4 Geomagic patch 生成 / 导入闭环

```text
默认路线：
当前 STP candidate faces / boundary
→ STP Sampled Candidate Surface
→ data/crop_stl/<step文件stem>/<step文件stem>_candidate_<id>.stl
→ wrapCore.exe --script scripts/geomagic_wrap/autosurface_pipeline.py
→ RepairMesh / RemoveNonManifoldVertices / FillSmallHoles
→ AutoSurface geometry=Mechanical, autoMerge=true, numPatches=1
→ data/crop_stp/<step文件stem>/<step文件stem>_candidate_<id>.stp
→ PatchImportService 导入
→ step_stats 输出 faces=12, edges=50, BRepCheck valid=true

备用 / 诊断路线：
原始 STL
→ Legacy centroid-only crop / Conservative boundary-band crop（自动 Patch preview fitting input modes）
→ Global Cut Chain crop（独立 STL 裁剪路线，当前不是 GeomagicFittingInputMode）
→ data/crop_stl/<step文件stem>/<step文件stem>_candidate_<id>.stl
→ Legacy / Conservative 可进入同一 Geomagic / import / preview / Apply 后续流程
→ Global Cut Chain 输出可通过手动 / 脚本 Geomagic 路线继续验证
```

当前验证命令：

```powershell
.\scripts\verify_spo.ps1
.\scripts\verify_spo.ps1 -Gui
.\scripts\verify_spo.ps1 -RealGeomagic
.\scripts\verify_spo.ps1 -StepStats -StepStatsPath "<patch_step_path>"
.\scripts\run_geomagic_patch.ps1 -InputStl "<local_stl_path>"
```

---

## 5. 已确认设计决策

### 5.1 MergePatchCommand undo 清空锁边状态

当前语义：

```text
用户先锁边
→ 执行合并
→ 撤销合并
→ 模型回到合并前
→ 锁边状态清空
```

该语义已确认是当前正确设计。

理由：

```text
1. 合并前后拓扑 edge ID 可能变化。
2. 合并后的锁边状态不一定能安全映射回合并前模型。
3. 撤销合并时清空锁边，可以避免锁边引用错误拓扑。
4. 用户可以在回退后的模型上重新选择并锁定边。
```

### 5.2 UnlockEdgeCommand 必须要求已有文档

当前语义：

```text
没有已加载模型时，LockEdgeCommand 和 UnlockEdgeCommand 都应返回错误。
```

这保证锁边/解锁边行为一致。

### 5.3 用户锁边是 protectedEdges 的一部分

当前语义：

```text
protectedEdges = 自动检测特征边 + 用户锁定边
```

目的：

```text
1. 自动特征边保护棱边、free edge、multiple edge。
2. 用户锁边保护人工认为重要但算法未识别的边。
3. same-domain 合并不应跨越 protectedEdges。
```

---

## 6. 待办清单

## P0：NSR-1.3 已完成证据与多候选复核待办

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| BRepCheck 阶段化诊断 | 已完成 | Filling / oriented / STEP roundtrip 分开报告；纯面积加权定位到 face `UnorientableShape`、wire `SelfIntersectingWire` |
| 边界表达诊断 | 已完成 | 26 条输出边均映射到原始边；pcurve / SameRange / SameParameter 完整，记录 3D / curve-on-surface 偏差 |
| 每张源面样本覆盖 | 已完成 | 报告 training / validation 数量、每面离群数和验证离群点最近训练距离 |
| 法向样本级诊断 | 已完成 | JSON 与独立 CSV 记录源/拟合法向、投影点、角度、UV 和 face id |
| 固定 96 点采样分配 A/B | 已完成 | 只改变 area_weighted / face_stratified_area_weighted；其余配置逐字段一致 |
| NSR-1 通过判定 | 未通过 | 0 / 1 / 4 / 8 四档均有 holdout interior / normal 超限；4 / 8 另有内存态 BRep 失败，`patch_acceptable=false` |
| 局部 G1 约束数据通路 | 已完成 | 样本采样时保留稳定支撑面 / UV；选中训练点使用 BRepFill_Filling 支撑面 G1，其余训练点和全部边界保持 G0 |
| 独立选择集与终验集 | 已完成 | 固定 192 点 selection 与 192 点 holdout；三组交集均为 0，最终接受只使用 holdout |
| 0 / 1 / 4 / 8 局部 G1 有限周期 | 已完成 / 未通过 | 总训练约束始终 96；实际 G1 与配置一致；无 Jacobian 问题，仍无可接受 patch |
| 多候选复核 | 待做 | 至少三个受支持真实候选重复同类距离 / 法向 / BRep 失败后，才评估 NSR-4 |
| NSR-1.4 自适应训练预算 | 后续冻结 | NSR-1.3 完成后再按面积、边界和曲率 / 法向变化确定预算；不得依赖原始面片数量 |
| NSR-4 解冻判定 | 未满足 / 继续冻结 | candidate 179 三档均失败仅为单例；同类问题尚未在至少三个受支持真实候选中重复 |

## P1：合并候选规划

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现 MergeCandidate 数据结构 | 已完成基础版 | 能表达候选区域 face 集合、边界、风险说明和运行时状态 |
| 实现 MergePlanner 基础候选生成 | 已完成基础版 | 能从 TopologyGraph 生成 FeatureBoundedRefit 候选区域 |
| 实现 FeatureBoundedRegionBuilder 基础区域构建 | 已完成基础版 | 能根据特征边/锁边阻断区域扩张 |
| GUI 显示候选区域 | 已完成基础版 | 可高亮 Top N、全部非隐藏候选和指定候选 |
| 用户接受/拒绝候选区域 | 已完成基础版 | 支持选择、接受、拒绝、隐藏、恢复候选；本阶段不应用到 B-rep |

## P2：项目保存与报告

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现项目状态序列化 | 待做 | 保存 `.spo.json` |
| 保存用户锁边状态 | 待做 | 重开项目后锁边可恢复 |
| 保存参数配置 | 待做 | 重开项目后参数可恢复 |
| 保存操作日志 | 待做 | 项目文件中记录核心操作 |
| 实现统一基础报告 | 待做 | 输出 face/edge 变化、free/multiple edge、BRepCheck |
| 实现导出批量实验报告 | 待做 | 可用于论文/组会实验记录 |

## P3：高级特征线与重拟合

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 曲率估计能力 | 待做 | 能估计局部曲率变化 |
| 边界分类能力 | 待做 | 能识别圆角起止线、凸凹分界 |
| ridge / valley 检测 | 待做 | 能在潮玩件凸起/凹陷处生成候选特征线 |
| SurfaceRefitter 自定义求解、G1 / G2 增强 | 冻结 / NSR-4 | 只有 OCCT 原生单曲面路线被定量证据证明不足时才评估 |
| 原生单曲面 Apply 验证 | NSR-1 通过后 / NSR-2 | 复用现有 probe、repair、StrictTopologyGate 和 STEP roundtrip，不先改 GUI |
| 局部 patch layout 重构 | 冻结 / NSR-5 | 单曲面路线被证据证明不适用后再评估，不属于当前目标 |
| 学习辅助候选区域推荐 | 待做 | 作为研究增强方向，不进入 MVP |

---

## 7. 当前建议验证命令

Windows 本地构建与测试：

```powershell
cd D:\pyProject\step-patch-optimizer
$env:Path='C:\Program Files\CMake\bin;C:\Users\27836\vcpkg;' + $env:Path

cmake --build --preset windows-msvc-debug --target step-patch-optimizer
ctest --preset windows-msvc-debug --output-on-failure --timeout 30
```

手动 GUI 验证流程：

```text
1. 打开一个 STEP/STP。
2. 切换到边选择模式。
3. Shift + 左键多选几条边。
4. 右键锁定选中边，确认锁边高亮。
5. Ctrl+Z，确认锁边撤销。
6. Ctrl+Y，确认锁边恢复。
7. 执行特征边检测。
8. 执行 same-domain 合并。
9. Ctrl+Z，确认模型回退并清空合并相关锁边状态。
10. Ctrl+Y，确认模型恢复。
11. 点击“预览合并”，选择或接受 FeatureBoundedRefit 候选区域。
12. 生成 Geomagic Patch preview，确认默认走 STP Sampled Candidate Surface fitting input。
13. 执行 Patch Apply，确认仍经过 PatchReplacementRepair、StrictTopologyGate 和 STEP roundtrip gate。
14. 执行合法性检查。
15. 导出 STEP，并确认导出期间 GUI 不进入未响应状态。
16. 确认导出完成后报告面板显示二次读取校验通过。
```

---

## 8. 近期推荐开发顺序（2026-07-14 修订）

当前默认路线：

```text
原始 STEP FeatureBoundedRefit candidate
→ 原始有序边界 G0 约束
→ 原始 STEP 内部训练点
→ BRepFill_Filling 单曲面拟合
→ 独立验证集误差 / 法向 / Jacobian / 复杂度检查
→ 单张 patch STEP + JSON
```

NSR-1 本轮实际完成：

```text
1. [x] 实现 SurfaceRefitter::fitSingleFace。
2. [x] 建立确定性内部训练样本和独立验证样本。
3. [x] 使用原始有序边界 G0 + 内部点构造 BRepFill_Filling。
4. [x] 输出边界、角点、内部距离和法向误差。
5. [x] 检查 Jacobian 翻转、近奇异和曲面复杂度。
6. [x] 新增 native_surface_fit_probe 和合成单元测试。
7. [x] candidate 179 手动探针保留在 data/runs/20260713_200400_711__native-surface-fit__03_配件_Clay__cand-0179__nsr-1-medium；结果为 BoundaryErrorExceeded，报告和诊断 patch 均可审阅。
8. [ ] NSR-2 未启动；本轮未改 GUI、未执行候选区域替换、未实现多曲面布局。
```

NSR-1.1 实际完成：

```text
1. [x] 把首个 failure_reason 与全部 failed_gates 分开表达。
2. [x] 输出 surface_error_samples.csv 和 surface_error_vectors.obj。
3. [x] 固定 tolerance3d=0.03、normal tolerance=15° 和 solver 参数，只比较 training=24/48/96。
4. [x] 三档统一 validation=192、boundary samples per edge=32、Jacobian grid=16。
5. [x] 每档写入独立 data/runs 目录并形成比较表。
6. [x] interior max/p95 从 0.154917/0.114527 降到 0.041323/0.025610，决定继续 OCCT Filling；不启动 NSR-4。
```

三档比较：

| tier | failed_gates | boundary max/p95/RMS/over | corner max/p95/RMS/over | interior max/p95/RMS/over | normal max/p95 | degree/poles/segments | Jacobian | BRepCheck |
|---|---|---|---|---|---|---|---:|---:|
| sparse | InteriorErrorExceeded, NormalErrorExceeded | 0.014135/0.008378/0.003548/0 | 0.012527/0.011608/0.004509/0 | 0.154917/0.114527/0.053045/89 | 58.4834°/14.7188° | 8x8/529/9 | true | true |
| medium | BoundaryErrorExceeded, CornerErrorExceeded, InteriorErrorExceeded, NormalErrorExceeded | 0.038180/0.025657/0.011631/24 | 0.038180/0.034959/0.014290/2 | 0.093389/0.042576/0.019742/18 | 47.9057°/9.3050° | 8x8/256/4 | true | true |
| dense | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.015591/0.009086/0.004321/0 | 0.015591/0.013647/0.005295/0 | 0.041323/0.025610/0.010416/4 | 45.4750°/9.7698° | 8x8/529/9 | true | false |

三档非 training 配置逐字段一致。normal 超限数从 8 降到 4 再降到 1；dense 的边界和角点已通过，但面本身 BRepCheck 失败。下一独立任务应继续 Filling 路线并定位 dense BRepCheck 失败与残余法向离群点；edge 1540 在 medium 为最差边界且超限，在 dense 仍为最差边界但已低于 0.03，因此暂不启动边界约束参数实验。

NSR-1.2 已按以下顺序完成：

```text
1. [x] 先补 BRepCheck 分阶段 / 子形状状态，不修改求解器或调用 repair。
2. [x] 再补每张源面的训练覆盖和法向样本级诊断。
3. [x] 固定总训练点 96，只比较纯面积加权与分层面积加权。
4. [x] 两种策略均未通过全部原生质量门，因此不进入 NSR-2；下一步选择局部切平面约束证据，NSR-4 继续冻结。
```

NSR-1.3 已按以下顺序完成：

```text
1. [x] 0 个 G1 数值复现分层面积加权基线，并为全部训练样本保留源面 / UV；支撑面重算点最大偏差 1.59e-14。
2. [x] 建立互斥的 training=96、selection=192、holdout=192；四档三个交集均为 0。
3. [x] 按归一化残差和确定性 NMS 形成嵌套 1 / 4 / 8 集合，并映射到不同现有训练样本，只替换 G0，不增加总约束。
4. [x] 完成 0 / 1 / 4 / 8 四档及 STEP roundtrip；全部失败后停止继续混调 OCCT Filling。
5. [x] candidate 179 只形成单例能力边界证据；NSR-4 继续冻结，多候选复核列为下一证据任务。
```

固定配置：training=96、selection=192、holdout=192、boundary samples per edge=32、Jacobian grid=16、tolerance3d=0.03、normal tolerance=15°、plate degree=3、points on curve=24、iterations=3、max degree=8、max segments=12。

这里的 96 只用于隔离“局部 G1 是否有效”这一变量，不是生产环境的通用点数。不同大小和复杂度区域的训练预算将在 NSR-1.4 中按连续几何自适应计算；本轮不得同时改变训练预算。

| G1 | failed_gates | holdout interior max/p95/RMS/over | holdout normal max/p95/RMS/over | BRep filling/oriented/roundtrip | Jacobian near/flip | degree/poles/segments |
|---:|---|---|---|---|---|---|
| 0 | InteriorErrorExceeded, NormalErrorExceeded | 0.054879/0.026982/0.010159/8 | 23.5646°/7.2512°/3.9602°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 1 | InteriorErrorExceeded, NormalErrorExceeded | 0.054914/0.026859/0.010164/8 | 23.1334°/7.2574°/3.9184°/2 | true/true/true | 0/0 | 8x8/529/9 |
| 4 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.038039/0.017075/0.007938/2 | 22.9609°/7.2526°/3.5619°/1 | false/false/true | 0/0 | 8x8/529/9 |
| 8 | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.038155/0.017076/0.007676/2 | 22.8859°/6.7965°/3.4800°/1 | false/false/true | 0/0 | 8x8/529/9 |

4 / 8 个 G1 的具体 BRep 失败均为 face[0]=`UnorientableShape`、wire[0]=`SelfIntersectingWire`；所有 STEP roundtrip 均为一张 face 且 BRepCheck=true，但不能替代 oriented face 验收。正式 comparison：`data/runs/20260714_143049_479__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.3-local-g1-tiers`。

| strategy | failed_gates | boundary max/p95/RMS/over | corner max/p95/RMS/over | interior max/p95/RMS/over | normal max/p95/RMS/over | training min/max/zero | degree/poles/segments | Jacobian | BRep filling/oriented/roundtrip | roundtrip face/edge |
|---|---|---|---|---|---|---|---|---:|---|---|
| area_weighted | BRepCheckFailed, InteriorErrorExceeded, NormalErrorExceeded | 0.015591/0.009086/0.004321/0 | 0.015591/0.013647/0.005295/0 | 0.041323/0.025610/0.010416/4 | 45.4750°/9.7698°/5.3601°/1 | 0/4/2 | 8x8/529/9 | true | false/false/true | 1/50 |
| face_stratified_area_weighted | InteriorErrorExceeded, NormalErrorExceeded | 0.015730/0.010173/0.004392/0 | 0.015730/0.011804/0.005387/0 | 0.068507/0.024468/0.012112/8 | 40.7441°/8.2060°/4.7287°/1 | 1/3/0 | 8x8/529/9 | true | true/true/true | 1/43 |

结构诊断结论：

```text
1. area_weighted 在 Filling 输出和方向反转后都失败：face[0]=UnorientableShape，wire[0]=SelfIntersectingWire；方向反转没有改变该状态。
2. 26 条内存态输出边全部一一映射到原始 boundary edge；无缺失 pcurve、SameRange / SameParameter 失败或 edge 级 BRep 状态错误。
3. area_weighted 最差边表示为 output edge 5 / source edge 1540，3D-COS max=0.0178773，小于 edge tolerance=0.0189194。
4. face_stratified_area_weighted 的 Filling / oriented BRepCheck 均通过；两种 STEP roundtrip 都为一张 face 且 BRepCheck=true，但 edge 数分别为 50 / 43，不能替代内存态验收。
```

误差与覆盖结论：

```text
1. area_weighted 的 4 个内部超限点来自 face 1719/1717/1710/1750，对应每面 training=3/3/3/2，最近训练距离=0.378863/0.313861/0.287934/0.426819；两个零训练面 1735/1738 并不是离群来源。
2. 分层策略使上述四个同一验证点全部降到 0.03 内，也把每面 training zero 从 2 降到 0；但在 face 1709/1717/1718 形成 5/2/1 个新超限点，interior max 和超限数反而升到 0.068507 和 8。
3. face 1705 的 normal max 从 45.4750° 降到 40.7441°，仍远高于 15°；两策略该面 training 均为 2，说明只保证每面点数不等于获得局部切平面控制。
4. 分层策略已证明采样覆盖会同时改变 BRep 结构和局部误差分布，但没有形成 patch_acceptable=true；下一步先验证局部切平面约束，不据此直接启动自定义 B-spline 求解。
```

正式运行目录：

```text
data/runs/20260714_014513_419__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.2-area-weighted
data/runs/20260714_014616_181__native-surface-fit__03_配件_Clay__cand-0179__nsr-1.2-face-stratified-area-weighted
data/runs/20260714_014717_549__native-surface-fit-comparison__03_配件_Clay__cand-0179__nsr-1.2-sampling-ab
```

冻结范围：

```text
- 不把局部原生拟合扩展到整个潮玩件。
- 不做原生多曲面布局。
- NSR-1.3 证据门未满足前不解冻 NSR-4；解冻后也只实现受限控制点求解，不重写通用 NURBS 内核。
- 不给全部边界施加 G1。
- 不接 GUI 或自动 Geomagic 回退。
- 不放宽 StrictTopologyGate。
- 不在默认单元测试里依赖真实 Geomagic、Creo 或真实大样例路径。
```

Geomagic 后备路线和覆盖 Gate 见 `docs/native_surface_refitting_guide.md`，原有执行细节继续见 `docs/geomagic_patch_workflow.md`。

---

## 9. 版本进度判断

当前阶段判断：

```text
MVP 基础闭环：已完成
锁边交互闭环：已完成
最小 undo/redo：已完成
same-domain 合并闭环：已完成
P0 稳定性收口：基本完成
复杂 STEP 样例回归测试：待做
合并候选规划：已完成基础版
候选区域 GUI 预览：已完成基础版
候选区域选择/接受/拒绝/隐藏：已完成基础版
Face / Candidate Inspect：已完成基础版
Analytic primitive candidate detection：已删除（Cleanup-6）
Stage 2.8 Enhancement A B-spline CylinderLike approximate detection：已删除（Cleanup-6）
Stage 2.8 Enhancement B B-spline ConeLike / FrustumLike approximate detection：已删除（Cleanup-6）
Candidate type preview：已收口为 FeatureBoundedRefit / Unknown
RegionMerger 框架准备：历史实现已删除（Cleanup-1 / Cleanup-5）
PlaneRegionMerge：已删除旧后端（Cleanup-4）
平面候选批量合并：已删除旧入口 / Command / 后端
平面合并边界简化：随旧 PlaneRegionMerger 删除
Stage 3-S Shared Primitive Fields：历史实现已删除（Cleanup-5）
SphereRegionMerge：已删除旧后端（Cleanup-4）
Geomagic patch preview / Apply：保留为 feature-bounded-refit 稳定基线和 native-surface-refit 后备路线
STP Sampled Candidate Surface：已完成并作为当前默认 fitting input mode
STL Global Cut Chain crop：已完成第一版并作为可选裁剪器接入 GUI
GUI 后台任务 / 状态显示优化：已合入 1f0c3d7 基准；打开 STEP、导出 STEP、Patch preview 和 Patch Apply 均已后台化
d1c00e4 Improve boundary constrained Geomagic patch flow：因真实样例缝合回归，暂不进入当前主线
Sharp Contours 实验：无收益，暂不进入当前主线
NSR-0：已确认 fitting STL 角点完整而 raw Geomagic 底层曲面圆角化，并建立原生拟合指导文档
NSR-1：探针实现已完成；candidate 179 中等档同时存在边界、角点和内部超限，`BoundaryErrorExceeded` 只是首个失败项
NSR-1.1：已完成全部失败门、样本级诊断和固定三档实验；结果支持继续 OCCT Filling，但尚无可接受 patch
NSR-2：冻结，NSR-1 通过后复用现有 Apply / repair / StrictTopologyGate / STEP roundtrip
```

**旧 OCCT 近似平面路线定位：**

```text
Stage 3A-Fix / A6 只保留为历史诊断与研究分支。
它不再是当前推荐的主推进路线。
除非明确回到 OCCT 近似平面合并问题，否则不应抢占 Geomagic patch input / Apply 收口任务。
```

**冻结区域：**

```text
CylinderRegionMerge：冻结
ConeRegionMerge：冻结
TorusRegionMerge：冻结
SphereRegionMerge：已下线并删除旧后端
Freeform Candidate Detection：冻结
Freeform B-spline / Plate Refit：已解冻，当前 NSR-1 主线
项目保存恢复：冻结
完整误差评估：冻结
高级特征线：冻结
局部重拟合：已解冻，当前仅限单外轮廓、无孔候选的单曲面拟合
```

总体评价：

```text
当前项目已在 native-surface-refit 独立分支进入原生局部自由曲面拟合阶段。
feature-bounded-refit 继续保留 Geomagic patch preview / Apply 稳定基线，不在该分支直接改写。

NSR-1.3 已完成，但 NSR-1 尚未通过：
1. 已保留当前原生拟合算法，并输出全部失败门和样本级位置。
2. 已固定验证密度和 solver 参数，只比较三档训练样本数量。
3. 已固定总训练点 96 完成纯面积加权 / 分层面积加权 A/B，并补齐三阶段 BRep、每面覆盖、最近训练距离和法向向量诊断。
4. 0 / 1 / 4 / 8 局部 G1 档均保持 96 个训练约束和三集合互斥；4 / 8 档降低内部距离，但法向仍超限并重新引入内存态 BRep 失败，没有 `patch_acceptable=true`。
5. 下一证据任务是在至少三个受支持真实候选上复核同类失败；不改 GUI，不做 Apply、多曲面、NSR-1.4、NSR-2 或 NSR-4。
6. “一步到位”指用户一次操作内运行有限确定性策略并自动过 Gate，不承诺单一求解参数覆盖任意曲面。

Geomagic 只在原生路线明确不支持或受控拟合失败后作为后备，并必须先通过底层曲面覆盖 Gate。
```

---

## 10. Stage 3A-Approx / A6 历史诊断分支（2026-05-27）

```text
Stage 3A-Approx / A6：历史诊断分支，非当前默认推进路线

Cleanup-5 后，`PlaneRegionMerger`、旧 RegionMerge result/options 类型和相关自动测试均已删除；以下条目只作为历史诊断记录，不代表当前仓库仍有可调用实现。

已完成：
- A6.1：PlaneRegionMerger 在 approximate planar rebuild 进入 MakeFace 前检查 ordered boundary edges。
- A6.1：boundary 3D curve 采样点若明显偏离拟合平面，提前返回 DeviationTooLarge。
- A6.1：失败时 result.document 保持原 document，Command 层仍不会污染当前模型。
- A6.1：新增测试覆盖“face 采样误差低，但 boundary 曲线偏离拟合平面”的失败路径。
- A6.2：RegionMergeResult 曾新增 diagnostic_report，用于承载近似平面重建失败诊断；该 result 类型已在 Cleanup-5 删除。
- A6.2：PlaneRegionMerger approximate mode 失败路径会输出 candidate id/type/status、face ids、boundary/internal/ordered edge ids、拟合平面、face/boundary deviation、RegionBoundaryAnalyzer 结果、每条 boundary edge 的 3D curve type、start/mid/end 距离和 pcurve 信息。
- A6.2：data/samples/3#_底部.stp 已纳入自动测试；样例存在时会读取真实 STP、生成候选并验证失败诊断报告，同时确认 document/stats 不被污染。

仍未完成：
- A6.3：ShapeFix_Wire / ShapeFix_Face / SameParameter 最小修复路径评估。
- A6.4：projected boundary / pcurve rebuild 设计与验证。

当前结论：
A6.1 不是提升合并力度，而是防止不安全的近似平面候选进入 MakeFace 后才在 BRepCheck 阶段失败。
A6.2 让真实失败样例具备可定位诊断。若后续明确回到 OCCT 近似平面合并，再基于 diagnostic_report 做 A6.3；当前不应让 A6 抢占 Geomagic patch input / Apply 收口。
```
