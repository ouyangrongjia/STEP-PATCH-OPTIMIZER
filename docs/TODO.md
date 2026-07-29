# 当前任务

本文只记录当前未完成任务及其验收条件。已完成事项见 completed_features.md，当前能力与限制见 implementation_status.md，历史过程由 Git 和 Obsidian 保存。

## 当前承诺

### 原生区域级多曲面重拟合

目标：

- 从 STEP 候选区域提取硬边界和相邻面约束，生成自适应参考采样、少面布局、多曲面联合拟合与原边界重裁剪结果。
- 新路线零调用、零运行时依赖 Geomagic 和 QuadriFlow，不改变现有 Surface Reduction 搜索语义或 GUI 入口。

验收：

- 首版明确拒绝孔洞、多外环、分支边界、非流形和不连通候选。
- 正式运行目录严格遵守单一 `final/` 白名单合同，状态文件始终存在，几何文件只在对应阶段确实完成时发布，并登记大小和 SHA-256。
- candidate-179 先通过距离、法向、接缝、BRep、StrictTopologyGate、STEP 往返和 Creo 整件检查，再动态复核两个额外真实候选。
- `windows-msvc-debug` 下 candidate-179 算法缓存冷运行不超过 15 分钟、暖运行不超过 5 分钟、峰值内存不超过 8 GiB，完整联合拟合最多三次。
- 保持现有 candidate-179 49→46 和 12 区域回归不变；失败不得提交模型或回退到 Geomagic。

当前阶段：命令行纵向链已完成实际 `surfaceParameters` 布局评价、同面 `FaceId/UV` 重心映射、四逻辑边确定性候选、每侧最多 25 控制点的兼容三次 B-spline 支撑，以及边界控制点固定、内部变量约化 SPD 公平求解。Coons 只保留为 `BoundaryLockedDiagnostic`。最终曲面直接统计逐面 poles/knots、公平能量、法向变化、曲率振荡、Jacobian 和条件数；非有限、翻转或退化为质量硬失败。builder 复用唯一 Vertex、唯一三维 Edge 和双侧精确 pcurve，把四张 Face 组成单一 Shell，并逐 Shell/Face/Wire/Edge 保存 `BRepCheck` 状态。

永久产物由运行脚本先写 `data\.native-region-refit-staging\<run-id>\`，成功包装后原子发布到 `data\runs\<run-id>\final\`，异常退出清理暂存目录，已有 run 冲突时拒绝覆盖。正式目录始终只含 `run.json`、`RUN.md`、`artifact_status.json`、`validation_summary.json`、`evidence_manifest.json`，以及阶段确实完成时的 `curve_network.json`、`curve_network.step`、`untrimmed_support_surfaces.step`、`trimmed_patch_faces.step` 和互斥的 `final_sewn.step` 或 `diagnostic_sewn.step`。manifest 最后生成且不记录自身。缓存二进制版本为 11，算法指纹为 `native-regional-refit-v40-fair-boundary-spd`，旧布局和拟合缓存明确拒绝。

当前 candidate-179 真实结果位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_032156_431__native-region-refit__03_配件_Clay__cand-0179__target\final\`。K=4 曲线网络产品门仍通过：5 条内部曲线均为 `RegularizedCubic`，最大投影误差 `0.0298705 mm`，无自交或非节点交叉；2572 个拟合样本全部完成同面重心映射。实际 `surfaceParameters` 发现 116 个翻转或退化三角形，面积比为 0，畸变非有限，布局产品门失败。每侧 25 控制点内不能满足冻结曲线误差，约化 SPD 正式路径未启动，保留 Coons 诊断；独立距离 `0.324422 mm`、法向 `21.7735°`、G1 `18.3652°`，拟合门失败。四张面共 9645 poles，最大单边 125，复杂度门失败。4 张裁剪 Face 的 38 次 `SameParameter` 检查无失败，内部自由边和意外交线均为 0；单 Shell 中两张 Face 报 `BadOrientationOfSubshape`。固定 `0.001 mm` 诊断缝合与 STEP 往返得到 1 个 Solid、0 条自由边、0 条多重边和有效 B-Rep；Creo 导入 `OK/PASS` 并生成 PRT，ModelCHECK 未通过。结果仍为 `DiagnosticOnly`、`canApply=false`。

下一验收点仍在布局、边支撑和内存态裁剪校验：先消除实际 `surfaceParameters` 的翻转与退化，使畸变不超过 100；再让四逻辑边在每侧 25 控制点内通过冻结曲线门并进入约化 SPD 公平求解，使独立距离、法向和 G1 分别不超过 `0.03 mm`、`15°`、`5°`；最后消除两张 Face 的 `BadOrientationOfSubshape` 并复核 Creo ModelCHECK。K=5/6/8 本次均未在 900000 ms 预算与 30 秒硬退出宽限内完成布局筛选。不得放宽曲线投影、Curve-on-Surface、`SameParameter` 或固定 `0.001 mm` sewing 容差制造成功。旧 candidate-179 49→46 与 12 区域 Surface Reduction 回归仍是独立前置验收，不能由本次诊断缝合替代。GUI 接入仍不在本轮范围内。

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
