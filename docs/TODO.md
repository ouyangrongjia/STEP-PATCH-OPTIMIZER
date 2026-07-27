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

当前阶段：命令行纵向链已重构为显式 `RegionCurveNetwork` 先行。原 STEP 外边界在网络中精确引用或受控切分；内部投影点以端点固定、局部弦切向和不超过相邻最短弦长三分之一的手柄构造三次复合 B-spline，并依次用 `1/0.5/0.25` 手柄缩放在原 STEP 上验收。局部三次仍越过 `0.03 mm` 投影门时，以通过投影点的一级 B-spline 作为仅诊断回退，并把实际连续性降为 G0。布局和联合拟合通过 network hash 消费冻结网络，双侧轨迹以共同参数硬约束到唯一三维曲线；G1/G2 进入软目标并作至少 97 点解后门。原生 builder 为每个节点只创建一个 Vertex、每条内部曲线只创建一个 Edge，双侧 pcurve 附着完成后统一执行 `SameRange`/`SameParameter`。只要有可构造曲面网络就继续固定容差诊断缝合，任何产品门失败时 `canApply=false`。

永久产物合同已经进入探针和运行脚本：区域重拟合未显式传入 `-OutputDir` 时，运行目录固定建立在 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\<run-id>\`；显式 `-OutputDir` 仍优先。每次运行均在根目录写 `evidence_manifest.json`，并创建 `artifacts/curve_network/`、`artifacts/layout/`、`artifacts/fit/`、`artifacts/brep/`、`artifacts/final/`、`validation/` 和 `external_cad/`。能够到达相应阶段时，依次保留 `selected_curve_network.step`、拟合 patch network、`replacement_quilt.step`、`sewn_attempt.step` 和最终整件 STEP，且全部进入 manifest 并登记 SHA-256。`artifacts/final/artifact_status.json` 始终存在；产品通过写 `final_sewn.step`，仅诊断缝合成功写 `diagnostic_sewn.step`，无可构造几何时记录 `Unavailable` 且不伪造 STEP。缓存保存内部曲线的精确 B-spline 定义、构造模式和实际连续性，二进制版本为 7，旧版本明确拒绝。

当前 candidate-179 全链诊断基线为 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260727_185138_313__native-region-refit__03_配件_Clay__cand-0179__target\`。K=4 曲线网络、布局和参数化均构造成功；5 条内部曲线全部使用 `LocalCubic`，没有一级回退或连续性降级，最大投影误差为 `0.0286171 mm`，原先曲线 30 的 `31.0338 mm` 离面振荡已消除。联合拟合因硬边界约束不相容而使用 best-effort 支撑网，随后仍生成拟合面片、4-face replacement quilt、固定 `0.001 mm` 容差的诊断缝合件和永久 `diagnostic_sewn.step`。该缝合件仍有 34 条自由边、BRepCheck 和 STEP 往返失败；Creo 可导入并生成 ModelCHECK 报告，但几何检查和短边检查失败。因此本轮状态为 `DiagnosticOnly`、`canApply=false`，不是产品成功。

下一验收点已从曲线构造前移到联合拟合与重裁剪：先消除 `InfeasibleHardBoundaryConstraints`，使原 STEP 外边界和共享内部曲线的完整硬约束在当前控制网中可满足；再解决 21 次 `SameParameter` 失败以及替换后 34 条自由边，最后复核 StrictTopologyGate、STEP 往返和 Creo。不得通过放宽 `0.03 mm` 曲线投影门、`SameParameter` 或固定 sewing 容差制造成功。旧 candidate-179 49→46 与 12 区域 Surface Reduction 回归仍是独立前置验收，不能由合成测试或本次诊断缝合替代。GUI 接入仍不在本轮范围内。

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
