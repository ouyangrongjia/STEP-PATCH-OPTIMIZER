# STEP Patch Optimizer

STEP Patch Optimizer 是一个面向 STEP/B-rep 模型的桌面工具，用于检查曲面、构造受控候选、验证替换质量，并在证据充分时减少曲面数量。当前产品主线是原生 Surface Reduction；Geomagic AutoSurface 仅作为显式后备路线。

项目坚持三个边界：

- STEP 边界、拓扑与解析几何是硬事实。
- STL、点云和采样网格只用于拟合与检查，不能覆盖 STEP 真值。
- 候选只有通过拓扑、几何、往返和外部 CAD 证据后，才可视为可交付结果。

## 当前主线

分支状态及最近验证基线见 docs/implementation_status.md。Surface Reduction 的产品合同见 docs/surface_reduction_guide.md。

主线流程：

1. 载入 STEP，并建立面、边、壳体和支撑曲面信息。
2. 生成可以减少输出面数的候选分组。
3. 从原始 STEP 面重新拟合候选支撑曲面。
4. 以原始边界重建替换面，并执行修复。
5. 运行 StrictTopologyGate、几何质量门和 STEP 往返检查。
6. 仅归档通过全部适用门禁的候选与证据。

49→46 的 candidate-179 结果是已锁定回归基线，不代表全局最少面数证明。最少面模式按预算返回当前最好结果，并明确区分 BestFoundWithinBudget 与最优性证明。

## 快速开始

### 环境

推荐环境：

- Windows 10 或 Windows 11
- Visual Studio 2022，包含 C++ 桌面开发组件
- CMake 3.25 或更高版本
- vcpkg
- Qt 与 Open CASCADE，由 vcpkg 清单解析
- PowerShell 7 或 Windows PowerShell 5.1

默认预设为 windows-msvc-debug。构建脚本会优先使用 VCPKG_ROOT；未设置时使用用户目录下的 vcpkg。

### 配置与构建

~~~powershell
./scripts/build_debug.ps1
~~~

等价的底层命令：

~~~powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
~~~

应用程序位于：

~~~text
build/windows-msvc-debug/Debug/step-patch-optimizer.exe
~~~

### 运行测试

~~~powershell
./scripts/test.ps1
~~~

该脚本构建 spo_tests，运行 CTest，并把日志和 run.json 写入 data/runs 下的独立运行目录。

只执行 CTest 时可使用：

~~~powershell
ctest --preset windows-msvc-debug --output-on-failure
~~~

### 启动 GUI

~~~powershell
./build/windows-msvc-debug/Debug/step-patch-optimizer.exe
~~~

也可使用：

~~~powershell
./scripts/run_gui.ps1
~~~

完整操作、快捷键和冒烟步骤见 docs/run_gui.md。

## GUI 主流程

1. 打开 STEP 文件。
2. 在三维视图或面列表中检查目标区域。
3. 进入 Surface Reduction，生成并评估候选。
4. 检查候选的面数变化、偏差、边界和质量门状态。
5. 对通过门禁的候选执行 Apply。
6. 使用撤销、重做和导出验证事务行为。
7. 对交付候选执行 STEP 往返及外部 CAD 检查。

Apply 必须是事务性的：任何替换、修复或质量门失败都不得留下半成品状态。预览成功也不等于可应用或可交付。

## 常用命令

### 完整仓库验证

~~~powershell
./scripts/verify_spo.ps1
~~~

该脚本负责构建、测试和现有工程验证，不再机械要求同时修改多份文档。

### 运行 Surface Reduction 基线

具体参数、模式和产物合同见 docs/surface_reduction_guide.md。运行前应为本次实验创建独立 data/runs/<run-id> 目录，不覆盖既有证据。

### 运行 Geomagic 后备流程

Geomagic 路线必须由用户显式选择。命令、输入输出合同与质量门见 docs/geomagic_patch_workflow.md。

### 检查 GitNexus 索引

~~~powershell
node .gitnexus/run.cjs status
node .gitnexus/run.cjs analyze
~~~

只有索引过期时才需要重新分析。

## 事实来源

不同类型的事实只维护在一个位置：

| 内容 | 唯一事实来源 |
|------|--------------|
| 当前分支、能力、限制和最近基线 | docs/implementation_status.md |
| 当前未完成任务和验收条件 | docs/TODO.md |
| 稳定架构与依赖约束 | docs/module_design.md |
| 里程碑级完成事实 | docs/completed_features.md |
| 精确实验参数、指标和产物 | data/runs/<run-id>/run.json、RUN.md 及同目录产物 |
| 详细开发过程与技术日志 | Obsidian 项目库 |
| 历史版本 | Git |

仓库文档只摘要实验结论并链接证据，不复制完整指标表或长期流水账。

## 按任务查阅文档

无需预读固定文档集合。遇到具体任务时，按职责查阅：

| 文档 | 内容 |
|------|------|
| docs/module_design.md | 稳定架构、模块边界、依赖方向和事务不变量 |
| docs/implementation_status.md | 当前实现快照、限制和验证基线 |
| docs/TODO.md | 当前未完成工作及验收条件 |
| docs/completed_features.md | MVP、后备路线、冻结实验和 SR 里程碑 |
| docs/run_gui.md | GUI 启动、操作、快捷键、冒烟验证和故障排查 |
| docs/project_structure.md | 当前目录树和目录职责 |
| docs/surface_reduction_guide.md | Surface Reduction 产品合同 |
| docs/geomagic_patch_workflow.md | Geomagic 显式后备流程 |
| docs/native_surface_refitting_guide.md | 冻结的原生单曲面实验边界 |
| docs/native_autosurface_guide.md | 冻结的原生 AutoSurface 实验结论 |

## 核心工程约束

### 拓扑与修复

- StrictTopologyGate 是候选进入交付链的硬门禁。
- PatchReplacementRepair 负责替换后的受控修复，不能用宽松容差掩盖裂缝。
- 不允许仅凭 Open CASCADE 构造成功判定结果有效。
- 边界配对、方向、SameParameter、缝边和壳体闭合必须纳入验证。

### 拟合与缓存

- 放大后的候选分组必须从原始 STEP 面重新拟合，不能把父候选拟合面当作新真值。
- 可复用缓存以原始面集合和拟合配置做内容寻址。
- FitSet、Replay、Full 与 RefreshCache 的语义必须保持可区分。
- 缓存命中不能跳过最终候选质量门。

### 证据

- 每次正式运行使用独立 run-id。
- run.json 记录参数、环境、阶段结果和产物路径。
- RUN.md 提供供人检查的短摘要。
- 失败运行也应留下足以定位失败阶段的日志；不应把失败结果冒充基线。
- 商业 CAD 或 Creo 证据用于弥补单一内核可能漏掉的裂缝和参数化问题。

## 目录入口

~~~text
src/                 核心库、GUI 与缩减实现
include/             公共头文件
apps/                桌面应用入口
tests/               单元、合同与回归测试
tools/               探针和批处理工具
scripts/             构建、验证和实验脚本
docs/                当前有效的职责型文档
data/runs/           可复现运行证据
build/               本地构建产物
~~~

更完整的当前目录说明见 docs/project_structure.md。

## 常见问题

### CMake 找不到 vcpkg

确认 VCPKG_ROOT 指向有效安装目录，且目录中存在 scripts/buildsystems/vcpkg.cmake。

### GUI 无法启动

先确认 Debug 可执行文件已生成，再从仓库根目录启动。若缺少 Qt 或 Open CASCADE 运行库，重新运行构建脚本并检查 vcpkg 安装结果。

### 测试输出难以定位

scripts/test.ps1 会打印本次 data/runs 目录。优先检查其中 logs/build.log、logs/ctest.log 和 run.json。

### 候选预览正常但 Apply 失败

这是允许的结果。预览只证明候选可展示；Apply 仍需通过边界重建、修复、拓扑和几何门禁。不要绕过失败门禁。

### 结果在 Open CASCADE 中正常但外部 CAD 失败

按跨内核失败处理。检查边界距离、法向、缝边、SameParameter、支撑曲面包络和 STEP 往返证据，不得只保留内部内核结果。

## 网格重建实验

给定分区的 STL→B-Rep 研究入口、输入合同和复跑命令见 [tools/mesh_brep](tools/mesh_brep/README.md)。这是独立诊断后端；当前实测能力与限制由 [实现状态](docs/implementation_status.md) 维护。

## 许可证

许可证和第三方依赖声明以仓库实际文件及依赖清单为准。
