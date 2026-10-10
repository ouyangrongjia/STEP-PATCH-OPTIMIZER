# 当前任务

本文只记录当前未完成任务及其验收条件。已完成事项见 completed_features.md，当前能力与限制见 implementation_status.md，历史过程由 Git 和 Obsidian 保存。

## 当前承诺

### 固定 GUI 分区重建的剩余产品验收

以 candidate-179 所属的主 GUI 分区为条件，保持原区域归属，统一构造公共边界，分区内部独立拟合后缝合；本轮不改变分区算法。当前重建必须完全自主实现，不调用 Geomagic Wrap API 或复用商业拟合曲面。

剩余验收：

- 最终 STEP 的实际回读面数严格小于对应参考 STEP；同时保持保真度、边界对应和水密性。
- 无肉眼可见的明显褶皱，尤其是弯折、狭窄过渡和肘部形状；通过统一视角、真实曲面法向和接缝检查，不能以减面单独宣告成功。
- 在 Creo 中实际打开为水密实体，保留导入与 ModelCHECK 证据，不能用 OCCT 有效实体代替 Creo 验收。
- 公共边界平滑后仍由相邻区域共用；保留精确区域/公共边组合对应和完整源三角形覆盖，不用最近标签投票代替。
- 验证不同曲率、孔洞及复杂边界的区域，并比较多档质量与性能；任何未通过项保留失败记录，不回退至商业拟合。
- 在共用骨架路线中消除窄弯折区残余波纹，并降低短边交换和控制点的表达成本；保持现有边界、距离和实体预算。
- 在主曲线重构结果上继续消除数字 3 内弧、急弯、相邻牌面和窄带残余起伏；补齐闭环、多分支及未覆盖接口的曲线组织，并联合改进过渡带宽度与曲率约束。解决局部支线转角和接缝法向极值的退化，核查逐面采样的极端法向。保持两侧支撑兼容、原曲面距离预算与安装配置 Creo 验收，不能仅以接缝 P95 下降判断可见质量。

已完成能力与限制见 `implementation_status.md`，最新诊断候选见 `data/runs/whole-partition-brep-v1/native-v7/result_v8/`，此前 native-v6、native-v4 的零提示基线、native-v5 的失败记录及更早对照保留；此前商业版本仅保留为历史证据，不满足当前依赖合同。原 candidate-179 基线保持不变。

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

当前阶段：阶段 2A 已增加独立的硬位置闭合预求解。阶段 1 开缝低秩面即使 `allConstructible=false`，也先以外部和内部 Curve-on-Surface / `SameParameter` 的 `0.001 mm` 硬门执行闭合可行性求解；内部曲线相对原区域的 `0.03 mm` 投影门保持独立。预求解只使用最小位移、弱公平和 Jacobian barrier，允许一般 pcurve 与控制点交替更新，并仅在失败 patch 的主导方向局部插 knot，单方向实际 poles 硬限 25。只有闭合预求解通过后才允许进入阶段 2B 的 G1/G2 联合公平化。

永久产物由运行脚本先写 `data\.native-region-refit-staging\<run-id>\`，成功包装后原子发布到 `data\runs\<run-id>\final\`，异常退出清理暂存目录，已有 run 冲突时拒绝覆盖。正式目录始终只含 `run.json`、`RUN.md`、`artifact_status.json`、`validation_summary.json`、`evidence_manifest.json`，以及阶段确实完成时的 `curve_network.json`、`curve_network.step`、`untrimmed_support_surfaces.step`、`trimmed_patch_faces.step` 和互斥的 `final_sewn.step` 或 `diagnostic_sewn.step`；没有缝合几何时保留 `sewing_attempt.json`。manifest 最后生成且不记录自身。缓存二进制版本为 17，算法指纹为 `native-regional-refit-v48-stage1-closure-initialization`，旧缓存明确拒绝。

P0 锁定诊断证据位于 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs\20260729_212344_628__native-region-refit__03_配件_Clay__cand-0179__target\final\`。旧 `4.96138 mm` seam 距离来自 mesh-vertex 配对污染；共同参数下双侧支撑面最大分离为 `4.36e-14 mm`，Curve-on-Surface 偏差为 `4.80e-14 mm`。7 个原始三角形符号变化来自 STEP Face 局部绕序，不应归类为 UV 翻转。Face/Wire 仍有 `BadOrientationOfSubshape`；固定 `0.001 mm` 诊断缝合、STEP 回读和 Creo 导入成功，但 ModelCHECK 失败，因此仍为 `DiagnosticOnly`、`canApply=false`。

阶段 1 当前证据位于 `data/runs/20260730_104812_470__native-region-refit__03_配件_Clay__cand-0179__target/final/`。正式公平求解已进入并解出内部控制点；即使边界门失败，有限、一般 pcurve 闭合且满足 poles 真门的实际低秩面仍以 `IndependentLowRankBoundaryViolationDiagnostic` 保留并进入固定容差诊断链，不再被 P0 Coons 对照替换。四张实际控制网为 9×12、11×10、11×10、9×12，共 436 poles / 68 knots，均为正 Jacobian；最大外部 Curve-on-Surface 误差依次为 `0.103737`、`0.160706`、`0.075830`、`0.079820 mm`，所以四张面仍不可作为产品裁剪面，阶段 2 继续跳过。38 次 SameParameter 检查全部失败、最大偏差 `0.160708 mm`，但固定 `0.001 mm` 诊断缝合仍形成 1 个 Solid、0 条自由边和 0 条多重边。该水密结果只回答“边界不合格时能否强制缝合”，不得称为几何或 Apply 验收。

阶段 2A 证据位于 `data/runs/20260730_151521_082__native-region-refit__03_配件_Clay__cand-0179__target/final/`。闭合预求解已进入但未通过：最低硬残差为 `0.0164339 mm`，继续局部增点后出现 trim 域内 Jacobian 翻转；最坏热区为 patch 1、edge 30、`t=0`、UV=`(1, 0.4002505)`，Curve-on-Surface 偏差 `0.161957 mm`、内部双侧分离 `0.163864 mm`。阶段 2B 未执行，路由为 `Stage3`。最终面片明确携带阶段 1 产物，固定 `0.001 mm` 缝合、STEP 回读和 Creo 诊断继续执行，但 `SameParameter` 38 次全部失败、存在 1 条约 `0.369118 mm` 的意外交线且 ModelCHECK 失败，仍为 `DiagnosticOnly`、`canApply=false`。

当前未完成项仍是自适应局部曲面布局，后续需要继续探索。以保留的五片为直接对照、历史四片为补充，优先级为水密、面数与实际 STEP 体积、光顺性、耗时；仍须满足既定形状和可见表面质量要求。本轮失败策略不再作为默认起点，不能以精确合面工具代替通用布局能力。新探索须处理复杂边界表达成本与局部参数化，避免预设中央面/外围数量；默认保留原 26 条接口、真实特征和未替换区域，不提高公差或修改 Creo 模板。采用同一规则验证 candidate-179、至少三类区域及相邻联合替换，最终确认样本不参与调参；继续报告单方向控制点预算、全部质量门和完整探索耗时。只有通过原有距离、法向、连续性、边界落面、STEP 往返及外部 CAD 检查，才可讨论替换资格。

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
