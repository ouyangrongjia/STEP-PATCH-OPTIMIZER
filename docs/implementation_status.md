# 实现状态与待办清单

> 文档定位：本文件用于记录当前工程实现进度、已完成功能、部分完成模块、待办事项与验证方式。  
> 长期模块规划放在 `module_design.md`，本文件随开发进度持续更新。

---

## 1. 当前版本概览

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

当前重点已经从”搭建项目结构”转向：

```text
1. Stage 3A-Fix：PlaneRegionMerge 导出稳定性收口（唯一优先级）。
2. 暂停扩展球面/圆柱/圆锥/自由曲面真实合并。
3. 冻结候选检测增强和自由曲面路线。
```

### 1.1 当前关键诊断（2026-05-27）

```text
GUI 显示”看起来连续” ≠ STEP/B-rep 拓扑合法。

平面重建后，boundary wire / pcurve / orientation / 闭合关系不正确
→ 外部软件（非 OCCT）重新解析 STEP 时暴露为缺面、飞面、无限平面、开壳。

这不是渲染问题，而是 B-rep 拓扑 / 裁剪边界不稳定问题。
```

因此当前唯一优先级是 Stage 3A-Fix：
PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild。

---

## 2. 最新进度同步

本次状态更新确认以下事项已经完成：

```text
1. UnlockEdgeCommand 已补充 document 校验。
2. 用户锁边进入 protectedEdges 的测试已补充。
3. GUI 手动流程验证已通过。
4. MergePatchCommand undo 时清空锁边状态是当前确认的正确语义。
5. Stage 2 Generic Merge Candidate Framework 已完成：MergeCandidate / MergePlanner / MergeRegionGrower 可生成 PlaneLike 候选区域。
6. Stage 2.5 Candidate GUI Preview 已完成：支持 Top N、显示全部非隐藏候选、按 ID 高亮和清除候选高亮。
7. Stage 2.6 Candidate Selection / Rejection 已完成：支持候选区域点击选择、接受、拒绝、隐藏、恢复和状态统计。
8. Stage 3-0 Analytic RegionMerger Framework Preparation 已完成：已预留统一结果、选项、失败原因和解析图元 merger stub。
9. Stage 3A PlaneRegionMerge 已完成基础可用版：支持 PlaneLike candidate 的真实 planar trimmed face 替换、Command 层执行、undo/redo、批量平面候选合并和 BRepCheck 报告。
10. PlaneRegionMerge 已修复合并后实体数丢失问题：当前通过原 shape 上的 face 级 reshape 保留 solid/shell 容器，并对候选外边界执行受限 edge-only same-domain 简化，减少共线边界分段。
11. Stage 2.7 Face / Candidate Inspect 已完成：点击 face 可查看 surface type、候选归属、candidate type/status/risk/metrics，并支持未命中原因提示。
12. Stage 2.8 Analytic Primitive Candidate Detection 已完成基础版：PlaneLike 保持原行为，CylinderLike/SphereLike/ConeLike 支持基础检测，TorusLike/Freeform 类型保留通道。
13. Stage 2.9 Multi-type Candidate Preview 已完成：报告、模型树、按类型筛选、Viewer 多类型配色和 Face Inspect 已支持 PlaneLike / CylinderLike / SphereLike / ConeLike / TorusLike / FreeformG1 / FreeformG2 / Unknown 的显示通道。
14. Stage 2.8 Enhancement A 已完成：CylinderLike 支持对 B-spline / Bezier / SurfaceOfRevolution 近似圆柱 face 做保守采样拟合检测；CylinderLike 仍要求至少 2 个 face 形成可合并候选；本阶段只生成候选，不执行 CylinderRegionMerge。
15. Stage 2.8 Enhancement B 已完成：ConeLike 支持对 B-spline / Bezier / SurfaceOfRevolution 近似圆锥/圆台 face 做保守采样拟合检测；ConeLike 仍要求至少 2 个 face 形成可合并候选；本阶段只生成候选，不执行 ConeRegionMerge。
16. Stage 3-S Shared Primitive Result Fields 已完成：RegionMergeResult 已新增通用 primitive 参数字段（primitive_center_x/y/z、primitive_axis_x/y/z、primitive_radius、primitive_secondary_radius、primitive_angle_degrees、primitive_fit_error）；Stage 3A PlaneRegionMerge 仍保持原有行为，成功时填充 primitive_axis_* 和 primitive_fit_error；Cylinder / Sphere / Cone / Torus 真实合并仍未实现；后续 Stage 3D / Stage 3B 将复用这些字段。
17. Stage 3D SphereRegionMerge 已完成稳定版调整：SphereLike candidate 合并不再手工构造 spherical trimmed face，也不再使用 ReShape 删除 face；当前改为只放开候选内部边并保护其他所有边，然后调用 OCCT `ShapeUpgrade_UnifySameDomain` 合并同域球面片，避免手工球面边界导致缺面和飞线；支持所有已接受 SphereLike candidates 的批量合并；支持全部可合并 SphereLike candidates 的实验性批量合并；支持 Command 层执行和 undo/redo；支持 AppController 和 GUI 三个入口；写入 RegionMergeResult primitive_center / primitive_radius / primitive_fit_error；Cylinder / Cone / Torus 真实合并仍未实现。
18. SphereRegionMerge 一键批量合并已增加防护：一键全部可合并球面候选会跳过 High risk 和单 face 候选；批量合并保护候选外部边和其他所有边，只允许候选内部边被同域合并消除；若结果丢失拓扑、solid 数变化或 face 数未下降则失败回滚。
19. GUI 主工具栏已按功能收敛为下拉入口：选择、候选显示、候选状态、合并、检查/导出；候选显示下拉中补充了直接显示 PlaneLike / SphereLike 候选的入口，避免工具栏横向溢出。
20. GUI 平面候选入口已区分“PlaneLike 预览候选”和“可真实平面合并候选”：严格合并入口只显示原生 `GeomAbs_Plane`、边界有效、非隐藏/非拒绝的候选；B-spline backed planar-like 候选会明确报告为预览专用，不再显示 Unknown 失败原因。
21. Stage 3A-Fix T3 Unsafe Candidate Rejection Report 已完成：`RegionMergeFailureReason` 已有稳定字符串转换；平面/球面候选合并报告会输出 candidate、failure reason、message、统计、BRepCheck 和失败时 `document was not modified / rollback applied`。
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
| STEP 写出 | 已完成 | 支持导出 STEP |
| STEP 二次读取验证 | 已完成 | 导出后可重新读取验证 |
| 项目文件保存 | 未完成 | `ProjectSerializer` 仍待实现 |
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
| 圆角起止线识别 | 未完成 | 后续由 BoundaryClassifier 增强 |
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
| `MergePlanner` | 已完成基础版 | 基于特征边和锁边生成 PlaneLike 候选区域 |
| `MergeRegionGrower` | 已完成基础版 | 支持平面近似区域生长，protectedEdges 可阻断扩张 |
| `MergeCandidate` 预览 | 已完成基础版 | GUI 可预览 Top N、全部非隐藏候选和指定候选 |
| 候选状态管理 | 已完成基础版 | Pending / Accepted / Rejected / Hidden，仅运行时保存 |
| Face / Candidate Inspect | 已完成基础版 | 点击 face 可查看 surface type、候选归属、candidate type/status/risk/fit_error 等信息 |
| Analytic primitive candidate detection | 已完成增强 B | 支持 PlaneLike、CylinderLike 原生与 B-spline/Bezier/SurfaceOfRevolution 近似检测、SphereLike、ConeLike 原生与 B-spline/Bezier/SurfaceOfRevolution 近似检测；TorusLike/Freeform 类型预留 |
| Multi-type Candidate Preview | 已完成基础版 | 支持多类型统计、按类型筛选、Viewer 类型配色、模型树同步和点击查看 |
| RegionMergeResult / RegionMergeOptions | 已完成基础版 | 统一解析图元区域合并返回值、失败原因和基础选项 |
| Plane/Cylinder/Cone/Sphere/Torus RegionMerger stub | 已完成基础版 | 仅返回 NotImplemented / UnsupportedCandidateType，不修改 B-rep |
| `PlaneRegionMerger` | 已完成基础可用版 | 支持 PlaneLike 候选区域真实替换为 planar trimmed face，保留 solid/shell 容器 |
| 平面候选批量合并 | 已完成基础版 | 支持合并当前候选、合并所有已接受平面候选、一键合并全部可合并平面候选 |
| 平面合并边界简化 | 已完成基础版 | 对候选外边界执行受限 edge-only same-domain 简化，减少共线边界分段 |
| `PlaneRegionMergeCommand` | 已完成基础版 | 平面候选合并通过 Command 层执行，支持 undo/redo |
| `PlaneRegionBatchMergeCommand` | 已完成基础版 | 批量平面候选合并通过 Command 层执行，支持 undo/redo |
| Stage 3-S Shared Primitive Fields | 已完成 | RegionMergeResult 已新增 9 个通用 primitive 参数字段，供后续 Sphere / Cylinder / Cone / Torus 复用 |
| `SphereRegionMerger` | 已完成稳定版调整 | 支持 SphereLike 候选区域通过 OCCT same-domain unifier 消除内部边，不再手工构造 spherical trimmed face；批量路径保护候选外边界和其他所有边，避免缺面和飞线 |
| `SphereRegionMergeCommand` | 已完成基础版 | 球面候选合并通过 Command 层执行，支持 undo/redo |
| `SphereRegionBatchMergeCommand` | 已完成基础版 | 批量球面候选合并通过 Command 层执行，支持 undo/redo |
| `CylinderRegionMerger` | 未完成 | 当前仍为 stub |
| `ConeRegionMerger` | 未完成 | 当前仍为 stub，后置 |
| `TorusRegionMerger` | 未完成 | 当前仍为 stub，可选 |
| `SurfaceRefitter` | 未完成 | 当前为后续研究增强方向 |

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
| MergePlanner / MergeRegionGrower 测试 | 已完成基础版 | 覆盖候选生成、protectedEdges 阻断、min_region_faces 和预览不改模型 |
| MergeCandidate 状态测试 | 已完成基础版 | 覆盖 Pending 默认状态、状态切换、Hidden 过滤和 stats 不变 |
| RegionMerger stub 测试 | 已完成基础版 | 覆盖 NotImplemented、UnsupportedCandidateType、Rejected/Hidden 和 stats 不变 |
| PlaneRegionMerger 测试 | 已完成基础版 | 覆盖非法候选、边界无效、NURBS-backed 平面区域、solid 容器保留和边界分段简化 |
| PlaneRegionMergeCommand 测试 | 已完成基础版 | 覆盖失败不污染文档、成功后 undo/redo |
| PlaneRegionBatchMergeCommand 测试 | 已完成基础版 | 覆盖批量平面合并成功后的 undo/redo |
| Face / Candidate Inspect 测试 | 已完成基础版 | 覆盖 surface type、候选归属、NotInCandidate 和 stats 不变 |
| Analytic Candidate Detection 测试 | 已完成增强 B | 覆盖 CylinderLike 原生与 NURBS-backed 近似检测、SphereLike/ConeLike 基础检测、NURBS-backed ConeLike 近似检测、近似圆柱不误判为 ConeLike、protected edge 阻断和 ID 唯一 |
| Candidate Type Statistics 测试 | 已完成基础版 | 覆盖多类型统计、Hidden 过滤、按类型筛选和 PlaneLike 合并过滤 |
| SphereLike 一键合并过滤测试 | 已完成基础版 | 覆盖一键球面候选过滤会跳过 High risk 和单 face 候选；球面合并不再依赖 boundary wire 闭合，而是基于候选内部边做同域合并 |
| AppController 打开新文档清历史测试 | 已完成 |
| GUI 自动化测试 | 未完成 | 当前主要依赖手动验证 |
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
4. PlaneRegionMergeCommand
5. PlaneRegionBatchMergeCommand
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

## P0：稳定性收口

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 手动完整验证 GUI 主流程 | 已完成 | 打开、选择、多选、锁边、合并、撤销、重做、验证、导出 |
| 确认 MergePatchCommand undo 锁边语义 | 已完成 | 已确认撤销合并时清空锁边是正确语义 |
| 补 UnlockEdgeCommand document 校验 | 已完成 | 无模型时解锁返回错误 |
| 补充用户锁边进入 protectedEdges 的测试 | 已完成 | 已有测试验证锁边会贡献 protectedEdges |
| 复杂 STEP 样例回归测试 | 待做 | 选择 3-5 个潮玩件或碎片 STP 样例 |

## P1：合并候选规划

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现 MergeCandidate 数据结构 | 已完成基础版 | 能表达候选区域 face 集合、边界、风险说明和运行时状态 |
| 实现 MergePlanner 基础候选生成 | 已完成基础版 | 能从 TopologyGraph 生成 PlaneLike 候选区域 |
| 实现 MergeRegionGrower 基础区域生长 | 已完成基础版 | 能根据特征边/锁边阻断区域扩张 |
| GUI 显示候选区域 | 已完成基础版 | 可高亮 Top N、全部非隐藏候选和指定候选 |
| 用户接受/拒绝候选区域 | 已完成基础版 | 支持选择、接受、拒绝、隐藏、恢复候选；本阶段不应用到 B-rep |

## P2：项目保存与报告

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| 实现 ProjectSerializer | 待做 | 保存 `.spo.json` |
| 保存用户锁边状态 | 待做 | 重开项目后锁边可恢复 |
| 保存参数配置 | 待做 | 重开项目后参数可恢复 |
| 保存操作日志 | 待做 | 项目文件中记录核心操作 |
| 实现 ReportGenerator 基础报告 | 待做 | 输出 face/edge 变化、free/multiple edge、BRepCheck |
| 实现导出批量实验报告 | 待做 | 可用于论文/组会实验记录 |

## P3：高级特征线与重拟合

| 任务 | 状态 | 验收方式 |
|---|---:|---|
| CurvatureEstimator 实用化 | 待做 | 能估计局部曲率变化 |
| BoundaryClassifier 实用化 | 待做 | 能识别圆角起止线、凸凹分界 |
| ridge / valley 检测 | 待做 | 能在潮玩件凸起/凹陷处生成候选特征线 |
| SurfaceRefitter 局部重拟合 | 待做 | 能对局部碎片区域拟合新 B-spline |
| 局部 patch layout 重构 | 待做 | 能减少 tiny/slender patch |
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
11. 点击“预览合并”，选择或接受 PlaneLike 候选区域。
12. 通过“平面合并”下拉菜单执行当前候选、全部已接受候选或全部可合并候选。
13. 确认平面合并后 face/edge 数下降，solid 数不丢失，候选外边界共线分段尽量被简化。
14. 执行合法性检查。
15. 导出 STEP。
16. 确认导出后二次读取校验通过。
```

---

## 8. 近期推荐开发顺序（2026-05-27 修订）

**唯一当前任务：Stage 3A-Fix**

```text
1. 导出级合法性闭环：
   - 每次真实合并后自动执行 BRepCheck_Analyzer
   - ShapeValidator
   - 导出临时 STEP
   - 重新读取临时 STEP
   - 再次统计 solid/shell/face/edge
   - 若失败则 rollback

2. 冻结 PlaneRegionMerge 输入范围：
   - 只允许原生 Plane + 单 outer wire + 无 inner loop + 边界闭合
   - 不跨 protected/locked/feature edge
   - 合并后 solid 数不下降
   - 近似平面降级为候选预览

3. 修复 boundary wire：
   - outer wire 排序
   - edge orientation
   - 3D curve 与 2D pcurve 同步
   - ShapeFix_Wire + ShapeFix_Face + BRepLib::BuildCurves3d
   - 必要时重建 planar pcurve

4. 不安全候选明确拒绝：
   - 多边界环 → 不支持
   - 有洞 → 不支持
   - 边界不闭合 → 不支持
   - 导出重读失败 → 已回滚
   - 近似平面 → 暂不进入真实重建

5. 用真实 STEP 样例做导出重读回归测试。
```

**冻结范围（Stage 3A-Fix 验收通过前不可推进）：**

```text
- Stage 3B CylinderRegionMerge → 冻结
- Stage 3D SphereRegionMerge → 实验性保持，不进一步扩展
- Stage 3C ConeRegionMerge → 冻结
- Stage 3E TorusRegionMerge → 冻结
- Stage 4 Freeform Candidate Detection → 冻结
- Stage 5 Freeform B-spline / Plate Refit → 冻结
```

**3A-Fix 验收通过后：**

```text
1. 将导出闭环机制复用于 SphereRegionMerge。
2. 评估 CylinderRegionMerge 推进条件。
3. 逐步解冻 Stage 4 / Stage 5。
```

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
Analytic primitive candidate detection：已完成增强 B
Stage 2.8 Enhancement A B-spline CylinderLike approximate detection：已完成
Stage 2.8 Enhancement B B-spline ConeLike / FrustumLike approximate detection：已完成
Multi-type candidate preview：已完成基础版
RegionMerger 框架准备：已完成基础版
PlaneRegionMerge：已完成基础可用版（导出稳定性未验证）
平面候选批量合并：已完成基础版
平面合并边界简化：已完成基础版
Stage 3-S Shared Primitive Fields：已完成
SphereRegionMerge：已完成稳定版调整（标记为实验性）
```

**当前唯一优先级 → Stage 3A-Fix：**

```text
PlaneRegionMerge Export-Stable Validation + Safe Boundary Rebuild
- 导出级合法性闭环：待实现
- 输入范围冻结：待实现
- Boundary wire 修复：待实现
- 不安全候选明确拒绝：待实现
- STEP 导出重读回归测试：待做
```

**冻结区域：**

```text
CylinderRegionMerge：冻结
ConeRegionMerge：冻结
TorusRegionMerge：冻结
SphereRegionMerge 进一步扩展：冻结
Freeform Candidate Detection：冻结
Freeform B-spline / Plate Refit：冻结
项目保存恢复：冻结
完整误差评估：冻结
高级特征线：冻结
局部重拟合：冻结
```

总体评价：

```text
当前项目已具备可演示的工程雏形，并已完成 PlaneLike 候选生成、可视化预览、
人工筛选基础和 Stage 3A PlaneRegionMerge 基础可用闭环。
SphereRegionMerge 已完成稳定版调整（基于 same-domain unifier）。

当前唯一待收口项是 Stage 3A-Fix：PlaneRegionMerge 导出稳定性验证与安全边界重建。
在平面合并做到"导出后重读仍然对"之前，暂停所有其他合并类型的扩展。
```
