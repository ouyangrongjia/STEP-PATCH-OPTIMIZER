# UI Design

项目名称：`step-patch-optimizer`  
界面目标：提供一个面向 STEP/STP 后处理的可交互 GUI，使用户能够导入模型、查看 B-rep 拓扑、识别特征线、手动修正边界约束、预览曲面片合并结果，并导出优化后的 STEP 文件。

---

## 1. UI 设计目标

本项目的 GUI 不是普通模型浏览器，而是服务于 **曲面片合并与边界优化** 的交互式工具。核心目标如下：

1. **模型可视化**  
   支持 STEP/B-rep 模型旋转、缩放、平移、选择和高亮。

2. **拓扑可解释**  
   用户能够查看 face、edge、shell、solid 的数量、编号和属性。

3. **特征线可编辑**  
   自动检测出的特征边允许用户手动锁定、解除锁定和重新检测。

4. **合并过程可控**  
   用户可以预览合并候选区域，调整参数后再执行合并。

5. **结果可验证**  
   合并前后统计、合法性检查、错误信息和导出报告需要在界面中明确展示。

---

## 2. 主界面布局

推荐采用 Qt Widgets + OCCT Viewer 的三栏布局：

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Menu Bar: File | View | Detect | Merge | Validate | Export | Help    │
├──────────────────────────────────────────────────────────────────────┤
│ Tool Bar: Open | Save | Select Face | Select Edge | Detect | Merge   │
├───────────────┬──────────────────────────────────────┬───────────────┤
│ Model Tree    │                                      │ Parameter     │
│               │                                      │ Panel         │
│ - Solids      │                                      │               │
│ - Shells      │             3D Viewer                │ - angle tol   │
│ - Faces       │        OCCT AIS/V3d View             │ - linear tol  │
│ - Edges       │                                      │ - merge mode  │
│ - Groups      │                                      │ - buttons     │
│               │                                      │               │
├───────────────┴──────────────────────────────────────┴───────────────┤
│ Bottom Panel: Log | Inspection | Validation | Report                 │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. 界面区域说明

### 3.1 Menu Bar

| 菜单 | 功能 |
|---|---|
| `File` | 打开 STEP、保存项目、导出 STEP、退出 |
| `View` | 显示/隐藏特征线、显示/隐藏面编号、重置视角 |
| `Detect` | 自动检测特征边、重新检测、清除检测结果 |
| `Merge` | 生成合并候选、预览合并、执行合并、撤销合并 |
| `Validate` | 检查实体合法性、检查 free edge、生成报告 |
| `Export` | 导出 STEP、导出 JSON/CSV/Markdown 报告 |
| `Help` | 快捷键说明、版本信息 |

### 3.2 Tool Bar

工具栏放置高频操作：

```text
Open STEP
Save Project
Select Face
Select Edge
Box Select
Detect Feature Edges
Preview Merge
Apply Merge
Validate
Export STEP
Undo
Redo
```

### 3.3 Model Tree Panel

模型树显示 B-rep 层级结构：

```text
Model
├── Solid 0
│   ├── Shell 0
│   │   ├── Face 0
│   │   ├── Face 1
│   │   └── ...
│   └── Edges
├── Feature Edges
│   ├── Sharp Edges
│   ├── Curvature Edges
│   └── User Locked Edges
└── Merge Candidates
    ├── Candidate 0
    ├── Candidate 1
    └── ...
```

用户点击树节点时，3D Viewer 中对应元素应高亮。

### 3.4 3D Viewer

3D Viewer 是主操作区域，基于 OCCT 的 `AIS_InteractiveContext` 和 `V3d_View` 实现。

需要支持：

```text
模型显示
面选择
边选择
特征线高亮
候选合并区域高亮
合并前后对比显示
错误区域高亮
```

### 3.5 Parameter Panel

参数面板用于调整算法参数：

| 参数 | 控件类型 | 默认值 | 说明 |
|---|---|---:|---|
| `Angular Threshold` | Slider + SpinBox | `25°` | 二面角特征边阈值 |
| `Linear Tolerance` | DoubleSpinBox | `0.001` | 距离容差 |
| `Curvature Threshold` | DoubleSpinBox | 待定 | 曲率突变阈值 |
| `Min Edge Length` | DoubleSpinBox | 待定 | 过滤短边噪声 |
| `Merge Mode` | ComboBox | `Same-domain` | 合并模式 |
| `Preserve Feature Edges` | CheckBox | `true` | 是否保护特征边 |
| `Preserve User Locked Edges` | CheckBox | `true` | 是否保护用户锁定边 |
| `Enable Refit` | CheckBox | `false` | 第一版默认关闭 |

### 3.6 Bottom Panel

底部面板包含：

```text
Log        显示运行日志
Inspect    显示当前选中 face / edge 属性
Validation 显示合法性检查结果
Report     显示合并前后统计
```

---

## 4. 鼠标交互设计

必须支持以下鼠标操作：

| 操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标滚轮 | 放大 / 缩小视图 |
| 鼠标左键点击 | 选择当前模式下的 face 或 edge |
| `Shift + 左键点击` | 多选 face / edge |
| `Ctrl + 左键点击` | 从选择集中移除对象 |
| 鼠标右键点击 | 打开上下文菜单 |
| 鼠标右键拖拽 | 平移视图，可选 |
| 双击元素 | 聚焦并在 Inspect Panel 中显示属性 |

### 4.1 视图操作要求

```text
鼠标中键拖拽：围绕模型中心或当前视点旋转视角
鼠标滚轮向上：放大视图
鼠标滚轮向下：缩小视图
```

视角操作不应改变当前选择状态。

### 4.2 选择模式

界面应提供三种选择模式：

```text
Select Face
Select Edge
Select Candidate Region
```

当前模式影响左键点击的拾取对象。

### 4.3 右键菜单

右键点击 edge 时：

```text
Lock as Feature Edge
Unlock Feature Edge
Show Edge Properties
Select Adjacent Faces
Hide Edge
```

右键点击 face 时：

```text
Add to Merge Group
Remove from Merge Group
Show Face Properties
Select Neighbor Faces
Detect Local Features
```

右键点击 merge candidate 时：

```text
Preview This Candidate
Apply This Candidate
Reject This Candidate
Show Candidate Statistics
```

---

## 5. 键盘快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl + O` | 打开 STEP 文件 |
| `Ctrl + S` | 保存项目文件 |
| `Ctrl + E` | 导出 STEP 文件 |
| `Ctrl + Z` | 撤销 |
| `Ctrl + Y` | 重做 |
| `F` | 切换 Face 选择模式 |
| `E` | 切换 Edge 选择模式 |
| `H` | 显示/隐藏特征线 |
| `M` | 预览合并候选 |
| `V` | 执行合法性检查 |
| `R` | 重置视角 |
| `Delete` | 从当前选择集中移除元素 |
| `Esc` | 清空当前选择 |

---

## 6. 可视化编码规范

为了让用户快速理解模型状态，需要建立稳定的颜色和显示规则。

| 元素 | 建议显示方式 |
|---|---|
| 普通 face | 默认浅灰或材质色 |
| 当前选中 face | 高亮填充 |
| 普通 edge | 细线显示 |
| 自动检测 sharp edge | 粗线高亮 |
| 曲率特征边 | 虚线或另一种高亮样式 |
| 用户锁定边 | 明确高亮，优先级最高 |
| 合并候选区域 | 半透明覆盖 |
| 合并后新增面 | 临时高亮 |
| 错误区域 | 警示高亮 |
| free edge | 强提示高亮 |
| multiple edge | 强提示高亮 |

显示原则：

```text
用户锁定边 > 拓扑错误边 > 自动特征边 > 合并候选边 > 普通边
```

当多个状态重叠时，优先显示更高优先级状态。

---

## 7. 用户工作流

### 7.1 基础工作流

```text
打开 STEP
  ↓
查看模型和统计信息
  ↓
自动检测特征线
  ↓
用户检查特征线
  ↓
手动锁定或解除部分边界
  ↓
生成合并候选
  ↓
预览候选区域
  ↓
执行合并
  ↓
合法性检查
  ↓
导出 STEP 和报告
```

### 7.2 交互式修正工作流

```text
检测结果不理想
  ↓
调整 angular threshold / curvature threshold
  ↓
重新检测特征边
  ↓
手动锁定关键棱边
  ↓
重新生成合并候选
  ↓
局部执行合并
```

### 7.3 合并失败工作流

```text
执行合并
  ↓
Validation 失败
  ↓
显示错误区域
  ↓
自动回滚到合并前状态
  ↓
提示失败原因
  ↓
用户调整参数或锁定更多边
```

---

## 8. Inspect Panel 设计

### 8.1 选中 Face 时

显示：

```text
Face ID
Surface Type
Area
Center
Average Normal
Boundary Edge Count
Neighbor Face Count
Is Selected
Merge Candidate ID
```

### 8.2 选中 Edge 时

显示：

```text
Edge ID
Length
Adjacent Face Count
Adjacent Face IDs
Dihedral Angle
Curvature Jump
Is Free Edge
Is Multiple Edge
Is Feature Edge
Is User Locked
```

### 8.3 选中 Merge Candidate 时

显示：

```text
Candidate ID
Face Count
Internal Edge Count
Boundary Edge Count
Target Surface Type
Estimated Error
Merge Cost
Can Apply
Reject Reason
```

---

## 9. Validation Panel 设计

Validation Panel 显示合并后的合法性检查结果。

建议内容：

```text
Shape Status: Valid / Invalid
Solid Count
Shell Count
Face Count Before / After
Edge Count Before / After
Free Edge Count
Multiple Edge Count
Non-manifold Edge Count
Max Deviation
Mean Deviation
Feature Edge Preserve Rate
```

如果存在错误，需要提供：

```text
错误类型
错误数量
错误元素 ID
是否可定位
是否可高亮
推荐处理方式
```

---

## 10. Report Panel 设计

Report Panel 用于给实验、组会和论文记录提供数据。

### 10.1 合并统计

```text
Input file: case_001.step
Output file: case_001_optimized.step
Face count before: 2850
Face count after: 940
Face reduction ratio: 67.02%
Edge count before: 5712
Edge count after: 1881
Free edge count before: 0
Free edge count after: 0
Solid count before: 1
Solid count after: 1
Runtime: 3.42 s
```

### 10.2 导出格式

支持：

```text
JSON
CSV
Markdown
```

---

## 11. 状态管理与撤销重做

所有修改模型或约束的操作都必须通过 Command 系统执行。

```text
LoadStepCommand
DetectFeatureCommand
LockEdgeCommand
UnlockEdgeCommand
GenerateMergeCandidateCommand
ApplyMergeCommand
ValidateShapeCommand
ExportStepCommand
```

每个命令至少包含：

```cpp
class Command {
public:
    virtual bool execute() = 0;
    virtual bool undo() = 0;
    virtual std::string name() const = 0;
};
```

撤销/重做要求：

```text
合并操作必须可撤销
锁定/解锁特征边必须可撤销
参数改变不一定进入撤销栈，但应记录在项目文件中
导出操作不进入撤销栈
```

---

## 12. 项目文件保存

用户交互状态应保存为项目文件，例如：

```text
case_001.spo.json
```

保存内容：

```json
{
  "source_step": "case_001.step",
  "current_step": "case_001_optimized.step",
  "parameters": {
    "angular_threshold_deg": 25.0,
    "linear_tolerance": 0.001,
    "merge_mode": "same_domain"
  },
  "locked_edges": [12, 30, 44],
  "merge_candidates": [0, 1, 2],
  "applied_operations": []
}
```

---

## 13. 错误提示设计

错误提示应明确、可定位、可恢复。

| 错误 | 提示内容 | 建议操作 |
|---|---|---|
| STEP 读取失败 | 文件无法读取或格式不支持 | 检查文件路径和格式 |
| 模型不是实体 | 当前模型不是 closed solid | 先运行修复或检查 free edge |
| 合并失败 | 合并后拓扑无效 | 自动回滚，调整参数 |
| 特征边为空 | 当前阈值下未检测到特征边 | 降低角度阈值或手动选择 |
| 导出失败 | STEP 写出失败 | 检查输出路径和权限 |

---

## 14. MVP UI 范围

第一版 GUI 必须完成：

```text
1. 打开 STEP 文件。
2. 显示 B-rep 模型。
3. 鼠标中键拖拽旋转视角。
4. 鼠标滚轮放大/缩小视图。
5. 左键选择 face / edge。
6. 显示 face / edge 属性。
7. 自动检测并高亮 sharp edge。
8. 右键锁定/解锁特征边。
9. 生成并预览 same-domain 合并候选。
10. 执行合并并显示前后统计。
11. 运行合法性检查。
12. 导出 STEP 文件。
```

第一版可以暂缓：

```text
复杂主题样式
多视口对比
局部截面分析
网格级特征检测可视化
高级 NURBS 重拟合交互
批量处理 GUI
```

---

## 15. 验收标准

UI 完成标准：

```text
1. 用户可以通过菜单或快捷键打开 STEP。
2. 模型能够稳定显示、旋转、缩放和平移。
3. 鼠标中键拖拽能够旋转视角。
4. 鼠标滚轮能够放大/缩小视图。
5. 用户可以选择 face 和 edge。
6. 选中元素属性能显示在 Inspect Panel。
7. 特征边检测结果能高亮显示。
8. 用户可以手动锁定和解锁边。
9. 合并候选区域能预览。
10. 合并后统计信息能显示。
11. 合法性检查结果能显示。
12. 用户可以导出优化后的 STEP。
```
