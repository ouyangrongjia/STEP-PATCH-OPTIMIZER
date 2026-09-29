# 项目结构

本文只记录当前目录和目录职责。架构依赖见 module_design.md，当前实现状态见 implementation_status.md。

## 根目录

~~~text
step-patch-optimizer/
├─ .ai-bridge/           本地大模型桥接配置
├─ .claude/              本地代理技能和配置
├─ .gitnexus/            GitNexus 本地索引与启动脚本
├─ .vscode/              VS Code 构建与调试配置
├─ assets/               GUI 和文档使用的静态资源
├─ build/                本地 CMake 构建产物
├─ data/                 样例、缓存和可复现运行证据
├─ docs/                 当前有效的职责型文档
├─ experiments/          隔离实验代码或输入
├─ log/                  本地临时日志
├─ scripts/              构建、测试、验证和实验脚本
├─ src/                  产品源代码
├─ tests/                单元、合同和回归测试
├─ tools/                命令行探针与辅助工具
├─ vcpkg_installed/      本地依赖安装结果
├─ AGENTS.md             本地忽略的大模型协作说明
├─ CLAUDE.md             本地代理入口说明
├─ CMakeLists.txt        CMake 构建入口
├─ CMakePresets.json     Windows MSVC 预设
├─ README.md             项目入口
└─ vcpkg.json            依赖清单
~~~

build、vcpkg_installed、log 及部分本地代理目录不是产品源代码。是否纳入 Git 以 .gitignore 和当前索引为准。

## src

~~~text
src/
├─ app/                  应用入口、窗口编排与工作流状态
├─ brep/                 ShapeDocument、拓扑图和实体索引
├─ command/              可撤销命令与事务历史
├─ common/               Result、配置、容差和通用类型
├─ external/
│  └─ geomagic/          Geomagic 配置、路径和结果协议
├─ feature/              特征边检测
├─ gui/                  三维视图和停靠面板
├─ io/                   STEP/STL 读取与写出
├─ merge/                区域、候选、边界及冻结的原生拟合
├─ patch/                patch 导入、重裁剪、装配、修复和诊断
├─ reduction/            当前 Surface Reduction 主线
├─ stl/                  STL 索引、裁剪与拟合网格
└─ validate/             形状、严格拓扑和商业 CAD 类质量门
~~~

### app

包含 AppController、MainWindow、PatchPreviewWorkflow、应用工作流状态和运行记录器。这里负责调用顺序和 UI 生命周期，不实现几何算法。

### brep

包含 ShapeDocument、FaceIndex、EdgeIndex、TopologyGraph 和 SurfaceTypeProbe。索引只对所属文档版本有效。

### command

包含加载、导出、锁边、候选合并、patch 替换、Surface Reduction 和验证命令。成功命令才能进入 CommandHistory。

### external/geomagic

封装 GeomagicAutoSurfaceBackend、配置、JSON 文件和输出路径解析。第三方结果仍需经过 patch 与 validate 模块。

### merge

包含特征边约束区域、候选筛选、边界分析、同域合并和 SurfaceRefitter。当前搜索策略不在此目录扩展。

### patch

包含导入、预览报告、边界约束构造、多面 patch 分析、替换装配、PatchReplacementRepair、闭合和裁剪诊断。

### reduction

包含 SurfaceReductionEngine、SurfaceReductionFitCache、RestrictedBSplinePatchFitter 和 StlHybridParameterization。

### validate

包含 ShapeValidator、StrictTopologyGate 和 CommercialCadQualityGate。验证层不静默改变候选。

## tests

tests 采用与产品模块相邻的合同和回归测试，覆盖：

- B-rep 与索引。
- 命令事务、撤销和重做。
- 候选与边界分析。
- patch 导入、替换、修复和诊断。
- Surface Reduction 搜索、拟合、缓存和质量门。
- PowerShell 脚本静态合同。
- 真实基线的非硬编码约束。

测试夹具不应写死真实用户路径、一次性 candidate ordinal 或稳定性不足的 FaceId，除非明确作为锁定夹具。

spo_tests 由 CMake 生成并通过 CTest 执行。

## tools

tools 提供面向单一问题的命令行入口，例如：

- STEP 统计与曲线在曲面上检查。
- 原生曲面拟合探针。
- Surface Reduction 探针。
- patch Apply 与角部基线探针。
- Creo Toolkit 实验探针。
- `candidate179_research/`：五片研究基线及其独立数值、STEP/闭合缝校验、计时和显式精确合面辅助工具。

探针用于生成证据和定位失败，不应绕过产品命令的质量合同。

工具输出应进入本次 data/runs 目录或显式实验目录，而不是覆盖输入。

## scripts

脚本按用途分为：

- 环境与依赖：setup_deps.ps1、configure.ps1。
- 日常构建：build_debug.ps1。
- 日常测试：test.ps1。
- GUI 启动：run_gui.ps1。
- 仓库验证：verify_spo.ps1。
- 运行产物：run_artifacts.ps1。
- Surface Reduction、Geomagic、patch、Creo 和基线实验脚本。

脚本应：

- 从仓库根目录解析路径。
- 接受显式输出目录或创建独立 run-id。
- 把参数、日志和产物写入运行上下文。
- 不隐式启动昂贵外部 CAD 流程。
- 不用文档同步门禁替代事实维护。

## data

~~~text
data/
├─ baseline_runs/        旧基线与后备路线结果
├─ cache/                可复用内容寻址缓存
├─ crop_igs/             历史裁剪 IGES 产物
├─ crop_stl/             裁剪或 fitting STL 产物
├─ crop_stp/             裁剪或 patch STEP 产物
├─ expected/             测试期望数据
├─ runs/                 统一的可复现运行目录
├─ samples/              小型测试样例
├─ stl/                  STL 输入
└─ stp/                  STEP 输入
~~~

### data/runs

每次正式运行使用独立目录：

~~~text
data/runs/<run-id>/
├─ run.json              机器可读参数、环境、阶段和产物索引
├─ RUN.md                人工摘要
├─ logs/                 构建、测试、探针和外部工具日志
└─ ...                   STEP、STL、JSON、CSV 或图片证据
~~~

run.json 和实际产物是精确实验事实。仓库文档只保留短结论和入口。

### data/cache

缓存只加速可重复计算，不是交付证据。使用者必须验证算法指纹、输入面集合和配置匹配，并继续执行最终候选质量门。

### crop 与 baseline 目录

这些目录包含历史工作流产物，来源可能是原始扫描、STEP 采样、缓存裁剪或第三方工具。引用时必须说明来源，不能仅按扩展名推断。

## docs

~~~text
docs/
├─ module_design.md                    稳定架构与依赖约束
├─ implementation_status.md            当前能力、限制和验证基线
├─ TODO.md                             当前未完成任务
├─ completed_features.md               里程碑摘要
├─ run_gui.md                          GUI 使用与冒烟验证
├─ project_structure.md                当前目录职责
├─ surface_reduction_guide.md          Surface Reduction 产品合同
├─ geomagic_patch_workflow.md          Geomagic 显式后备合同
├─ native_surface_refitting_guide.md   冻结单曲面实验边界
└─ native_autosurface_guide.md         冻结 AutoSurface 结论
~~~

docs 不保存历史归档副本。删除或重写内容仍可由 Git 查询。

## experiments

experiments 只用于隔离尚未进入产品合同的尝试。实验成熟前：

- 不从 GUI 默认入口调用。
- 不改变当前产品状态声明。
- 不把一次性输入写入通用代码。
- 结果进入独立运行目录。

实验被冻结时，应在相应指南记录结论和重新启用条件，不保留日常流水账。

## 构建产物

windows-msvc-debug 的主要输出：

~~~text
build/windows-msvc-debug/Debug/
├─ step-patch-optimizer.exe
├─ spo_tests.exe
└─ Qt/Open CASCADE 运行依赖
~~~

build 可由配置和源代码重建，不是事实来源。

## 路径维护规则

- 产品代码放入与职责匹配的 src 子目录。
- 新探针放 tools，新可复用自动化放 scripts。
- 测试与受测合同同一提交维护。
- 精确运行事实放 data/runs。
- 当前状态、任务和架构分别只在对应文档维护。
- 不新增 docs/archive 来搬运已过期流水账。
