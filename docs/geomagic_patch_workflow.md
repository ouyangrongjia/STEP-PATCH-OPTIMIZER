# Geomagic Patch 后备流程

本文定义 Geomagic AutoSurface 的当前输入输出合同、预览与 Apply 边界、质量门、命令和产物规范。

状态：显式后备。当前默认产品主线是原生 Surface Reduction。只有用户明确选择高级流程时，才启动 Geomagic 或相关外部 CAD。

## 适用范围

该流程适用于：

- 需要使用 Geomagic Wrap AutoSurface 生成支撑 patch。
- 需要检查历史 patch preview 或 Apply 行为。
- 需要复现外部工具输出、trim、closure 或 Creo 诊断。
- 原生主线无法满足明确任务，且用户接受外部软件依赖。

不适用于：

- 默认候选缩减。
- 仅为查看模型而自动启动外部工具。
- 用 Geomagic 输出替代 STEP 原始边界。
- 在没有最终合并 STEP 时声称完成外部 CAD 验证。

## 总体合同

~~~text
原始 STEP
  -> 选择候选区域
  -> 生成 fitting STL
  -> Geomagic AutoSurface
  -> patch STEP/IGES 与日志
  -> 导入并预览
  -> 以原始 STEP 边界重裁剪
  -> PatchReplacementRepair
  -> StrictTopologyGate 与几何质量门
  -> 导出最终整件 STEP
  -> STEP 往返与外部 CAD
~~~

关键边界：

- 原始 STEP 提供拓扑、边界和当前模型真值。
- fitting STL 只提供第三方拟合输入。
- patch STEP 只提供候选支撑几何或多面 patch。
- 最终整件 STEP 只能由 Apply 成功后的当前模型导出。

任何阶段成功都不能替代后续阶段。

## 输入来源

### 原始 STEP

必须明确记录：

- 文件路径或运行产物引用。
- 当前 ShapeDocument 版本。
- 候选区域的动态发现方式。
- 原始面集合和边界。

不要把 candidate ordinal 当作跨模型稳定身份。正式脚本应支持 auto 或可追溯的候选选择，而不是在通用代码中写死真实 FaceId。

### fitting STL

支持来源：

- 从 STEP 候选面采样生成。
- 从原始扫描 STL 显式裁剪。
- 从已知运行目录复用并保留来源说明。

默认后备链优先使用从 STEP 候选采样的 fitting mesh，以保持边界来源可解释。原始扫描可作为拟合参考，但不是边界真值。

不得混淆：

- 原始扫描 STL。
- STEP 采样 mesh。
- 缓存 crop STL。
- Geomagic 处理中间 mesh。

### 外部工具

Geomagic 调用需要：

- wrapCore.exe 的有效路径。
- scripts/geomagic_wrap/autosurface_pipeline.py 或显式脚本。
- 可写输出目录。
- 适用的许可证和运行环境。

外部工具不会由默认单元测试自动启动。

## fitting STL 合同

fitting STL 应满足：

- 包含目标候选的拟合区域。
- 单位和坐标系与 STEP 一致。
- 法向和三角形绕序可解释。
- 不用无关大范围网格稀释目标区域。
- 边界扩展、support collar 或 over-cover 参数写入运行记录。

如果使用 support collar：

- collar 用于给 AutoSurface 提供候选边界外的局部支撑。
- 它不能改变最终原始 STEP 边界。
- 角点 clamp、平滑和最大偏移必须受参数约束。
- collar 生成失败应在 Geomagic 前停止。

fitting STL 的拓扑质量只能说明输入 mesh 可用，不能证明最终 B-rep 可替换。

## 运行 Geomagic

基础命令：

~~~powershell
./scripts/run_geomagic_patch.ps1 `
  -InputStl <fitting-stl> `
  -OutputStep <patch-step>
~~~

可选参数：

- WrapCorePath：Geomagic Wrap 核心程序。
- ScriptPath：AutoSurface Python 脚本。
- StrictPatchTarget：要求输出符合严格 patch 目标。
- NoStepStats：跳过输出后的 step_stats；只用于明确诊断。
- Preset：工具构建预设。

脚本设置并恢复 FIT_REGION_* 环境变量，调用外部脚本，并生成：

- patch STEP。
- AutoSurface IGES sidecar。
- fit region 日志。
- 可选 step_stats 输出。

使用默认输出规则时，data/crop_stl 下的相对路径映射到 data/crop_stp。正式实验仍建议显式传入独立运行目录，避免覆盖历史文件。

### Geomagic 配置

示例配置：

scripts/geomagic_wrap/autosurface_config.example.json

配置的有效值应进入 run.json 或实际调用日志。不要只修改示例文件后假设运行采用了新值。

### 成功判定

Geomagic 阶段成功至少表示：

- 外部进程成功结束。
- 预期 patch 文件存在且非空。
- STEP 可以被本项目读取。
- 日志没有明确的脚本异常。

这仍不表示 patch 能 Apply，也不表示商业 CAD 合格。

## patch 导入与预览

PatchImportService 读取 patch STEP，并建立 ImportedPatchInfo。预览阶段应输出：

- 导入是否成功。
- patch 面数和壳体结构。
- 单面或多面分析。
- 边界环和自由边信息。
- 与目标候选的空间关系。
- 可用于后续 Apply 的支撑面或 patch 结构。

预览不得：

- 替换当前 ShapeDocument。
- 进入 CommandHistory。
- 静默修复原始模型。
- 把第三方自由边当作最终边界。

patch 文件可以成功预览但无法应用，这是合法且必须可报告的结果。

## Apply 边界

PatchReplacementCommand 组织一次隔离事务。

### 支撑几何与边界

单面 patch 通常只提供支撑曲面；BoundaryConstrainedPatchBuilder 或 BoundaryConstrainedSurfaceRetrim 依据原始候选边界重建面。

多面 patch 由 MultiFacePatchAnalyzer 和 BoundaryConstrainedMultiSurfaceShellBuilder 检查内部结构，并仍以原始外边界约束装配。

不得直接把 patch 自由边缝入整件模型来规避边界不一致。

### 替换装配

PatchReplacementAssembler 在模型副本上：

1. 移除目标原始面。
2. 插入受边界约束的替换面。
3. 检查方向和边界配对。
4. 形成待修复候选。

任何装配失败都返回原模型不变。

### 受控修复

PatchReplacementRepair 可执行受控 sewing、局部修复和候选选择，但必须：

- 保留修复前后指标。
- 惩罚新增自由边和退化。
- 不以任意扩大容差获得表面成功。
- 不删除 StrictTopologyGate。
- 不把最佳失败结果提交为成功。

点状退化边和真实 open boundary 必须按 Open CASCADE 语义区分；分类修正不能放宽自由边门。

## 质量门

### StrictTopologyGate

Apply 的硬门禁。适用检查包括：

- BRepCheck。
- 壳体和实体结构。
- 自由边。
- 多重使用边。
- 退化边分类。
- 修复前后拓扑退化。
- STEP 往返后的对应检查。

门禁失败时没有 applied STEP 产品结果。

### CommercialCadLikeQualityGate

用于发现拓扑合法但几何不可交付的结果，适用指标包括：

- 原始边界漂移。
- 角部漂移。
- 特征边漂移。
- 边界 gap 的最大值、P95 和 RMS。
- over-cover 与 under-cover。
- internal seam gap。
- 支撑面覆盖与投影稳定性。

阈值和单位必须由实际运行配置记录。不要在文档复制某次实验的完整数值表。

### STEP 往返

最终整件模型写出后重新读取，并复查：

- 面、壳和实体结构。
- BRepCheck。
- 自由边与多重边。
- 曲线在曲面上的偏差。
- 关键 trim 与 seam 诊断是否变化。

原始 patch STEP 的往返不能替代最终整件 STEP 往返。

## 诊断

### closure

PatchClosureDiagnostics 用于定位：

- boundary cycle。
- non-manifold vertex。
- sewing 前后闭合变化。
- 小孔填充或局部闭合效果。

诊断可以指导修复，但不能自行改变成功标准。

### free edge

自由边诊断应记录：

- 长度、容差和退化状态。
- 所属面与邻近原始边界。
- split segment、owner switch 和投影距离。
- 是否在修复后新出现。

需要区分 seam、OCCT degenerated edge 和真实 open edge。

### trim

PatchTrimDiagnostics 覆盖：

- replacement face 数量。
- 无效 trim wire。
- over-cover 与 under-cover 样本。
- boundary gap 与 internal seam gap。
- 最差边标识。
- STEP 往返前后变化。

trim 诊断用于判断失败来自覆盖、边界投影、owner split、内部接缝还是往返。

## 基线脚本

后备链的综合入口：

~~~powershell
./scripts/run_corner_baseline_gate.ps1 `
  -SourceStep <source-step> `
  -CandidateId auto `
  -OutputDir <run-directory>
~~~

只有明确需要真实 Geomagic 时添加：

~~~powershell
-RealGeomagic
~~~

脚本保留 A0、B1、B2、B2.2 和 B2.3 等历史实验选择用于复现；这些名称不是当前产品阶段，也不应重新写入状态流水账。

AllowQualityGateFailure 只允许诊断运行继续归档失败证据，不能把结果标记为可交付。

## 外部 CAD 路由

外部 CAD 分为两类：

### patch 预检查

对象是 Geomagic 原始 patch STEP，用于判断：

- 第三方输出能否导入。
- patch 自身是否存在明显结构问题。

预检查不能证明最终替换模型有效。

### 最终整件检查

对象必须是 Apply 成功并导出的最终整件 STEP。

Creo 批处理入口：

~~~powershell
./scripts/run_creo_step_diagnostic.ps1 `
  -StepPath <applied-step> `
  -OutputDir <diagnostic-directory>
~~~

脚本可执行 STEP 导入和 ModelCHECK，并归档：

- 生成的 Creo part。
- 导入状态。
- ModelCHECK XML 与解析摘要。
- pass、info、warning 和 error 计数。
- 失败检查与短边样本。
- 与 baseline trim 诊断的阶段级关联。

若报告只有 Creo edge/feature id 而没有空间坐标，不得声称已映射到 OCCT edge。需要 Creo API 或额外几何导出才能做空间关联。

## 运行产物

正式运行目录至少包含：

~~~text
data/runs/<run-id>/
├─ run.json
├─ RUN.md
├─ logs/
├─ fitting/
│  └─ candidate_fitting.stl
├─ geomagic/
│  ├─ patch.step
│  ├─ patch_autosurface.igs
│  └─ fit_region.log
├─ apply/
│  ├─ baseline_report.json
│  └─ applied.step
└─ external/
   └─ creo_step_diagnostic_result.json
~~~

具体目录可由脚本调整，但 run.json 必须能定位实际产物。

RUN.md 只摘要：

- 输入来源。
- 实际参数。
- 各阶段状态。
- 最终是否通过。
- 主要失败原因。
- 产物入口。

完整指标放 JSON、CSV 和日志，不复制回本文。

## 失败处理

### 外部进程失败

保留 stdout、stderr、环境参数和部分输出。不得继续导入不存在或不完整的 patch。

### patch 无法导入

记录 StepReader 或结构错误。不要尝试以 IGES 成功替代 STEP 产品合同，除非任务明确只做支撑面诊断。

### 预览通过但 Apply 失败

保留 preview 和 Apply 报告，按 boundary、trim、repair、topology、quality 或 roundtrip 阶段分类。当前模型保持不变。

### StrictTopologyGate 失败

停止产品提交。可以继续生成诊断，但不能导出 applied STEP 作为成功结果。

### 商业 CAD 类门失败

拓扑可能仍合法，但结果不满足交付质量。保留整件诊断文件，并把状态记录为失败或仅诊断。

### Creo 失败

区分：

- STEP 无法导入。
- 导入后不是有效实体。
- ModelCHECK 几何或短边失败。
- 工具或许可失败。

外部工具基础设施失败不能冒充几何失败，反之亦然。

## 安全边界

- 不自动启动 Geomagic、Creo 或其他昂贵外部阶段。
- 不删除 StrictTopologyGate 或 PatchReplacementRepair。
- 不用更宽容差掩盖裂缝。
- 不覆盖既有运行目录。
- 不把真实样例路径、candidate id 或 edge id 写进通用实现。
- 不把截图作为唯一证据。
- 不把 patch 预检查冒充最终整件检查。
- 不把 Open CASCADE 成功冒充跨内核成功。

## 维护规则

本文只维护当前后备合同。新的历史实验、阶段日志和完整指标写入 data/runs 与开发日志；路线状态变化写入 implementation_status.md；形成里程碑时只在 completed_features.md 增加短结论。
