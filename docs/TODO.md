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

当前阶段：命令行纵向链已改为显式 `RegionCurveNetwork` 先行。内部曲线采用端点固定的正则化 C2 三次 B-spline，并分别检查投影、转角、自交和非节点交叉；失败时只允许用穿过原投影点的一级曲线继续诊断。每张 patch 同时保存无翻转载体坐标和严格矩形 `surfaceParameters`，四组逻辑边映射到 `[0,1]^2` 的四条精确 iso 边。联合拟合以及超时或不可行时保留的 Coons 候选都必须锁定四条边界；G1/G2 只能调整相邻内部控制行，未锁边的初始支撑面不能进入 B-Rep。builder 复用唯一 Vertex、唯一三维 Edge 和双侧精确 pcurve，经 257 点检查后才设置 `SameParameter`。

永久产物由运行脚本先写 `data\.native-region-refit-staging\<run-id>\`，成功包装后原子发布到 `data\runs\<run-id>\final\`，异常退出清理暂存目录，已有 run 冲突时拒绝覆盖。正式目录始终只含 `run.json`、`RUN.md`、`artifact_status.json`、`validation_summary.json`、`evidence_manifest.json`，以及阶段确实完成时的 `curve_network.json`、`curve_network.step`、`untrimmed_support_surfaces.step`、`trimmed_patch_faces.step` 和互斥的 `final_sewn.step` 或 `diagnostic_sewn.step`。manifest 最后生成且不记录自身。缓存二进制版本为 10，精确保存双参数域和 edge-use pcurve，旧版本明确拒绝。

当前 candidate-179 真实结果位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260728_205420_144__native-region-refit__03_配件_Clay__cand-0179__target\final\`。K=4 曲线网络产品门通过：5 条内部曲线均为 `RegularizedCubic`，最大投影误差 `0.0298705 mm`，无自交或非节点交叉。布局可构造且无翻转或 pcurve 交叉，但面积比 `2.49277e-5`、最大畸变 `201.442`，布局产品门失败。边界锁定诊断候选的边界和双侧分离均远小于 `0.001 mm`，但独立距离 `0.245723 mm`、法向 `20.1538°`、G1 `18.3648°`，拟合门失败。4 张裁剪 Face 的 38 次 `SameParameter` 检查无失败，内部自由边和意外交线均为 0；内存态逐 Face BRepCheck 尚未通过，而导出的 `trimmed_patch_faces.step` 再导入后 BRepCheck 和 Curve-on-Surface 检查通过。固定 `0.001 mm` 诊断缝合得到 1 个 Solid、0 条自由边，STEP 往返通过；Creo 可导入并生成 PRT，ModelCHECK 未通过。结果为 `DiagnosticOnly`、`canApply=false`。

下一验收点仍在布局、拟合和内存态裁剪校验：先把面积比和畸变带回产品门，使独立距离、法向和 G1 分别不超过 `0.03 mm`、`15°`、`5°`，再定位并消除逐 Face BRepCheck 与导出后 BRepCheck 的差异，最后复核 Creo ModelCHECK。不得放宽曲线投影、Curve-on-Surface、`SameParameter` 或固定 `0.001 mm` sewing 容差制造成功。旧 candidate-179 49→46 与 12 区域 Surface Reduction 回归仍是独立前置验收，不能由本次诊断缝合替代。GUI 接入仍不在本轮范围内。

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
