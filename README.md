# step-patch-optimizer

Feature-aware STEP Patch Optimization.

## Build

Prerequisites:

- CMake
- Visual Studio 2022 Build Tools with MSVC
- vcpkg at `C:/Users/27836/vcpkg`
- vcpkg packages: `opencascade:x64-windows`, `qtbase:x64-windows`

Configure, build, and test:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```
