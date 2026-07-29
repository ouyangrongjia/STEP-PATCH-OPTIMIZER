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

当前阶段：命令行纵向链保留原有矩形 `surfaceParameters`、四逻辑边、每侧最多 25 控制点的兼容三次 B-spline 支撑和内部变量约化 SPD 求解。P0 已统一内部 seam 的共同参数语义：沿唯一共享三维 Edge 的同一参数 `t` 同时评价 `Edge(t)` 与双侧 `Surface(pcurve(t))`；UV 拓扑、支撑曲面 Jacobian、Face/Wire 方向和验证器一致性分别判定。Coons 仍只允许作为 `BoundaryLockedDiagnostic`。

永久产物由运行脚本先写 `data\.native-region-refit-staging\<run-id>\`，成功包装后原子发布到 `data\runs\<run-id>\final\`，异常退出清理暂存目录，已有 run 冲突时拒绝覆盖。正式目录始终只含 `run.json`、`RUN.md`、`artifact_status.json`、`validation_summary.json`、`evidence_manifest.json`，以及阶段确实完成时的 `curve_network.json`、`curve_network.step`、`untrimmed_support_surfaces.step`、`trimmed_patch_faces.step` 和互斥的 `final_sewn.step` 或 `diagnostic_sewn.step`。manifest 最后生成且不记录自身。缓存二进制版本为 14，算法指纹为 `native-regional-refit-v44-shared-edge-parameter-validator-gates`，旧缓存明确拒绝。

P0 锁定诊断证据位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_212344_628__native-region-refit__03_配件_Clay__cand-0179__target\final\`。旧 `4.96138 mm` seam 距离来自 mesh-vertex 配对污染；共同参数下双侧支撑面最大分离为 `4.36e-14 mm`，Curve-on-Surface 偏差为 `4.80e-14 mm`。7 个原始三角形符号变化来自 STEP Face 局部绕序，不应归类为 UV 翻转。Face/Wire 仍有 `BadOrientationOfSubshape`；固定 `0.001 mm` 诊断缝合、STEP 回读和 Creo 导入成功，但 ModelCHECK 失败，因此仍为 `DiagnosticOnly`、`canApply=false`。

当前未完成项仍是拟合曲面的距离、法向、接缝连续性、控制网复杂度和物理公平性产品门。当前实现不包含 P1 支撑面解耦或 P2 边界预检与定向增片；其既有运行目录只作为历史失败证据保留。不得以水密、STEP 可回读或 Creo 可导入替代几何与 Apply 验收，也不得放宽 Curve-on-Surface、`SameParameter` 或固定缝合容差制造成功。

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
