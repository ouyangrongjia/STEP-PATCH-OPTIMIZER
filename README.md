# STEP 曲面片优化器

Feature-aware STEP Patch Optimization，用于导入 STEP/STP 模型、显示 B-rep 拓扑、检测特征边、冻结边、执行 same-domain 合并、检查合法性并导出 STEP。

## 最简单用法

第一次配置一台 Windows 开发机时，优先使用脚本，不需要手动记一长串 CMake 和 vcpkg 命令。

```powershell
cd D:\pyProject\step-patch-optimizer

.\scripts\setup_deps.ps1
.\scripts\build_debug.ps1
.\scripts\run_gui.ps1
```

以后日常开发通常只需要：

```powershell
.\scripts\build_debug.ps1
.\scripts\run_gui.ps1
```

运行测试：

```powershell
.\scripts\test.ps1
```

## 这几个脚本做了什么

```text
scripts\setup_deps.ps1   使用 vcpkg.json 安装依赖
scripts\configure.ps1    执行 cmake --preset windows-msvc-debug
scripts\build_debug.ps1  配置并构建 GUI 和测试
scripts\run_gui.ps1      启动 GUI，如果 exe 不存在会先构建
scripts\test.ps1         构建测试并运行 ctest
```

默认路径：

```text
项目目录：D:\pyProject\step-patch-optimizer
vcpkg：C:\Users\27836\vcpkg
CMake preset：windows-msvc-debug
构建目录：build\windows-msvc-debug
GUI exe：build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

如果你的 vcpkg 不在默认位置，可以这样指定：

```powershell
.\scripts\setup_deps.ps1 -VcpkgRoot D:\dev\vcpkg
```

注意：`CMakePresets.json` 当前仍默认指向 `C:\Users\27836\vcpkg\scripts\buildsystems\vcpkg.cmake`。如果换了 vcpkg 位置，也要同步改 preset 里的 `CMAKE_TOOLCHAIN_FILE`。

## 必需工具

需要先安装：

- Visual Studio 2022 Build Tools，包含 `Desktop development with C++` / MSVC v143 / Windows SDK
- CMake 3.25 或更高版本
- Git
- vcpkg
- VS Code，推荐但不是必需

VS Code 只作为编辑器和启动器，不需要打开 Visual Studio IDE。

## vcpkg manifest

仓库已提供 `vcpkg.json`：

```json
{
  "dependencies": [
    "opencascade",
    "qtbase"
  ]
}
```

因此不需要再手动记：

```powershell
vcpkg install opencascade:x64-windows qtbase:x64-windows
```

直接执行：

```powershell
.\scripts\setup_deps.ps1
```

脚本会等价执行 manifest 安装：

```powershell
C:\Users\27836\vcpkg\vcpkg.exe install --triplet x64-windows --x-manifest-root=D:\pyProject\step-patch-optimizer
```

## 手动构建命令

如果不使用脚本，也可以直接执行底层命令：

```powershell
cd D:\pyProject\step-patch-optimizer
$env:Path='C:\Program Files\CMake\bin;C:\Users\27836\vcpkg;' + $env:Path

cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target step-patch-optimizer
cmake --build --preset windows-msvc-debug --target spo_tests
ctest --preset windows-msvc-debug --output-on-failure --timeout 30
```

正常情况下不要手写裸命令：

```powershell
cmake -S . -B build
```

因为裸命令不会自动带上本项目 preset 里的 vcpkg toolchain，容易找不到 Qt6 或 OpenCASCADE。

## 运行 GUI

脚本方式：

```powershell
.\scripts\run_gui.ps1
```

手动方式：

```powershell
.\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

也可以直接双击：

```text
D:\pyProject\step-patch-optimizer\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

构建时 CMake 会自动执行 `windeployqt`，把 Qt 运行时和插件部署到 exe 同级目录。

## VS Code 使用方式

```powershell
code D:\pyProject\step-patch-optimizer
```

推荐扩展：

- `ms-vscode.cpptools`
- `ms-vscode.cmake-tools`

仓库已提供：

```text
.vscode\extensions.json
.vscode\settings.json
.vscode\tasks.json
.vscode\launch.json
```

VS Code 中常用操作：

- `Ctrl+Shift+P` -> `CMake: Configure`
- `Ctrl+Shift+P` -> `CMake: Build`
- `Ctrl+Shift+P` -> `CMake: Run Tests`
- `F5` -> 启动 `Launch step-patch-optimizer`

## 常见问题

### PowerShell 禁止运行 ps1

如果出现脚本执行策略错误，可以只对当前终端放开：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

然后重新执行脚本。

### Qt platform plugin "windows" 找不到

如果运行 exe 时出现：

```text
qt.qpa.plugin: Could not find the Qt platform plugin "windows" in ""
This application failed to start because no Qt platform plugin could be initialized.
```

说明 Qt 插件没有部署到 exe 同级目录。先重新构建 GUI：

```powershell
.\scripts\build_debug.ps1
```

如果还不行，手动执行：

```powershell
C:\Users\27836\vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.debug.bat --no-translations --no-system-d3d-compiler --no-compiler-runtime D:\pyProject\step-patch-optimizer\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

成功后 exe 同级目录应包含：

```text
platforms\qwindowsd.dll
styles\
imageformats\
```

### 链接失败 LNK1168

如果构建时出现：

```text
LINK : fatal error LNK1168: 无法打开 ... step-patch-optimizer.exe 进行写入
```

说明 GUI 程序正在运行。关闭窗口后重新构建：

```powershell
.\scripts\build_debug.ps1
```

### MSBuild tlog 文件被占用

如果并行启动多个构建命令，可能出现 `.tlog` 文件被占用。不要同时运行两个 `cmake --build` 或两个构建脚本，单独重跑即可。

### 找不到 Qt6 或 OpenCASCADE

先确认依赖安装：

```powershell
.\scripts\setup_deps.ps1
```

再重新配置和构建：

```powershell
.\scripts\configure.ps1
.\scripts\build_debug.ps1
```

## 主要目录

```text
src\app       应用入口、MainWindow、AppController
src\command   命令系统、undo/redo、冻结边命令
src\brep      ShapeDocument、TopologyGraph、face/edge 索引
src\feature   特征边检测
src\merge     same-domain 合并
src\validate  合法性检查
src\gui       Qt/OCCT GUI 组件
src\io        STEP 读写
tests         单元测试
docs          模块设计、技术路线、运行说明
scripts       环境配置、构建、运行和测试脚本
```

## 当前核心能力

- 导入 STEP/STP
- OCCT 三维视图显示
- face / edge 命中与多选
- 用户冻结边与解冻
- 冻结边常驻高亮
- 特征边检测
- same-domain 合并
- 命令级 undo/redo
- 合法性检查
- 导出 STEP 并二次读取校验
