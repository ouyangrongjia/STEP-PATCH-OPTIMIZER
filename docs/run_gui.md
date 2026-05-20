# GUI 启动与常用命令

本文档说明如何在 VS Code 或命令行中启动 `step-patch-optimizer` GUI。

## 1. 前置环境

当前工程默认使用：

```powershell
CMake
Visual Studio 2022 Build Tools
vcpkg
Qt6
OpenCASCADE / OCCT
```

vcpkg 路径：

```powershell
C:\Users\27836\vcpkg
```

CMake preset 已写在项目根目录：

```text
CMakePresets.json
```

推荐直接使用 preset，不手写复杂 CMake 参数。

---

## 2. VS Code 启动方式

1. 用 VS Code 打开项目目录：

```powershell
code D:\pyProject\step-patch-optimizer
```

2. 安装推荐扩展：

```text
C/C++
CMake Tools
```

VS Code 会根据 `.vscode/extensions.json` 提示安装。

3. 在 VS Code 底部状态栏选择：

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
Ctrl + Shift + P
→ Debug: Select and Start Debugging
→ Launch step-patch-optimizer
```

也可以按 `F5`，使用 `.vscode/launch.json` 中的配置启动。

---

## 3. 命令行启动方式

在 PowerShell 中进入项目目录：

```powershell
cd D:\pyProject\step-patch-optimizer
```

配置工程：

```powershell
cmake --preset windows-msvc-debug
```

构建工程：

```powershell
cmake --build --preset windows-msvc-debug
```

启动 GUI：

```powershell
.\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

如果直接运行 exe 时出现下面错误：

```text
Could not find the Qt platform plugin "windows"
```

说明 Qt 的运行时插件还没有部署到 exe 同级目录。正常情况下，当前工程的 CMake 会在构建后自动执行 `windeployqt`。如果你是在旧 build 目录上运行，先重新构建：

```powershell
cmake --build --preset windows-msvc-debug
```

也可以手动部署一次：

```powershell
C:\Users\27836\vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.debug.bat --no-translations --no-system-d3d-compiler --no-compiler-runtime D:\pyProject\step-patch-optimizer\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

部署成功后，exe 同级目录下应出现：

```text
platforms\qwindowsd.dll
styles\
imageformats\
```

---

## 4. 一键配置、构建并启动

```powershell
cd D:\pyProject\step-patch-optimizer
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
.\build\windows-msvc-debug\Debug\step-patch-optimizer.exe
```

---

## 5. 运行测试

```powershell
cd D:\pyProject\step-patch-optimizer
ctest --preset windows-msvc-debug
```

如果刚清理过 `build/`，先执行：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

---

## 6. 当前 GUI 状态

当前窗口已经完成 GUI 模块骨架：

```text
菜单栏
工具栏
模型树
参数面板
Viewer 占位区
Log / Inspect / Validation / Report 底部面板
Face / Edge / Candidate 选择模式
鼠标中键旋转占位交互
滚轮缩放占位交互
右键菜单
```

注意：当前 `OccViewWidget` 还是 OCCT Viewer 的占位实现，尚未接入真实 `AIS_InteractiveContext` 和 `V3d_View`，因此可以启动和交互，但还不能真实显示 STEP 模型。

---

## 7. 常见问题

### 7.1 找不到 cmake

重新打开 PowerShell 或 VS Code，让系统 PATH 生效。

也可以临时执行：

```powershell
$env:Path='C:\Program Files\CMake\bin;C:\Users\27836\vcpkg;' + $env:Path
```

### 7.2 找不到 Qt6 或 OpenCASCADE

确认使用 preset 构建：

```powershell
cmake --preset windows-msvc-debug
```

不要直接运行裸命令：

```powershell
cmake -S . -B build
```

因为裸命令不会自动带上 vcpkg toolchain。

### 7.3 VS Code 没有识别 preset

确认安装了 CMake Tools 扩展，并重新打开项目目录：

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
```
