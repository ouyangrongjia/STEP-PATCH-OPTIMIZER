# STEP-PATCH-OPTIMIZER 项目结构文档

> 草案版本：v0.2-preview-then-apply  
> 当前主线：候选区域预览 → Geomagic 后端生成 STP/IGS patch → patch 叠加预览 → 用户点击 Apply → 真实贴回与边界缝合 → StrictTopologyGate。

---

## 1. 项目定位

```text
Feature-aware STEP Patch Optimizer
with STL-guided Geomagic AutoSurface Backend

基于特征边界与原始 STL 引导的 STEP 曲面片重拟合优化系统
```

本阶段项目目标不是“烂面修复”，而是：

```text
在已有 STEP/STP B-rep 模型上，识别被特征边界包围的候选区域；
利用原始 STL 提供局部几何采样；
调用 Geomagic Wrap AutoSurface 生成局部 STP/IGS patch；
先叠加预览 patch，再由用户点击 Apply；
最终使用原 STP boundary wire 约束真实贴回，并通过严格拓扑验证。
```

---

## 2. 当前推荐目录结构

```text
step-patch-optimizer/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── vcpkg.json
│
├── docs/
│   ├── module_design.md
│   ├── project_structure.md
│   ├── implementation_status.md
│   ├── run_gui.md
│   └── refactor_geomagic_autosurface/
│       ├── 00_PROJECT_STRUCTURE.md
│       ├── 01_MODULE_GUIDE.md
│       ├── 02_REFACTOR_PLAN.md
│       ├── 03_TODO.md
│       └── 04_PROJECT_WORKFLOW.md
│
├── scripts/
│   ├── bootstrap_windows.ps1
│   ├── setup_deps.ps1
│   ├── configure.ps1
│   ├── build_debug.ps1
│   ├── run_gui.ps1
│   ├── test.ps1
│   └── geomagic_wrap/
│       ├── autosurface_pipeline.py
│       ├── autosurface_config.example.json
│       └── README.md
│
├── src/
│   ├── app/
│   ├── brep/
│   ├── command/
│   ├── common/
│   ├── external/
│   │   └── geomagic/
│   ├── feature/
│   ├── gui/
│   ├── io/
│   ├── jobs/
│   ├── merge/
│   ├── patch/
│   ├── stl/
│   └── validate/
│
├── tests/
└── tools/
```

---

## 3. 新增目录说明

### 3.1 `scripts/geomagic_wrap/`

用于存放 Geomagic Wrap 内置脚本与配置样例。

```text
scripts/geomagic_wrap/
├── autosurface_pipeline.py
├── autosurface_config.example.json
└── README.md
```

职责：

```text
1. 接收 C++ 端生成的 config.json。
2. 通过 Geomagic ReadFile 读取 local STL。
3. 通过 AutoSurface 生成 local IGS。
4. 可选将 IGS 再读回为 cadModel 并 WriteFile 为 STEP214。
5. 输出 autosurface_result.json。
```

---

### 3.2 `src/external/geomagic/`

用于封装外部闭源后端。

```text
src/external/geomagic/
├── GeomagicAutoSurfaceConfig.h
├── GeomagicAutoSurfaceResult.h
├── GeomagicAutoSurfaceBackend.h
├── GeomagicAutoSurfaceBackend.cpp
├── GeomagicJobCache.h
└── GeomagicJobCache.cpp
```

职责：

```text
1. 通过 QProcess 调用 wrapCore.exe。
2. 传入 autosurface_pipeline.py 与 autosurface_config.json。
3. 捕获 stdout/stderr。
4. 读取 result.json。
5. 管理 timeout、失败原因和 mock 测试。
6. 不直接修改 ShapeDocument。
```

---

### 3.3 `src/stl/`

用于管理原始 STL 及局部裁剪。

```text
src/stl/
├── StlMesh.h
├── StlMesh.cpp
├── StlRegionExtractor.h
├── StlRegionExtractor.cpp
├── StlCropReport.h
└── StlCropReport.cpp
```

职责：

```text
1. 保存 STL triangle mesh。
2. 计算 STL bbox。
3. 根据 candidate bbox + margin 裁剪 local STL。
4. 输出 crop report。
5. STL 只提供拟合采样，不决定最终 CAD 边界。
```

---

### 3.4 `src/patch/`

用于导入、预览和贴回 Geomagic patch。

```text
src/patch/
├── ImportedPatchInfo.h
├── PatchImportService.h
├── PatchImportService.cpp
├── PatchPreviewReport.h
├── PatchPreviewReport.cpp
├── PatchPreviewModel.h
├── PatchPreviewModel.cpp
├── BoundaryConstrainedPatchBuilder.h
└── BoundaryConstrainedPatchBuilder.cpp
```

职责：

```text
1. 导入 local_output.step / local_output.igs。
2. 提取 patch shape、face count、edge count、bbox、BRepCheck 状态。
3. 生成 patch preview report。
4. 在真实 Apply 前，只作为 overlay 叠加预览。
5. Apply 后，通过 BoundaryConstrainedPatchBuilder 使用原 STP boundary wire 构造 replacement patch。
```

---

### 3.5 `src/jobs/`

用于管理候选区域 patch 生成和 Apply 状态。

```text
src/jobs/
├── RegionPatchJob.h
├── RegionPatchJob.cpp
├── RegionJobManager.h
└── RegionJobManager.cpp
```

第一版允许同步执行，但必须保留状态机：

```text
Pending
AnalyzingBoundary
CroppingStl
RunningGeomagic
ImportingPatch
PreviewReady
ApplyPending
Replacing
Validating
Applied
Rejected
Failed
Cancelled
```

---

## 4. 需要新增或强化的已有目录

### 4.1 `src/merge/`

新增：

```text
FeatureBoundedRegionBuilder.h
FeatureBoundedRegionBuilder.cpp
```

强化：

```text
MergeCandidate.h/.cpp
MergePlanner.h/.cpp
RegionBoundaryAnalyzer.h/.cpp
```

新增候选类型：

```cpp
MergeCandidateType::FeatureBoundedRefit
```

---

### 4.2 `src/brep/`

新增：

```text
BoundaryWireBuilder.h
BoundaryWireBuilder.cpp
```

职责：

```text
根据 RegionBoundaryAnalyzer 输出的 ordered_boundary_edges 构造 TopoDS_Wire。
不得直接用未排序 candidate.boundary_edges MakeWire。
```

---

### 4.3 `src/io/`

新增：

```text
StlReader.h/.cpp
StlWriter.h/.cpp
IgesReader.h/.cpp    // 若需要直接导入 IGS
```

说明：

```text
优先导入 Geomagic 生成的 STEP。
如果 STEP 不存在或导入失败，再尝试 IGS。
```

如需 IGS 导入，CMake 可能需要补充 OCCT `TKIGES`。

---

### 4.4 `src/command/`

新增：

```text
PatchReplacementCommand.h
PatchReplacementCommand.cpp
```

职责：

```text
1. 只在用户点击 Apply 后执行。
2. 保存 beforeDocument。
3. 使用 BoundaryConstrainedPatchBuilder 生成替换 patch。
4. 调用 sewing / ShapeFix / SameParameter。
5. 调用 StrictTopologyGate。
6. 成功提交 afterDocument。
7. 失败 rollback。
8. 支持 undo/redo。
9. redo 不重新运行 Geomagic。
```

---

### 4.5 `src/validate/`

新增：

```text
StrictTopologyGate.h
StrictTopologyGate.cpp
```

职责：

```text
作为真实贴回后的唯一提交门槛。
Gate 失败时不允许修改 CommandContext.document。
```

---

## 5. CMake 更新建议

新增 `.cpp` 时加入 `spo_core`：

```cmake
src/merge/FeatureBoundedRegionBuilder.cpp
src/brep/BoundaryWireBuilder.cpp
src/io/StlReader.cpp
src/io/StlWriter.cpp
src/stl/StlMesh.cpp
src/stl/StlRegionExtractor.cpp
src/stl/StlCropReport.cpp
src/external/geomagic/GeomagicAutoSurfaceBackend.cpp
src/external/geomagic/GeomagicJobCache.cpp
src/patch/PatchImportService.cpp
src/patch/PatchPreviewReport.cpp
src/patch/PatchPreviewModel.cpp
src/patch/BoundaryConstrainedPatchBuilder.cpp
src/jobs/RegionPatchJob.cpp
src/jobs/RegionJobManager.cpp
src/command/PatchReplacementCommand.cpp
src/validate/StrictTopologyGate.cpp
```

新增测试加入 `spo_tests`：

```cmake
tests/test_feature_bounded_region_builder.cpp
tests/test_boundary_wire_builder.cpp
tests/test_stl_io.cpp
tests/test_stl_region_extractor.cpp
tests/test_geomagic_backend_mock.cpp
tests/test_patch_import_service.cpp
tests/test_boundary_constrained_patch_builder.cpp
tests/test_patch_replacement_command.cpp
tests/test_strict_topology_gate.cpp
tests/test_region_job_manager.cpp
```

---

## 6. 临时文件与 `.gitignore`

不应提交：

```text
workspace/
geomagic_workspace/
*.igs
*.iges
*.wrp
*.log
*.tmp.stl
*.patch.step
*.patch.stp
local_input.stl
local_output.step
local_output.igs
autosurface_result.json
```

建议 `.gitignore` 增加：

```gitignore
workspace/
geomagic_workspace/
*.igs
*.iges
*.wrp
*.tmp.stl
*.patch.step
*.patch.stp
autosurface_stdout.log
autosurface_stderr.log
```

---

## 7. 当前阶段目录更新原则

```text
1. 不删除旧 Plane/Sphere/Cylinder 相关代码。
2. 不把 PlaneRegionMerge 继续作为主线增强。
3. 新路线所有新增模块都应独立落位，避免污染旧模块。
4. Patch preview 和 Patch apply 必须在目录职责上分开。
5. external/geomagic 只负责外部拟合，不负责修改主模型。
6. patch/ 负责导入、预览、替换辅助。
7. command/ 负责真实模型修改。
8. validate/StrictTopologyGate 负责最终提交裁决。
