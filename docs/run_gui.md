# GUI 启动、操作与手动验证

本文档说明如何在 VS Code 或命令行中启动 `step-patch-optimizer` GUI，并给出当前 GUI 的主要交互方式和手动验证流程。

> 当前状态：`OccViewWidget` 已接入真实 OCCT Viewer，支持 STEP/STP 模型显示、face/edge 命中选择、特征边显示、锁边高亮、same-domain 合并、undo/redo、合法性检查和 STEP 导出。

---

## 1. 前置环境

当前工程默认使用：

```powershell
CMake
Visual Studio 2022 Build Tools
vcpkg
Qt6
OpenCASCADE / OCCT
```

默认 vcpkg 路径：

```powershell
%USERPROFILE%\vcpkg
```

CMake preset 已写在项目根目录：

```text
CMakePresets.json
```

推荐直接使用 preset 或仓库中的脚本，不手写复杂 CMake 参数。

---

## 2. 推荐脚本启动方式

首次配置、依赖安装、构建和测试：

```powershell
cd D:\pyProject\step-patch-optimizer
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\bootstrap_windows.ps1
```

初始化完成后直接启动 GUI：

```powershell
.\scripts\bootstrap_windows.ps1 -RunGui
```

日常构建与启动：

```powershell
cd D:\pyProject\step-patch-optimizer
.\scripts\build_debug.ps1
.\scripts\run_gui.ps1
```

运行测试：

```powershell
.\scripts\test.ps1
```

---

## 3. VS Code 启动方式

1. 用 VS Code 打开项目根目录：

```powershell
code D:\pyProject\step-patch-optimizer
```

2. 推荐安装扩展：

```text
C/C++
CMake Tools
```

仓库已提供 `.vscode/extensions.json`，VS Code 会提示安装推荐扩展。

3. 在 VS Code 中选择 preset：

```text
Configure Preset: windows-msvc-debug
Build Preset: windows-msvc-debug
```

4. 构建：

```text
Ctrl + Shift + P
→ CMake: Build
```

5. 启动 GUI：

```text
F5
→ Launch step-patch-optimizer
```

也可以通过：

```text
Ctrl + Shift + P
→ Debug: Select and Start Debugging
→ Launch step-patch-optimizer
```

---

## 4. 命令行启动方式

在 PowerShell 中进入项目目录：

```powershell
cd D:\pyProject\step-patch-optimizer
```

配置工程：

```powershell
cmake --preset windows-msvc-debug
```

构建 GUI：

```powershell
cmake --build --preset windows-msvc-debug --target step-patch-optimizer
```

启动 GUI：

```powershell
.\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

构建并运行测试：

```powershell
cmake --build --preset windows-msvc-debug --target spo_tests
ctest --preset windows-msvc-debug --output-on-failure --timeout 30
```

---

## 5. 当前 GUI 主流程

当前 GUI 已支持以下主流程：

```text
打开 STEP/STP
→ 显示 B-rep 模型
→ 选择 face / edge
→ Shift 多选 face / edge
→ Ctrl 点击移除选择
→ 检测基础特征边
→ 用户手动锁边 / 解锁边
→ 执行 same-domain 曲面片合并
→ undo / redo
→ 检查合并后模型合法性
→ 导出新的 STEP/STP 文件
→ 导出后二次读取校验
```

### 5.1 打开模型

菜单或工具栏：

```text
文件 → 打开 STEP/STP
```

支持格式：

```text
.step
.stp
.STEP
.STP
```

成功打开后，左侧模型树会显示实体、壳、面、边等统计信息，中央 OCCT Viewer 会显示真实 B-rep 模型。

### 5.2 特征边检测

菜单或工具栏：

```text
检测 → 检测特征边
```

当前基础特征边包括：

```text
1. sharp edge：基于二面角阈值。
2. free edge：只有一个邻接面的边。
3. multiple edge：邻接面数量大于 2 的异常边。
```

检测结果会以高亮线条显示，并在报告面板输出特征边数量。

### 5.3 用户锁边 / 解锁边

操作流程：

```text
1. 切换到边选择模式。
2. 左键选择边。
3. Shift + 左键多选边。
4. Ctrl + 左键从选择集中移除边。
5. 右键打开上下文菜单。
6. 选择“锁定选中边”或“解锁选中边”。
```

锁定边会以高亮方式显示，并在执行合并时加入 protectedEdges，防止合并跨越用户认为重要的结构边。

### 5.4 执行合并

菜单或工具栏：

```text
合并 → 执行合并
```

当前执行的是基础版 same-domain 合并：

```text
自动特征边 + 用户锁定边 → protectedEdges
protectedEdges 传入 SameDomainUnifier
OCCT same-domain unify 尝试合并同一几何域上的相邻 faces
```

注意：当前合并算法偏保守，只会合并同一底层几何域上的碎片面。对于视觉上接近连续但底层不是同一 surface 的碎片面，需要后续 MergePlanner / MergeRegionGrower / SurfaceRefitter 等增强模块。

### 5.5 合法性检查

菜单或工具栏：

```text
验证 → 合法性检查
```

当前基础检查包括：

```text
1. 是否存在 shape。
2. OCCT BRepCheck 是否通过。
3. 实体 / 壳 / 面 / 边数量统计。
4. free edge 数量。
5. multiple edge 数量。
```

### 5.6 导出 STEP/STP

菜单或工具栏：

```text
文件 → 导出 STEP
```

导出后会自动执行二次读取校验。如果二次读取失败，报告面板会显示错误信息。

---

## 6. 鼠标与快捷键

| 操作 | 功能 |
|---|---|
| 鼠标中键拖拽 | 旋转三维视角 |
| 鼠标右键拖拽 | 平移视图 |
| Alt + 鼠标左键拖拽 | 平移视图 |
| 鼠标滚轮 | 放大 / 缩小 |
| 鼠标左键 | 选择当前模式下的 face 或 edge |
| Shift + 鼠标左键 | 多选 face 或 edge；已选中时再次点击可切换选择状态 |
| Ctrl + 鼠标左键 | 从当前选择集中移除 face 或 edge |
| 鼠标右键 | 打开上下文菜单 |
| F | 切换到面选择模式 |
| E | 切换到边选择模式 |
| H | 显示 / 隐藏特征线 |
| M | 预览合并候选入口 |
| R | 重置视角 |
| V | 合法性检查 |
| Ctrl + Z | 撤销最近一次可撤销编辑 |
| Ctrl + Y | 重做最近一次撤销编辑 |

---

## 7. 手动 GUI 验证流程

每次修改 GUI、Command、合并或验证相关代码后，建议执行以下手动验证：

```text
1. 启动 GUI。
2. 打开一个 STEP/STP 文件。
3. 确认模型在中央 Viewer 中正确显示。
4. 切换到面选择模式，点击若干 face，确认检查面板显示 Face ID 和邻接信息。
5. 使用 Shift + 左键多选 face，确认选择高亮正常。
6. 切换到边选择模式，点击若干 edge，确认检查面板显示 Edge ID 和邻接面数量。
7. 使用 Shift + 左键多选 edge。
8. 使用 Ctrl + 左键移除某条已选 edge。
9. 右键选择“锁定选中边”，确认锁边高亮显示。
10. Ctrl + Z，确认锁边撤销。
11. Ctrl + Y，确认锁边恢复。
12. 执行“检测特征边”，确认特征线显示，报告面板输出数量。
13. 执行“执行合并”，确认模型刷新，报告面板输出合并前后 face/edge 数。
14. Ctrl + Z，确认模型回退。
15. Ctrl + Y，确认模型恢复合并后状态。
16. 执行“合法性检查”，确认报告面板输出 BRepCheck、free edge、multiple edge 等信息。
17. 导出 STEP/STP。
18. 确认导出后二次读取校验通过。
19. 关闭并重新打开导出的 STEP/STP，确认可正常显示。
```

---

## 8. 常见问题

### 8.1 PowerShell 禁止运行 ps1

如果出现脚本执行策略错误，可以只对当前终端放开：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

然后重新执行脚本。

### 8.2 找不到 cmake

重新打开 PowerShell 或 VS Code，让系统 PATH 生效。

也可以临时执行：

```powershell
$env:Path='C:\Program Files\CMake\bin;%USERPROFILE%\vcpkg;' + $env:Path
```

### 8.3 找不到 Qt6 或 OpenCASCADE

先确认依赖已经安装：

```powershell
.\scripts\setup_deps.ps1
```

再重新配置和构建：

```powershell
.\scripts\configure.ps1
.\scripts\build_debug.ps1
```

不要直接运行裸命令：

```powershell
cmake -S . -B build
```

因为裸命令不会自动带上本项目 preset 里的 vcpkg toolchain，容易找不到 Qt6 或 OpenCASCADE。

### 8.4 Qt platform plugin "windows" 找不到

如果运行 exe 时出现：

```text
Could not find the Qt platform plugin "windows"
```

说明 Qt 的运行时插件还没有部署到 exe 同级目录。正常情况下，当前工程的 CMake 会在构建后自动执行 `windeployqt`。可以先重新构建：

```powershell
.\scripts\build_debug.ps1
```

如果还不行，可手动执行：

```powershell
%USERPROFILE%\vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.debug.bat --no-translations --no-system-d3d-compiler --no-compiler-runtime D:\pyProject\step-patch-optimizer\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

部署成功后，exe 同级目录下应出现：

```text
platforms\qwindowsd.dll
styles\
imageformats\
```

### 8.5 链接失败 LNK1168

如果构建时出现：

```text
LINK : fatal error LNK1168: 无法打开 ... step-patch-optimizer.exe 进行写入
```

说明 GUI 程序正在运行。关闭窗口后重新构建：

```powershell
.\scripts\build_debug.ps1
```

### 8.6 VS Code 没有识别 preset

确认安装了 CMake Tools 扩展，并重新打开项目根目录：

```powershell
code D:\pyProject\step-patch-optimizer
```

VS Code 打开的目录必须是项目根目录，里面应该能看到：

```text
CMakeLists.txt
CMakePresets.json
.vscode/
src/
docs/
scripts/
```
