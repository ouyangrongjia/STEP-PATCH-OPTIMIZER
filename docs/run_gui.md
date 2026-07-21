# GUI 运行与验证

本文只说明 GUI 的启动、当前操作流程、快捷键、手动冒烟验证和常见故障。产品状态见 implementation_status.md，Surface Reduction 合同见 surface_reduction_guide.md。

## 准备环境

需要：

- Visual Studio 2022 C++ 桌面工具链。
- CMake 3.25 或更高版本。
- vcpkg。
- 由 vcpkg 安装的 Qt 与 Open CASCADE。

推荐从仓库根目录运行所有命令。

若尚未安装依赖：

~~~powershell
./scripts/setup_deps.ps1
~~~

## 构建

推荐：

~~~powershell
./scripts/build_debug.ps1
~~~

脚本使用 windows-msvc-debug 预设，生成 GUI 和 spo_tests。

底层等价命令：

~~~powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
~~~

不要使用不带 toolchain 的裸 cmake -S . -B build 代替项目预设。

## 启动

直接启动：

~~~powershell
./build/windows-msvc-debug/Debug/step-patch-optimizer.exe
~~~

也可使用：

~~~powershell
./scripts/run_gui.ps1
~~~

VS Code 调试配置可通过 F5 启动。若刚完成构建，优先直接运行可执行文件以排除编辑器配置影响。

## 窗口布局

主窗口包括：

- 中央三维视图。
- 模型树。
- 检查面板。
- 参数面板。
- 日志和报告面板。
- Process Status。
- 主工具栏与高级菜单。

中央视图用于选择和预览；右侧或停靠面板显示当前实体、候选、参数和验证结果。后台任务的阶段、心跳和拟合进度显示在 Process Status 与报告中。

## 当前主流程

### 1. 导入 STEP

通过文件菜单打开 .step 或 .stp。

成功后应看到：

- 模型在中央视图显示。
- 模型树包含形状与面信息。
- 检查面板可响应选择。
- 日志报告读取结果。

读取失败不得替换当前已加载模型。

### 2. 检查面与边

使用 F 切换面选择，E 切换边选择。

面检查用于确认：

- Face ID。
- 支撑曲面类型。
- 邻接关系。
- 当前选择和候选归属。

边检查用于确认：

- Edge ID。
- 邻接面数量。
- 是否为特征边或锁定边。
- 是否可能成为候选硬边界。

Face ID 与 Edge ID 只对当前 ShapeDocument 版本有效。

### 3. 检测或锁定特征边

执行“检测特征边”，检查视图高亮和报告数量。

用户可以在边选择模式下锁定或解锁选中边。锁定边是候选生成的硬约束；Surface Reduction 不应跨越它。

锁边属于可撤销命令，可使用 Ctrl + Z 和 Ctrl + Y 检查事务。

### 4. 生成候选区域

执行“选择候选区域”。

应用进入候选选择模式后：

- 单击候选可切换当前候选。
- 视图高亮完整候选区域。
- 报告显示候选标识、输入面数和边界信息。
- 普通面选择不会替代候选自身保存的面集合。

若候选列表为空，检查特征边、锁定边、当前模型和筛选报告，不要手工写死候选编号。

### 5. 选择拟合输入

当前支持：

- StepBrepOnly：默认，仅以 STEP/B-rep 采样为拟合事实。
- STEP 加扫描 STL 软参考：显式选择后，把原始扫描作为拟合参考。

载入 STL 不会自动改变当前请求。界面和报告必须明确记录实际输入模式。

缓存裁剪 STL、STEP 采样网格和原始扫描 STL 是不同来源，不可混写。

### 6. 运行原生重拟合预览

点击“原生重拟合（预览）”。

后台执行期间应看到：

- 按钮状态阻止重复启动。
- 窗口仍可刷新和交互。
- Process Status 定期更新心跳。
- 当前阶段、已处理拟合数和活动分组持续更新。
- 报告保留搜索、拟合和终验拒绝原因。

预览结果应明确显示：

- 输入与输出面数。
- TargetRatio 或 MinimizeFaceCount 模式。
- TargetAchieved、OptimalProven、BestFoundWithinBudget、NoValidatedResultFound 或拒绝结果。
- 适用的距离、法向、接缝和拓扑门状态。
- 缓存与预算摘要。

预览可展示不代表可以 Apply。

### 7. Apply

只有以下条件同时满足时才可应用：

- 预览仍对应当前模型和当前候选。
- 参数和输入模式未变化。
- 输出面数满足当前产品模式的成功合同。
- 重裁剪和 PatchReplacementRepair 成功。
- StrictTopologyGate、BRepCheck 和 STEP 往返通过。
- 适用的商业 CAD 类门通过。

点击同一状态化按钮执行应用，或使用“一键预览并应用”。

一键操作只在预览全部通过后自动提交；失败、取消、候选变化或旧预览失效都不得改变当前模型。

### 8. 撤销与重做

Apply 成功后：

- Ctrl + Z 恢复应用前模型。
- Ctrl + Y 恢复应用结果。
- 视图、模型树和索引随文档版本刷新。

撤销/重做不应重新执行昂贵拟合，而应恢复已经提交的事务状态。

### 9. 验证与导出

使用“合法性检查”查看当前模型的 BRepCheck、自由边、多重边等信息。

导出 STEP 后，正式结果还应：

1. 重新读入导出的 STEP。
2. 再次检查拓扑和关键几何指标。
3. 在要求跨内核证据时进入 Creo 或其他商业 CAD 检查。

运行证据写入独立 data/runs/<run-id>，不要覆盖已有基线。

## 高级后备流程

高级菜单保留旧候选、patch preview、Apply 和 Geomagic AutoSurface。

这些入口不是当前默认流程。只有任务明确要求后备路线时使用，并遵循 geomagic_patch_workflow.md 的输入输出合同和质量门。

不要把原始 Geomagic patch STEP 当作最终合并模型；外部 CAD 对最终结果的检查必须接在 Apply 成功并导出的整件 STEP 之后。

## 鼠标与快捷键

| 操作 | 功能 |
|------|------|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标右键拖拽 | 平移视图 |
| Alt + 鼠标左键拖拽 | 平移视图 |
| 鼠标滚轮 | 放大或缩小 |
| 鼠标左键 | 选择当前模式下的面或边 |
| Shift + 鼠标左键 | 多选或切换选择 |
| Ctrl + 鼠标左键 | 从当前选择移除 |
| 鼠标右键 | 打开上下文菜单 |
| F | 面选择模式 |
| E | 边选择模式 |
| H | 显示或隐藏特征线 |
| M | 预览合并候选入口 |
| R | 重置视角 |
| V | 合法性检查 |
| Ctrl + Z | 撤销 |
| Ctrl + Y | 重做 |

## 手动冒烟验证

修改 GUI、命令、Surface Reduction 或验证逻辑后，至少执行：

1. 启动 GUI 并打开一个 STEP。
2. 确认模型、模型树和日志正常。
3. 用 F 选择面，检查 Face ID 和邻接信息。
4. 用 E 选择边，检查 Edge ID 和邻接面。
5. 用 Shift 多选，并用 Ctrl 从选择中移除。
6. 锁定一条边，确认高亮；撤销后消失，重做后恢复。
7. 检测特征边，确认报告和显示一致。
8. 生成候选并切换候选，确认整个区域高亮。
9. 运行原生重拟合预览，确认后台心跳、阶段和进度可见。
10. 若候选通过，执行 Apply，确认模型树和视图刷新。
11. 撤销和重做 Apply，确认模型状态一致。
12. 重新生成候选，检查旧预览不会误用于新候选。
13. 执行“一键预览并应用”，确认只有全部通过时提交。
14. 执行合法性检查。
15. 导出 STEP，并重新打开导出文件。

失败候选也应检查：

- 当前模型未改变。
- CommandHistory 未增加失败命令。
- 报告指出失败阶段。
- GUI 没有卡在永久忙碌状态。

## 常见问题

### 找不到 cmake

确认 CMake 已安装并位于 Path。可先检查：

~~~powershell
cmake --version
~~~

脚本默认也会把 C:\Program Files\CMake\bin 加入当前进程 Path。

### 找不到 vcpkg

设置 VCPKG_ROOT：

~~~powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
~~~

目录中应存在 scripts/buildsystems/vcpkg.cmake。

### 找不到 Qt6 或 OpenCASCADE

先运行：

~~~powershell
./scripts/setup_deps.ps1
./scripts/build_debug.ps1
~~~

确认使用 windows-msvc-debug 预设，而不是无 toolchain 的裸构建目录。

### Qt platform plugin windows 找不到

重新构建以触发 windeployqt。可执行文件同级目录应包含 platforms/qwindowsd.dll 及相关插件目录。

如果仍失败，检查构建日志中的 windeployqt 调用和实际 vcpkg triplet，不要复制其他构建配置的 Release 插件到 Debug 目录。

### LNK1168 无法写入可执行文件

GUI 仍在运行。关闭 step-patch-optimizer.exe 后重新构建。

### GUI 启动后没有模型

确认打开的是 STEP/STP，而不是把 STL 当作主模型。检查日志中的 StepReader 错误；读取失败时旧模型会保留。

### 预览长时间运行

检查 Process Status 是否仍有心跳、拟合计数或阶段变化。大型候选可能受真实拟合成本影响。若心跳停止且进程无活动，再保留日志并终止；不要把正常的长计算误判为简单导出卡死。

### Apply 按钮不可用

常见原因：

- 没有当前候选。
- 预览尚未完成。
- 预览未通过全部质量门。
- 当前模型、候选、输入模式或参数已变化。
- 结果只属于后备 patch 预览，尚未进入对应 Apply 合同。

### 外部 CAD 结果与 GUI 不一致

以跨内核失败处理。保留最终导出的整件 STEP、run.json 和外部工具报告，检查 SameParameter、支撑面包络、裁剪裂缝和短边，不要只复核屏幕截图。

## 验证记录

手动验证只记录结论和 run-id；截图、精确指标与外部工具产物放入对应运行目录。本文不追加每次 GUI 实验日志。
