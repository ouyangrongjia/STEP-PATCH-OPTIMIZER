# Experiment Protocol

项目名称：`step-patch-optimizer`  
实验目标：系统评估 STEP 曲面片合并与边界优化算法在面片压缩、特征边保持、实体合法性、几何误差和运行效率上的表现，为工程迭代、组会汇报和后续论文写作提供可复现实验依据。

---

## 1. 实验目标

本项目实验不只统计“合并了多少面”，而是验证以下问题：

1. **曲面片数量是否有效减少**  
   合并后 face 数量、edge 数量是否明显下降。

2. **模型结构边界是否合理保留**  
   正方体棱边、圆柱边界、凸起边界、凹陷边界是否仍然存在。

3. **实体合法性是否保持**  
   合并后是否仍为 closed solid，是否新增 free edge、multiple edge 或非流形结构。

4. **几何偏差是否可控**  
   合并后的模型是否仍贴近原始模型。

5. **交互修正是否有效**  
   用户手动锁定特征边后，能否避免过度合并。

6. **算法是否具有稳定性和可复现性**  
   同一输入和参数下是否产生一致输出。

---

## 2. 实验数据集

### 2.1 数据集分类

建议至少准备以下类型的测试模型：

| 类型 | 示例 | 用途 |
|---|---|---|
| 规则几何体 | cube、box、cylinder、sphere | 检查基础特征边识别和同域合并 |
| 组合 CAD 模型 | 带孔方块、圆柱凸台、倒角零件 | 检查多曲面类型和结构边界 |
| 高细分 STEP | AutoSurface 或逆向导出的碎片化模型 | 检查真实任务中的面片压缩效果 |
| 潮玩模型局部 | 发尖、耳朵、凸起纹理、凹陷区域 | 检查弱特征线保持能力 |
| 烂面修补后模型 | 老师修复后的 closed solid STEP | 检查后处理链路兼容性 |
| 失败案例 | 自交、碎片严重、边界复杂模型 | 分析算法边界和失败原因 |

### 2.2 数据目录建议

```text
experiments/
├── datasets/
│   ├── primitive/
│   │   ├── cube.step
│   │   ├── cylinder.step
│   │   └── sphere.step
│   ├── cad_parts/
│   ├── toy_models/
│   ├── repaired_cases/
│   └── failure_cases/
│
├── configs/
│   ├── default.json
│   ├── strict_feature.json
│   ├── aggressive_merge.json
│   └── ablation_no_user_lock.json
│
├── results/
│   ├── csv/
│   ├── json/
│   ├── reports/
│   └── screenshots/
│
└── notebooks/
    └── analysis.ipynb
```

### 2.3 数据命名规范

建议统一命名：

```text
类别_编号_简述.step
```

示例：

```text
primitive_001_cube.step
cad_003_hole_block.step
toy_007_hair_tip.step
repaired_012_closed_solid.step
failure_002_intersection.step
```

---

## 3. 对比方法

### 3.1 Baseline 0：原始 STEP

不做任何合并，直接统计原始模型。

作用：作为所有指标的基准。

### 3.2 Baseline 1：无特征保护的 Same-domain 合并

只执行同域合并，不使用自动特征边和用户锁定边保护。

作用：观察过度合并风险。

### 3.3 Baseline 2：自动特征保护合并

使用二面角和拓扑规则自动识别特征边，再执行合并。

作用：评估自动算法效果。

### 3.4 Proposed：自动特征保护 + 用户约束修正

在自动检测基础上，用户手动锁定或解除部分边界，再执行合并。

作用：评估交互式 GUI 的实际价值。

### 3.5 后期扩展方法

如果后续实现曲率特征和曲面重拟合，可以增加：

```text
Baseline 3: 自动特征保护 + 曲率特征边
Baseline 4: Same-domain 合并 + 局部 NURBS refit
Proposed v2: 特征感知区域生长 + 局部重拟合
```

---

## 4. 实验变量

### 4.1 主变量

| 变量 | 取值示例 | 说明 |
|---|---|---|
| `angular_threshold_deg` | `10, 20, 30, 45` | 二面角特征边阈值 |
| `linear_tolerance` | `1e-5, 1e-4, 1e-3` | 几何距离容差 |
| `merge_mode` | `same_domain, region_grow` | 合并模式 |
| `preserve_feature_edges` | `true / false` | 是否保护自动特征边 |
| `preserve_user_locked_edges` | `true / false` | 是否保护用户锁定边 |
| `enable_curvature_feature` | `true / false` | 是否启用曲率特征 |
| `enable_refit` | `true / false` | 是否启用重拟合 |

### 4.2 默认参数

第一版建议默认配置：

```json
{
  "angular_threshold_deg": 25.0,
  "linear_tolerance": 0.001,
  "merge_mode": "same_domain",
  "preserve_feature_edges": true,
  "preserve_user_locked_edges": true,
  "enable_curvature_feature": false,
  "enable_refit": false
}
```

---

## 5. 评价指标

### 5.1 面片压缩指标

| 指标 | 定义 |
|---|---|
| `face_count_before` | 合并前 face 数量 |
| `face_count_after` | 合并后 face 数量 |
| `face_reduction_ratio` | `(before - after) / before` |
| `edge_count_before` | 合并前 edge 数量 |
| `edge_count_after` | 合并后 edge 数量 |
| `edge_reduction_ratio` | `(before - after) / before` |

### 5.2 拓扑合法性指标

| 指标 | 定义 |
|---|---|
| `solid_count_before` | 合并前实体数量 |
| `solid_count_after` | 合并后实体数量 |
| `shell_count_before` | 合并前 shell 数量 |
| `shell_count_after` | 合并后 shell 数量 |
| `free_edge_count_before` | 合并前自由边数量 |
| `free_edge_count_after` | 合并后自由边数量 |
| `multiple_edge_count_before` | 合并前多重边数量 |
| `multiple_edge_count_after` | 合并后多重边数量 |
| `is_valid_solid_after` | 合并后是否仍为合法实体 |

### 5.3 特征保持指标

| 指标 | 定义 |
|---|---|
| `feature_edge_count_before` | 合并前检测到的特征边数量 |
| `feature_edge_count_after` | 合并后保留的特征边数量 |
| `user_locked_edge_count` | 用户锁定边数量 |
| `locked_edge_preserve_rate` | 用户锁定边保留率 |
| `sharp_edge_preserve_rate` | 自动 sharp edge 保留率 |
| `manual_correction_count` | 用户手动修正次数 |

规则模型可以使用人工标注的特征边作为 Ground Truth，例如：

```text
cube: 12 条主棱边
box_with_hole: 外轮廓边 + 孔边界
cylinder: 上下圆边界 + 侧面连续区域边界
```

### 5.4 几何误差指标

| 指标 | 定义 |
|---|---|
| `max_deviation` | 合并后模型到原始模型的最大采样距离 |
| `mean_deviation` | 平均采样距离 |
| `rms_deviation` | 均方根偏差 |
| `normal_deviation_mean` | 平均法向偏差 |
| `normal_deviation_max` | 最大法向偏差 |

第一版如果暂不做重拟合，可以先将误差指标作为预留字段；same-domain 合并理论上不应引入明显几何偏差，但仍需要记录。

### 5.5 性能指标

| 指标 | 定义 |
|---|---|
| `load_time_ms` | STEP 读取时间 |
| `index_time_ms` | 拓扑索引构建时间 |
| `feature_detect_time_ms` | 特征边检测时间 |
| `candidate_time_ms` | 合并候选生成时间 |
| `merge_time_ms` | 合并执行时间 |
| `validation_time_ms` | 合法性检查时间 |
| `export_time_ms` | STEP 导出时间 |
| `total_time_ms` | 总运行时间 |

---

## 6. 实验流程

### 6.1 单模型实验流程

```text
1. 导入原始 STEP。
2. 记录原始 face / edge / shell / solid 数量。
3. 构建拓扑邻接图。
4. 运行特征边检测。
5. 可选：用户手动锁定或解除特征边。
6. 生成合并候选。
7. 预览合并候选。
8. 执行合并。
9. 运行合法性检查。
10. 计算合并前后统计指标。
11. 导出优化 STEP。
12. 重新读取导出的 STEP，确认可读。
13. 保存 JSON / CSV / Markdown 报告。
14. 保存 GUI 截图。
```

### 6.2 批量实验流程

```text
for each dataset in datasets:
    for each config in configs:
        load STEP
        run feature detection
        run merge
        run validation
        export result
        save metrics
        save report
```

建议命令行工具：

```text
tools/batch_optimize --input experiments/datasets --config experiments/configs/default.json --output experiments/results
```

---

## 7. CSV 结果字段

建议每条实验记录保存为一行：

```csv
case_id,input_file,config_name,face_before,face_after,face_reduction,edge_before,edge_after,edge_reduction,solid_before,solid_after,free_edge_before,free_edge_after,multiple_edge_before,multiple_edge_after,feature_before,feature_after,locked_edges,locked_preserve_rate,max_deviation,mean_deviation,total_time_ms,valid_after,export_success,notes
```

字段说明：

| 字段 | 说明 |
|---|---|
| `case_id` | 测试案例编号 |
| `input_file` | 输入 STEP 路径 |
| `config_name` | 参数配置名称 |
| `face_before` | 合并前面数 |
| `face_after` | 合并后面数 |
| `face_reduction` | 面数压缩率 |
| `edge_before` | 合并前边数 |
| `edge_after` | 合并后边数 |
| `solid_before` | 合并前实体数 |
| `solid_after` | 合并后实体数 |
| `free_edge_after` | 合并后自由边数 |
| `feature_after` | 合并后特征边数 |
| `locked_preserve_rate` | 锁定边保留率 |
| `valid_after` | 合并后是否合法 |
| `notes` | 失败原因或人工备注 |

---

## 8. JSON 报告格式

单个模型建议保存详细 JSON：

```json
{
  "case_id": "toy_007_hair_tip",
  "input_file": "experiments/datasets/toy_models/toy_007_hair_tip.step",
  "output_file": "experiments/results/step/toy_007_hair_tip_optimized.step",
  "config": {
    "angular_threshold_deg": 25.0,
    "linear_tolerance": 0.001,
    "merge_mode": "same_domain",
    "preserve_feature_edges": true,
    "preserve_user_locked_edges": true
  },
  "before": {
    "face_count": 2850,
    "edge_count": 5712,
    "solid_count": 1,
    "free_edge_count": 0,
    "multiple_edge_count": 0
  },
  "after": {
    "face_count": 940,
    "edge_count": 1881,
    "solid_count": 1,
    "free_edge_count": 0,
    "multiple_edge_count": 0
  },
  "metrics": {
    "face_reduction_ratio": 0.6702,
    "edge_reduction_ratio": 0.6707,
    "max_deviation": 0.0,
    "mean_deviation": 0.0,
    "locked_edge_preserve_rate": 1.0,
    "valid_after": true
  },
  "runtime_ms": {
    "load": 320,
    "index": 180,
    "feature_detection": 95,
    "candidate_generation": 120,
    "merge": 840,
    "validation": 210,
    "export": 300,
    "total": 2065
  },
  "notes": "same-domain merge succeeded"
}
```

---

## 9. 截图与可视化记录

每个重要案例建议保存以下截图：

```text
input_model.png                 原始模型
feature_edges_detected.png       自动检测特征线
user_locked_edges.png            用户锁定边
merge_candidates.png             合并候选预览
optimized_model.png              合并后模型
validation_errors.png            如果有错误，保存错误高亮图
before_after_comparison.png      前后对比图
```

截图命名建议：

```text
case_id_stage.png
```

示例：

```text
toy_007_hair_tip_feature_edges.png
toy_007_hair_tip_merge_candidates.png
toy_007_hair_tip_optimized.png
```

---

## 10. 消融实验设计

### 10.1 特征保护消融

比较：

```text
A: 不保护特征边
B: 保护自动 sharp edge
C: 保护自动 sharp edge + 用户锁定边
```

观察：

```text
面片减少率
特征边保留率
错误合并数量
合法性检查是否通过
```

### 10.2 阈值敏感性实验

测试不同角度阈值：

```text
10°, 20°, 30°, 45°
```

观察：

```text
检测到的特征边数量
合并候选数量
最终面片减少率
是否过度保护导致合并不足
是否保护不足导致过度合并
```

### 10.3 用户交互价值实验

比较自动结果与人工修正后结果：

```text
Auto only
Auto + manual lock
Auto + manual unlock
Auto + manual correction
```

观察：

```text
最终特征边合理性
误合并数量
用户操作次数
总耗时
```

---

## 11. 定性评估标准

除了量化指标，还需要人工评估曲面片分布是否合理。

### 11.1 评分项

| 评分项 | 说明 | 分值 |
|---|---|---:|
| 特征边保留 | 棱边、凸起、凹陷边界是否被保留 | 1-5 |
| 面片分布合理性 | 大平面/连续曲面是否减少碎片边界 | 1-5 |
| 过度合并情况 | 是否跨越真实结构边界 | 1-5 |
| 欠合并情况 | 是否仍保留大量无意义小 patch | 1-5 |
| 实用性 | 是否方便后续 CAD 编辑或检测 | 1-5 |

### 11.2 评分说明

```text
5 分：结果非常合理，特征边清晰，冗余面片明显减少。
4 分：整体合理，存在少量不影响使用的问题。
3 分：部分区域合理，但仍有明显欠合并或局部误合并。
2 分：结果较差，存在多处边界不合理。
1 分：结果不可用，拓扑或特征结构严重破坏。
```

---

## 12. 成功标准

MVP 实验成功标准：

```text
1. 至少 5 个测试 STEP 能成功读取和显示。
2. 所有规则几何体合并后仍为合法实体。
3. cube 的 12 条主棱边能够被识别并保留。
4. 至少 3 个过细分模型的 face 数量减少 30% 以上。
5. 合并后 free_edge_count 不增加。
6. 合并后 solid_count 保持不变。
7. 用户锁定边保留率达到 100%。
8. 导出的 STEP 可以被本程序重新读取。
9. 每次实验能输出 JSON 和 CSV 统计结果。
10. GUI 能保存关键截图用于汇报。
```

---

## 13. 失败案例记录

每个失败案例都要记录，不要只保存成功结果。

失败记录包括：

```text
输入文件
参数配置
失败阶段
错误信息
是否自动回滚
错误元素 ID
截图
人工分析
下一步修复建议
```

失败类型分类：

| 类型 | 说明 |
|---|---|
| `read_error` | STEP 读取失败 |
| `index_error` | 拓扑索引构建失败 |
| `feature_error` | 特征边检测异常 |
| `merge_error` | 合并执行失败 |
| `validation_error` | 合并后模型不合法 |
| `export_error` | STEP 导出失败 |
| `quality_error` | 合法但边界分布不合理 |

---

## 14. 实验报告模板

每个阶段可以生成一份 Markdown 报告：

```markdown
# Experiment Report: case_id

## Input
- File:
- Source:
- Model type:
- Notes:

## Parameters
- angular_threshold_deg:
- linear_tolerance:
- merge_mode:
- preserve_feature_edges:

## Before Optimization
- Face count:
- Edge count:
- Solid count:
- Free edge count:

## After Optimization
- Face count:
- Edge count:
- Solid count:
- Free edge count:

## Metrics
- Face reduction ratio:
- Edge reduction ratio:
- Feature preserve rate:
- Runtime:
- Valid after merge:

## Screenshots
- Input model:
- Feature edges:
- Merge candidates:
- Optimized result:

## Analysis
- Successes:
- Problems:
- Next steps:
```

---

## 15. 周报可用结论格式

每周汇报建议固定格式：

```text
本周目标：
完成 STEP 后处理中的特征感知曲面片合并实验。

完成内容：
1. 完成 X 个模型的 STEP 读取与拓扑统计。
2. 实现/验证二面角特征边检测。
3. 在 Y 个模型上完成 same-domain 合并。
4. 合并后平均 face reduction ratio 为 Z%。
5. 合并后 free_edge_count 未增加，solid_count 保持稳定。

发现问题：
1. 部分弱凸起边界仅靠二面角无法稳定识别。
2. 部分 NURBS patch 不同域但几何近似连续，same-domain 合并无法处理。

下周计划：
1. 增加曲率突变特征边检测。
2. 增加用户锁定边后的合并对比实验。
3. 设计局部 NURBS refit 的接口与测试样例。
```

---

## 16. 版本化实验管理

实验结果需要和代码版本绑定。

建议每次实验记录：

```text
git_commit_hash
program_version
occt_version
qt_version
config_file
input_file_hash
output_file_hash
```

这样后续写论文或复现实验时，不会出现结果来源不清的问题。
