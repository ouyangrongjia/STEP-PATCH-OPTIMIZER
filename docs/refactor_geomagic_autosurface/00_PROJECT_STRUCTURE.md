# STEP-PATCH-OPTIMIZER 项目结构重构文档

> 草案版本：v0.1  
> 目标：把当前仓库从 OCCT 解析图元重建主线，重构为 **FeatureBoundedRegion + 原始 STL + Geomagic AutoSurface + OCCT 拓扑验证** 的曲面片重拟合优化系统。

## 1. 项目定位

当前项目仍然是：

```text
特征感知的 STEP 曲面片合并与边界优化系统
Feature-aware STEP Patch Optimization
```

重构后的推荐表述：

```text
基于特征边界与原始 STL 引导的 STEP 曲面片重拟合优化系统
Feature-aware STEP Patch Optimizer with STL-guided Geomagic AutoSurface backend
```

不是“烂面修复”。它的目标不是修补已有坏面，而是：

```text
在特征线围起来的候选区域内，使用更少、更连续的曲面 patch 替换原来的碎片面。
```

核心原则：

```text
STP 提供拓扑边界。
STL 提供原始几何采样。
Geomagic Wrap 提供曲面拟合。
OCCT 提供 STEP/B-rep 读写、边界接回、验证和导出。
```

---

## 2. 当前仓库结构概览

当前仓库已有较清晰的 C++20 + OCCT + Qt6 + CMake + vcpkg 结构：

```text
step-patch-optimizer/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── vcpkg.json
├── docs/
├── scripts/
├── src/
│   ├── app/
│   ├── brep/
│   ├── command/
│   ├── common/
│   ├── feature/
│   ├── gui/
│   ├── io/
│   ├── merge/
│   └── validate/
├── tests/
└── tools/
```

现有主干能力：

```text
STEP/STP 读取
B-rep 拓扑索引
OCCT Viewer 显示
face / edge / candidate 选择
特征边检测
用户锁边
MergeCandidate / MergePlanner / MergeRegionGrower
Candidate GUI Preview
PlaneRegionMerge / SphereRegionMerge 实验性真实合并
Command + undo/redo
ShapeValidator
STEP 导出和 roundtrip
```

---

## 3. 推荐新增目录

为了支持新流程，建议新增以下目录：

```text
src/stl/
    StlMesh.h/.cpp
    StlRegionExtractor.h/.cpp
    StlCropReport.h/.cpp

src/external/geomagic/
    GeomagicAutoSurfaceConfig.h
    GeomagicAutoSurfaceResult.h
    GeomagicAutoSurfaceBackend.h/.cpp
    GeomagicJobCache.h/.cpp

src/patch/
    ImportedPatchInfo.h
    PatchImportService.h/.cpp
    PatchPreviewModel.h/.cpp
    BoundaryConstrainedPatchBuilder.h/.cpp

src/jobs/
    RegionPatchJob.h/.cpp
    RegionJobManager.h/.cpp

scripts/geomagic_wrap/
    autosurface_pipeline.py
    autosurface_config.example.json
    README.md
```

如需导入 IGS，`src/io/` 中建议增加：

```text
IgesReader.h/.cpp
```

如只让 Geomagic 输出 STEP，则可以暂缓 IGS Reader。

---

## 4. 推荐重构后结构

```text
src/
├── app/                    # AppController，连接 GUI、Command、Job、Core
├── gui/                    # Qt/OCCT 交互、候选预览、patch overlay、日志
├── command/                # 所有真实修改模型的操作；PatchReplacementCommand 新增
├── io/                     # STEP/STP、STL、可选 IGS 读写
├── brep/                   # ShapeDocument、Face/Edge index、TopologyGraph、boundary wire 构造
├── feature/                # feature edges、user locked edges、protectedEdges
├── merge/                  # MergeCandidate、FeatureBoundedRegionBuilder、RegionBoundaryAnalyzer
├── stl/                    # 原始 STL 的局部裁剪
├── external/geomagic/       # wrapCore.exe 后端封装
├── patch/                  # patch 导入、叠加预览、边界约束替换辅助
├── jobs/                   # 候选区域处理任务队列、缓存和状态机
├── validate/               # ShapeValidator、StrictTopologyGate、ErrorMetric、ReportGenerator
└── common/                 # Result、Tolerance、Config、公共类型
```

---

## 5. 保留、降级与新增

### 保留

```text
STEP/STP 读写
OCCT Viewer
face/edge/candidate 选择
特征边检测
用户锁边
Candidate Preview
Command/undo/redo
ShapeValidator
STEP roundtrip
SameDomainUnifier baseline
```

### 降级为 experimental

```text
PlaneRegionMerge
SphereRegionMerge
Cylinder/Cone/Torus 手工重建路线
OCCT 自研自由曲面拟合路线
```

### 新增主线

```text
FeatureBoundedRegion candidate
STLRegionExtractor
GeomagicAutoSurfaceBackend
PatchImportService
BoundaryConstrainedPatchBuilder
PatchReplacementCommand
StrictTopologyGate
RegionJobManager
```

---

## 6. CMake 修改建议

新增源文件必须加入 `spo_core`。

建议逐步加入：

```cmake
src/merge/FeatureBoundedRegionBuilder.cpp
src/io/StlReader.cpp
src/io/StlWriter.cpp
src/stl/StlMesh.cpp
src/stl/StlRegionExtractor.cpp
src/stl/StlCropReport.cpp
src/external/geomagic/GeomagicAutoSurfaceBackend.cpp
src/external/geomagic/GeomagicJobCache.cpp
src/patch/PatchImportService.cpp
src/patch/PatchPreviewModel.cpp
src/patch/BoundaryConstrainedPatchBuilder.cpp
src/jobs/RegionPatchJob.cpp
src/jobs/RegionJobManager.cpp
src/validate/StrictTopologyGate.cpp
```

如果导入 IGS，可能需要增加 OCCT 链接库：

```cmake
TKIGES
```

---

## 7. 不应提交到仓库的文件

新增 `.gitignore` 建议：

```gitignore
workspace/
geomagic_workspace/
*.igs
*.iges
*.wrp
*.log
*.tmp.stl
*.patch.step
*.patch.stp
```

临时 STL、IGS、STEP patch、Geomagic 日志都应只存在于 workspace。
