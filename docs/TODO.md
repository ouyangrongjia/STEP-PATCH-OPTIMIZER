# 当前任务

本文只记录当前未完成任务及其验收条件。已完成事项见 completed_features.md，当前能力与限制见 implementation_status.md，历史过程由 Git 和 Obsidian 保存。

## 当前承诺

### 原生区域级多曲面重拟合

目标：

- 从 STEP 候选区域提取硬边界和相邻面约束，生成自适应参考采样、少面布局、多曲面联合拟合与原边界重裁剪结果。
- 新路线零调用、零运行时依赖 Geomagic 和 QuadriFlow，不改变现有 Surface Reduction 搜索语义或 GUI 入口。

验收：

- 首版明确拒绝孔洞、多外环、分支边界、非流形和不连通候选。
- 最终运行目录同时保留自适应采样 STL、布局可视化和最终整件缝合 STEP，并登记 SHA-256 和门禁状态。
- candidate-179 先通过距离、法向、接缝、BRep、StrictTopologyGate、STEP 往返和 Creo 整件检查，再动态复核两个额外真实候选。
- `windows-msvc-debug` 下 candidate-179 算法缓存冷运行不超过 15 分钟、暖运行不超过 5 分钟、峰值内存不超过 8 GiB，完整联合拟合最多三次。
- 保持现有 candidate-179 49→46 和 12 区域回归不变；失败不得提交模型或回退到 Geomagic。

当前阶段：命令行纵向链已重构为显式 `RegionCurveNetwork` 先行。原 STEP 外边界在网络中精确引用或受控切分；内部曲线在支撑面拟合前生成、投影、公平化、验收并冻结。布局和联合拟合通过 network hash 消费该网络，双侧轨迹以共同参数硬约束到唯一三维曲线；G1/G2 进入软目标并作至少 97 点解后门。原生 builder 为每个节点只创建一个 Vertex、每条内部曲线只创建一个 Edge，双侧 pcurve 附着完成后统一执行 `SameRange`/`SameParameter`。只要有可构造曲面网络就继续固定容差诊断缝合，产品门失败时 `canApply=false`。

永久产物合同已经进入探针和运行脚本：每次运行均创建 `artifacts/final/artifact_status.json`；产品通过写 `final_sewn.step`，仅诊断缝合成功写 `diagnostic_sewn.step`，无可构造几何时记录 `Unavailable` 且不伪造 STEP。诊断增加 `curve_network.json`、曲线/节点 PLY、连续性计数、投影门、逐边 Curve-on-Surface/`SameParameter`/双侧分离/法向和曲率结果。缓存增加 curve-network 层并升级为不接受旧二进制的版本。

当前 candidate-179 独立运行 `data/runs/20260727_candidate_0179_curve_network_layout_probe_v8/` 在 `CurveNetwork` 投影门失败，报告最大投影偏差 `42272.5 mm`、门限 `0.03 mm`。该目录包含 `run.json`、`RUN.md`、完整 `evidence_manifest.json`、`diagnostics/curve_network.json` 和 `artifacts/final/artifact_status.json`；最终状态为 `Unavailable`，没有伪造 STEP。该失败只完成诊断闭环，不证明曲线网络、布局、拟合、B-Rep、缝合或产品质量在 candidate-179 上通过。

下一验收点是查明 candidate-179 内部曲线近似出现巨幅离面偏差的几何原因，并在不放宽投影门的前提下让冻结网络通过；之后才能继续验证真实候选上的 G0/G1/G2、双侧 pcurve、`SameParameter`、固定容差缝合、StrictTopologyGate、STEP 往返和 Creo。旧 candidate-179 49→46 与 12 区域 Surface Reduction 回归仍是独立前置验收，不能由合成测试或本次 `Unavailable` 诊断替代。GUI 接入仍不在本轮范围内。

事实来源：`src/reduction/region_refit/`、`native_region_refit_probe` 和 `data/runs/<run-id>/`。

## 可选后续方向

以下内容是候选方向，不代表已经排期。

### 最少面搜索改进

目标：

- 在不降低最终质量门的前提下，提高 MinimizeFaceCount 找到更低有效面数结果的能力。
- 保持 BestFoundWithinBudget 与 OptimalProven 的语义边界。

若启动，验收至少包括：

- 使用真实候选与独立 run-id。
- 明确拟合、终验、墙钟和内存预算。
- 与 candidate-179 49→46 TargetRatio 锁定回归隔离。
- 不把未通过接缝或终验的中间状态计为结果。
- 所有新接受结果通过 StrictTopologyGate、STEP 往返和适用的跨 CAD 检查。

### GUI 窗口级自动化

目标：

- 覆盖导入、候选选择、后台预览、Apply、撤销、重做和导出的窗口级行为。

若启动，验收至少包括：

- 自动化不依赖真实用户目录或固定样例编号。
- 长任务心跳与取消/失败状态可观察。
- 旧预览失效和失败不提交得到覆盖。
- 保留现有领域层和脚本合同测试。

### 项目会话持久化

目标：

- 在不复制几何真值的前提下保存必要的用户会话信息。

若启动，需先确定：

- 哪些状态属于项目文件，哪些仍由 STEP 和 data/runs 提供。
- FaceId、EdgeId 等瞬时身份在重新导入后的映射策略。
- 格式版本和迁移边界。
- 外部 STL 与运行产物只记录引用还是纳入包。

## 冻结路线

以下路线不属于当前任务：

- 原生单曲面重拟合。边界和重新启用条件见 native_surface_refitting_guide.md。
- 原生局部 AutoSurface。结论见 native_autosurface_guide.md。
- Geomagic AutoSurface 的继续优化。当前仅保留显式后备合同，见 geomagic_patch_workflow.md。

只有用户明确重新启用并给出新的验收目标时，才为冻结路线增加任务。

## 新任务记录格式

新增任务时只写：

1. 目标和不做什么。
2. 可观察的验收条件。
3. 受影响模块和事实来源。
4. 需要保留的回归基线。
5. 预算或外部依赖。

完成后从本文删除，并在属于里程碑时把一句结论和证据入口写入 completed_features.md。精确参数、指标和日志仍留在 data/runs。
