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

当前阶段：指定目标面数与自适应多曲面两个入口、拟合参考与布局载体解耦、局部四边参数域、逐边一致性分类和输出复杂度报告已进入命令行探针，旧 `fixed` 入口继续保留。candidate-179 的 K=1 历史证据仍是已缝合且拓扑水密、但几何门禁失败的诊断实体，不能作为产品成功。

同一干净提交 `588daa0` 的两次冷运行已经完成并完整保留失败证据：

- 指定 K=4：`data/runs/20260723_020003_candidate_0179_target_k4_local_domains/`。结构采样通过，但 K=4 布局无法形成稳定四边参数域，未进入完整拟合，结果为 `NoValidatedLayout`。
- 自适应最小合格：`data/runs/20260723_020217_candidate_0179_adaptive_minimum_qualified/`。算法生成 K=1 至 K=12；K=2 至 K=12 在四边参数域预检失败，K=1 进入完整拟合后因控制网超出局部支撑包络失败，结果为 `NoProductValidatedResult`，未证明 `MinimumQualified`。

两种模式均未通过全部自动门，因此未运行 Creo，也没有 `cad_check_candidate.step` 或 `final_sewn.step`。下一验收点是改进外边界所有权、内部缝线与四边角点构造，使多 patch 候选能够进入完整拟合，同时改善布局载体质量；不得放宽现有几何和拓扑门禁。GUI 接入仍不在本轮范围内，本任务保持未完成。

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
