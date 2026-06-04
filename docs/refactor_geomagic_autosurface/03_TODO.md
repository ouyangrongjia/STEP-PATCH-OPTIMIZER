# STEP-PATCH-OPTIMIZER 当前阶段 TODO

> 草案版本：v0.7-t6-multiface-replacement
> 当前主线：**候选区域预览 → STL 局部裁剪 → Geomagic AutoSurface 生成 IGS/STP patch → patch 叠加预览 → 用户点击 Apply → 真实贴回与边界缝合 → StrictTopologyGate 验证**。  
> 核心调整：Geomagic 后端采用 `wrapCore.exe --script` + `FIT_REGION_*` 环境变量传参；当前真实脚本只要求 input/output/log，`config.json` / `result.json` 只作为 C++ 后端兼容和 mock 测试结构，不作为真实 wrapCore 调用的必需输入输出。新增 `PatchArtifactLocator` 作为 T5 入口，生产逻辑必须根据 local STL / GeomagicAutoSurfaceResult / candidate artifact 动态定位 patch，禁止写死当前真实样例文件名。T5.4 已完成 Apply 占位状态机；T6 必须以 multi-face / complex patch replacement fragment 为主路径，不能假设 Geomagic 输出 1 个 B-rep face。

---

## 0. 当前路线定义

### 0.1 总体目标

```text
输入：当前 STP + 原始 STL

1. 在 STP 上生成 FeatureBoundedRegion candidates。
2. 用户在 GUI 中预览候选区域。
3. 用户接受 / 选择一个 candidate。
4. 对 candidate 提取并验证原 STP closed boundary wire。
5. 从原始 STL 中裁剪对应 local STL，允许 margin。
6. 将 local STL 写入 data/crop_stl。
7. 调用 wrapCore.exe + AutoSurface，生成同步目录下的 local IGS / local STP patch。
8. OCCT 导入 Geomagic 输出 patch。
9. 在 Viewer 中把 patch 与原 candidate 区域叠加预览。
10. 用户点击“应用 / Apply”。
11. 使用原 STP boundary wire 约束 Geomagic patch 替换，支持 multi-face patch fragment。
12. 尝试 sewing / ShapeFix / SameParameter。
13. StrictTopologyGate 验证。
14. 合法则提交 Command；不合法则 rollback。
```

### 0.2 核心原则

```text
STP 提供拓扑边界。
STL 提供几何采样。
Geomagic 提供曲面拟合。
OCCT 负责 patch 导入、叠加预览、真实替换、缝合与验证。
用户确认是 patch preview 和真实替换之间的硬分界线。
```

禁止：

```text
不要把 STL 裁剪边界作为最终 CAD 边界。
不要直接信任 Geomagic patch 的外边界。
不要在 patch 生成后自动贴回主模型。
不要绕过用户确认执行真实替换。
不要绕过 StrictTopologyGate 提交模型。
不要让 redo 重新运行 Geomagic。
不要在生产逻辑中写死 `local_candidate_0179_mechanical.stp`、`local_candidate_0179` 或任何固定 candidate 文件名。
当前真实样例只能作为 optional integration test / manual verification fixture。
```

### 0.3 crop 输出目录规范

T3/T4 后续统一使用以下目录约定：

```text
data/
  crop_stl/
    ... local region STL files
  crop_stp/
    ... Geomagic output STEP/STP files
  crop_igs/
    ... optional / compatibility IGES files
```

当前同步导出规则：

```text
输入 local STL:
data/crop_stl/<relative_dir>/<name>.stl

对应输出 STP:
data/crop_stp/<relative_dir>/<name>.stp

当前脚本保留的中间 IGS sidecar:
data/crop_stp/<relative_dir>/<name>_autosurface.igs

兼容路径推导仍保留:
data/crop_igs/<relative_dir>/<name>.igs
```

示例：

```text
data/crop_stl/03_配件_Clay/candidate_0007.stl
→ data/crop_stp/03_配件_Clay/candidate_0007.stp
→ data/crop_stp/03_配件_Clay/candidate_0007_autosurface.igs
```

要求：

```text
1. relative_dir 必须从 data/crop_stl 下的相对路径推导。
2. 输出目录不存在时自动创建。
3. 不允许把 Geomagic 输出文件写回 data/crop_stl。
4. 不允许覆盖原始 data/stl。
5. T4 后端应支持显式传入 outputStepPath / outputIgesPath；若未显式传入，则按 crop_stl → crop_stp / crop_igs 规则推导。
6. 当前真实脚本以 outputStepPath 为唯一必需产物；outputIgesPath 是兼容字段，脚本不再要求 FIT_REGION_OUTPUT_IGES。
```


### 0.4 Patch artifact 动态定位规则

当前真实样例：

```text
data/crop_stl/local_candidate_0179.stl
data/crop_stp/local_candidate_0179_mechanical.stp
data/crop_stp/local_candidate_0179_mechanical_autosurface.igs
data/crop_stp/local_candidate_0179_mechanical_fit_region.log
```

定位原则：

```text
这些文件只能作为 optional integration test / manual GUI verification 的样例。
生产流程不得写死 `local_candidate_0179`、`0179`、`mechanical` 或任何固定 patch 文件名。
正式流程必须根据 local STL / GeomagicAutoSurfaceResult / selected candidate 动态定位对应 patch。
```

建议引入 `PatchArtifactLocator`，负责把 T3/T4 产物转化为 T5 可导入的 patch artifact：

```text
selected candidate
→ localStlPath
→ PatchArtifactLocator
→ patchStepPath / patchIgesSidecarPath / fitRegionLogPath
→ PatchImportService
→ patch overlay
→ PatchPreviewReport
→ Apply
```

定位优先级：

```text
1. 如果存在 GeomagicAutoSurfaceResult：
   - 优先使用 result.outputStepPath。
   - result.outputIgesPath / result.preservedIgesPath 作为兼容 fallback。
   - result.inputStlPath 用于溯源。

2. 如果只有 local STL：
   - 根据 data/crop_stl/<relative>/<stem>.stl 定位：
     - data/crop_stp/<relative>/<stem>.stp
     - data/crop_stp/<relative>/<stem>_mechanical.stp
     - data/crop_stp/<relative>/<stem>_organic.stp
     - data/crop_stp/<relative>/<stem>_*.stp

3. 如果存在多个 patch：
   - 优先选择 *_mechanical.stp。
   - 然后选择最近修改的 .stp/.step。
   - 如果修改时间不可用，则使用固定字典序策略。
   - 不允许随机选择。

4. sidecar 文件：
   - 对于 patchStepPath = data/crop_stp/<relative>/<patch_stem>.stp
   - 优先寻找：
     - data/crop_stp/<relative>/<patch_stem>_autosurface.igs
     - data/crop_stp/<relative>/<patch_stem>_fit_region.log
   - data/crop_igs 只作为兼容 fallback，不作为唯一来源。
```

后续约束：

```text
T5.2/T5.3 不得直接从固定路径导入 patch。
T5.4/T6 Apply / replacement 不得直接从固定路径读取 patch。
Apply 所使用的 patch 必须来自当前 candidate 关联的 PatchArtifactPaths、PatchPreviewReport 或 GeomagicAutoSurfaceResult。
```


---

## 1. MVP 定义

### MVP-A：候选区域预览

```text
输入 STP
→ 检测 feature edges / user locked edges / model boundary
→ 生成 FeatureBoundedRegion candidates
→ GUI 高亮候选区域
→ 用户可接受 / 拒绝 / 隐藏 candidate
```

验收：

```text
1. FeatureBoundedRefit candidate 可显示。
2. 候选区域不跨越 protectedEdges。
3. candidate 状态变化不修改主模型。
4. GUI 能显示 candidate face count / boundary count / risk。
```

### MVP-B：Geomagic patch 生成 + 叠加预览

```text
用户选择一个 accepted candidate
→ RegionBoundaryAnalyzer 通过
→ 裁剪 local STL 到 data/crop_stl
→ wrapCore.exe + AutoSurface 生成 data/crop_stp，并可保留 IGS sidecar
→ PatchArtifactLocator 根据 local STL / result 动态定位 patch
→ PatchImportService 导入 patch
→ Viewer 叠加预览 patch
→ 输出完整 workspace 和日志
```

验收：

```text
1. local STL 成功生成。
2. Geomagic 输出对应的 crop_stp/*.stp。
3. fit_region log 可定位 mesh repair、AutoSurface 参数和最终退出状态。
4. PatchArtifactLocator 能根据 local STL / result 动态定位 patch，不写死样例文件。
5. patch 能导入 OCCT。
6. Viewer 能叠加显示 patch。
7. 清除 overlay 后主模型不变。
8. 此阶段不修改主 ShapeDocument。
```

### MVP-C：用户点击应用后的单候选真实贴回（multi-face patch 主路径）

```text
用户在 patch overlay 预览后点击 Apply
→ PatchReplacementCommand
→ MultiFacePatchAnalyzer
→ BoundaryConstrainedPatchBuilder 生成 multi-face replacement fragment
→ sewing / ShapeFix / SameParameter
→ StrictTopologyGate
→ 成功提交 Command
→ 失败 rollback
→ 支持 undo/redo
```

验收：

```text
1. 只处理单 closed outer boundary candidate。
2. 无 holes。
3. 无 non-manifold boundary。
4. Geomagic patch 可以是 multi-face / multi-shell，不允许因 patchFaceCount > 1 直接拒绝。
5. Gate 失败时主模型不变。
6. Gate 成功后可导出 STEP 并 roundtrip。
7. undo/redo 正常。
8. redo 不重新运行 Geomagic。
```

### MVP-D：批量与工程化增强

```text
多个 accepted candidates
→ 按 candidate 逐个生成 patch
→ 每个 patch 先 overlay preview
→ 用户逐个或批量 Apply
→ job queue / cache / report
```

MVP-D 在 MVP-C 单候选闭环稳定后再做。

---

# P0：候选区域主线重构

## T1.1 新增候选类型

文件：

```text
src/merge/MergeCandidate.h
src/merge/MergeCandidate.cpp
```

任务：

```text
新增 MergeCandidateType::FeatureBoundedRefit。
同步 toString、统计、GUI 显示。
```

验收：

```text
编译通过。
GUI/报告可显示 FeatureBoundedRefit。
现有 Plane/Sphere 测试不受影响。
```

## T1.2 新增 FeatureBoundedRegionBuilder

文件：

```text
src/merge/FeatureBoundedRegionBuilder.h
src/merge/FeatureBoundedRegionBuilder.cpp
tests/test_feature_bounded_region_builder.cpp
CMakeLists.txt
```

任务：

```text
输入 ShapeDocument + protectedEdges。
基于 face adjacency BFS/DFS。
不跨越 protectedEdges。
输出 FeatureBoundedRefit candidate。
```

验收：

```text
未保护边可跨越。
保护边不可跨越。
用户锁边不可跨越。
feature edge 不可跨越。
free edge / model boundary 成为区域边界。
```

## T1.3 MergePlanner 接入新开关

文件：

```text
src/merge/MergePlanner.h
src/merge/MergePlanner.cpp
```

新增：

```cpp
bool enable_feature_bounded_refit_candidates = true;
int min_feature_bounded_region_faces = 2;
```

验收：

```text
开关关闭时不生成 FeatureBoundedRefit。
开关开启时生成 FeatureBoundedRefit。
不破坏现有 PlaneLike / CylinderLike / SphereLike 入口。
```

## T1.4 GUI 候选区域预览入口

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/gui/MainWindow.cpp
src/gui/OccViewWidget.h
src/gui/OccViewWidget.cpp
src/gui/ModelTreePanel.cpp
src/gui/LogPanel.cpp
```

任务：

```text
1. GUI 新增 / 复用“预览 FeatureBoundedRegion 候选区域”入口。
2. 候选区域可高亮显示。
3. 用户可接受 / 拒绝 / 隐藏 candidate。
4. candidate 状态变化只影响候选管理，不修改 TopoDS_Shape。
```

验收：

```text
候选预览能跑通。
用户点击 accepted candidate 后能在报告面板看到 candidate id、face count、boundary edge count、risk。
```

---

# P0：Boundary 合法性门槛

## T2.1 强化 RegionBoundaryAnalyzer

文件：

```text
src/merge/RegionBoundaryAnalyzer.h
src/merge/RegionBoundaryAnalyzer.cpp
tests/test_region_boundary_analyzer.cpp
```

任务：

```text
分析 candidate faces 连通性。
提取并排序 outer boundary。
构造或输出 ordered boundary edges。
判断单闭环。
判断 inner holes。
判断 non-manifold / branch boundary。
输出 failure reason。
```

MVP-C 通过条件：

```text
connected_component_count == 1
outer_wire_count == 1
boundary_closed == true
inner_wire_count == 0
has_holes == false
has_non_manifold_edges == false
has_branching_boundary == false
```

验收：

```text
单闭环通过。
open boundary 拒绝。
multiple loop 拒绝。
holes 拒绝。
non-manifold boundary 拒绝。
branch boundary 拒绝。
失败原因可读。
```

## T2.2 BoundaryWireBuilder

文件：

```text
src/brep/BoundaryWireBuilder.h
src/brep/BoundaryWireBuilder.cpp
tests/test_boundary_wire_builder.cpp
CMakeLists.txt
```

任务：

```text
根据 RegionBoundaryAnalyzer 的 ordered_boundary_edges 构造 TopoDS_Wire。
不允许直接使用未排序 candidate.boundary_edges MakeWire。
```

验收：

```text
ordered closed edge loop 可构造 TopoDS_Wire。
open loop 构造失败。
乱序 edges 不能绕过 analyzer。
```

---

# P0：STL 输入与裁剪

## T3.1 新增 STL 数据结构

文件：

```text
src/stl/StlMesh.h
src/stl/StlMesh.cpp
```

任务：

```text
定义 StlTriangle、StlMesh、bbox 统计、triangle count 统计。
```

验收：

```text
StlMesh 可保存 triangle list。
可计算 bbox。
空 mesh 返回 invalid bbox。
```

## T3.2 新增 StlReader / StlWriter

文件：

```text
src/io/StlReader.h
src/io/StlReader.cpp
src/io/StlWriter.h
src/io/StlWriter.cpp
tests/test_stl_io.cpp
CMakeLists.txt
```

任务：

```text
读取原始 STL。
写出 local region STL。
第一版优先支持 binary STL；ASCII STL 可后置。
```

验收：

```text
读取测试 STL 成功。
写出后再次读取成功。
triangle count 保持。
不存在文件返回失败。
optional real STL test 文件不存在时跳过。
```

## T3.3 新增 StlRegionExtractor

文件：

```text
src/stl/StlRegionExtractor.h
src/stl/StlRegionExtractor.cpp
src/stl/StlCropReport.h
src/stl/StlCropReport.cpp
tests/test_stl_region_extractor.cpp
CMakeLists.txt
```

任务：

```text
candidate bbox
→ expand margin
→ 裁剪相交 triangles
→ 输出 local STL
→ 输出 crop report。
```

默认参数：

```cpp
double bboxMarginRatio = 0.01;
double minMargin = 0.1;
```

验收：

```text
local STL triangle count > 0。
margin 增大，triangle count 不减少。
空结果返回失败。
local STL bbox 与 candidate bbox / expanded bbox 有合理交集。
真实 STP + STL optional integration test 文件不存在时跳过。
```

注意：

```text
STL 裁剪只用于曲面拟合采样。
STL 裁剪边界不是最终 CAD 边界。
```

## T3.4 GUI 裁剪与 crop_stl 导出

> T3.4 是当前工程中已超出原始 T3.3 的 GUI 能力，保留为 TODO 文档中的正式子任务。

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/gui/MainWindow.h
src/gui/MainWindow.cpp
src/gui/OccViewWidget.h
src/gui/OccViewWidget.cpp
tests/test_commands.cpp
```

任务：

```text
1. GUI 支持打开原始 STL。
2. Viewer 可显示源 STL。
3. 用户选择 FeatureBoundedRefit candidate 后可裁剪当前候选 STL。
4. 裁剪在后台执行，不阻塞 GUI。
5. 裁剪结果可显示 cropped STL 和 expanded bbox。
6. 裁剪结果默认写入 data/crop_stl。
```

导出命名建议：

```text
data/crop_stl/<source_model_name>/candidate_<candidate_id>.stl
```

验收：

```text
打开 STP 后可打开对应原始 STL。
候选区域生成后可选择一个 FeatureBoundedRefit candidate。
点击裁剪后生成 local STL。
Viewer 可显示 local STL 和 crop bbox。
报告面板显示 source triangle count、output triangle count、candidate bbox、expanded bbox、output bbox。
此阶段不运行 Geomagic。
此阶段不修改主 ShapeDocument。
```

---

# P0：Geomagic AutoSurface 后端

> 本阶段目标是把 T3 裁剪得到的 local STL 交给 Geomagic Wrap 后台 AutoSurface，先做最小网格修复，再生成 local STEP patch，并输出单一 fit_region log。
> T4 只负责“生成 Geomagic patch 文件”，不导入 patch，不做 overlay，不做 Apply，不做 replacement face，不修改主 ShapeDocument。

## T4 总体定位

输入：

```text
FeatureBoundedRefit candidate
→ T2 boundary analysis valid
→ T3 local STL crop
→ data/crop_stl/<relative_dir>/<name>.stl
```

输出：

```text
data/crop_stp/<relative_dir>/<name>.stp
data/crop_stp/<relative_dir>/<name>_autosurface.igs   # keepTemp 时保留
autosurface_stdout.log
autosurface_stderr.log
fit_region.log
```

Geomagic 最小处理链：

```text
ReadFile(.stl)
→ RepairMesh / RemoveNonManifoldVertices / FillSmallHoles
→ optional Remesh / QuickSmooth / Relax
→ AutoSurface(.igs)
→ ReadFile(.igs).cadModel
→ WriteFile(.stp, filterId=5)
```

明确不做：

```text
Solidify
healCAD
BestFitFreeform
STEP 主模型替换
OCCT sewing
OCCT ShapeFix
patch overlay
Apply
StrictTopologyGate
```

重要原则：

```text
Geomagic 输出的 IGS/STP 是“拟合曲面来源”。
STL 裁剪边界不是最终 CAD 边界。
最终 trim / replacement 仍必须使用原 STP boundary wire。
`numPatches=1` 是 Geomagic 的近似 NURBS patch 目标，不保证最终 STEP 只有一个 B-rep face。
当前真实样例以 `geometry=Mechanical + autoMerge=true` 作为默认组合；Organic 在同一 crop STL 上会产生过多 STEP face。
```

真实 wrapCore 路径默认设为：

```text
E:\Geomagic Wrap\wrapCore.exe
```

注意：工具名是 `wrapCore.exe`，不是 `warpCore.exe`。

## T4.0 crop_stl → crop_stp / crop_igs 输出路径推导

文件：

```text
src/external/geomagic/GeomagicOutputPathResolver.h
src/external/geomagic/GeomagicOutputPathResolver.cpp
tests/test_geomagic_output_path_resolver.cpp
CMakeLists.txt
```

任务：

```text
根据 inputStlPath 推导 outputStepPath / outputIgesPath。
```

路径规则：

```text
输入：
data/crop_stl/<relative_dir>/<name>.stl

输出：
data/crop_stp/<relative_dir>/<name>.stp
data/crop_igs/<relative_dir>/<name>.igs
```

要求：

```text
1. 只对位于 data/crop_stl 下的 inputStlPath 自动推导。
2. 如果 inputStlPath 不在 data/crop_stl 下，返回失败或要求显式传入 outputStepPath / outputIgesPath。
3. 保留 relative_dir。
4. 输出目录不存在时自动创建。
5. 不写入 data/crop_stl。
6. 不覆盖原始 data/stl。
```

建议接口：

```cpp
struct GeomagicOutputPaths {
    bool success = false;
    std::filesystem::path outputStepPath;
    std::filesystem::path outputIgesPath;
    std::string message;
};

GeomagicOutputPaths resolveGeomagicOutputPathsFromCropStl(
    const std::filesystem::path& inputStlPath,
    const std::filesystem::path& cropStlRoot = std::filesystem::path("data/crop_stl"),
    const std::filesystem::path& cropStpRoot = std::filesystem::path("data/crop_stp"),
    const std::filesystem::path& cropIgsRoot = std::filesystem::path("data/crop_igs"));
```

验收：

```text
data/crop_stl/a/b/candidate_0001.stl 可推导到 data/crop_stp/a/b/candidate_0001.stp 和 data/crop_igs/a/b/candidate_0001.igs。
中文路径可处理。
非 crop_stl 输入返回明确失败。
输出目录自动创建。
```

## T4.1 Geomagic AutoSurface 配置和结果结构

文件：

```text
src/external/geomagic/GeomagicAutoSurfaceConfig.h
src/external/geomagic/GeomagicAutoSurfaceConfig.cpp
src/external/geomagic/GeomagicAutoSurfaceResult.h
src/external/geomagic/GeomagicAutoSurfaceResult.cpp
tests/test_geomagic_autosurface_config.cpp
CMakeLists.txt
```

任务：

```text
1. 定义 GeomagicAutoSurfaceConfig。
2. 定义 GeomagicAutoSurfaceResult。
3. 支持 config 写入 JSON。
4. 支持 result 写入 / 读取 JSON。
5. 支持基础 validation。
6. 不调用 QProcess。
7. 不调用真实 Geomagic。
```

建议结构：

```cpp
struct GeomagicAutoSurfaceConfig {
    std::filesystem::path wrapCorePath = std::filesystem::path("E:/Geomagic Wrap/wrapCore.exe");
    std::filesystem::path scriptPath;

    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path workDir;

    std::filesystem::path configJsonPath;
    std::filesystem::path resultJsonPath;
    std::filesystem::path stdoutLogPath;
    std::filesystem::path stderrLogPath;
    std::filesystem::path fitRegionLogPath;

    bool keepTemp = true;
    bool skipRemesh = true;
    bool quickSmooth = false;
    bool relax = false;
    int relaxIterations = 2;
    double relaxStrength = 0.25;

    bool adaptiveFit = false;
    bool autoMerge = true;
    bool strictPatchTarget = true;

    int numPatches = 1;
    std::vector<int> fallbackNumPatches = {2, 4, 8};

    double detail = 0.10;
    double tolerance = 0.03;
    std::string geometry = "Mechanical";

    bool convertIgesToStep = true;
    int timeoutSeconds = 1800;
};
```

```cpp
struct GeomagicAutoSurfaceResult {
    bool success = false;
    bool timedOut = false;

    int exitCode = -1;
    int bodies = 0;
    int openLoops = 0;

    std::string message;
    std::string errorMessage;
    std::string failedStage;

    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path preservedIgesPath;
    std::filesystem::path configJsonPath;
    std::filesystem::path resultJsonPath;
    std::filesystem::path stdoutLogPath;
    std::filesystem::path stderrLogPath;
    std::filesystem::path fitRegionLogPath;

    long long durationMs = 0;
};
```

JSON 字段使用 snake_case。

validation 要求：

```text
wrapCorePath 不为空。
scriptPath 不为空。
inputStlPath 不为空。
outputStepPath 不为空。
outputIgesPath 不为空。
workDir 不为空。
resultJsonPath 不为空。
numPatches > 0。
timeoutSeconds > 0。
tolerance > 0。
detail 在 [0, 1]。
geometry 只能是 Organic 或 Mechanical。
fallbackNumPatches 中每个 patch 数 > 0。
```

注意：

```text
T4.1 不检查 wrapCorePath 是否真实存在。
真实存在性检查放到 T4.2 runtime 或手动验证。
autoMerge=true 且 adaptiveFit=true 时，不建议在 validation 阶段直接失败；backend / Python 脚本应强制 adaptiveFit=false。
```

验收：

```text
config 可写入 JSON。
result 可写入 JSON。
result 可从 JSON 读取。
路径为空时返回配置错误。
非法数值参数返回配置错误。
默认 wrapCorePath 为 E:/Geomagic Wrap/wrapCore.exe。
默认 detail=0.10，tolerance=0.03。
默认 geometry=Mechanical，autoMerge=true，adaptiveFit=false。
不调用真实 Geomagic。
```

## T4.2 GeomagicAutoSurfaceBackend

文件：

```text
src/external/geomagic/GeomagicAutoSurfaceBackend.h
src/external/geomagic/GeomagicAutoSurfaceBackend.cpp
tests/test_geomagic_backend_mock.cpp
CMakeLists.txt
```

任务：

```text
1. 用 QProcess 调用 wrapCore.exe。
2. program = config.wrapCorePath。
3. arguments = ["--script", config.scriptPath]。
4. 通过 FIT_REGION_* 环境变量传参。
5. 捕获 stdout / stderr。
6. 写 stdout / stderr log。
7. 支持 timeout。
8. 若存在 result.json 则读取；当前真实脚本不输出 result.json 时，按 outputStepPath 是否存在兜底判断。
9. 支持 mock executable / mock cmd 测试。
10. 真实 Geomagic 不进入自动测试。
```

建议接口：

```cpp
class GeomagicAutoSurfaceBackend {
public:
    GeomagicAutoSurfaceResult run(const GeomagicAutoSurfaceConfig& config) const;
};
```

QProcess 调用要求：

```text
program: config.wrapCorePath
arguments: --script config.scriptPath
workingDirectory: config.workDir
environment: 系统原环境 + FIT_REGION_* 变量
```

必须设置的环境变量：

```text
FIT_REGION_INPUT = config.inputStlPath
FIT_REGION_OUTPUT = config.outputStepPath
FIT_REGION_WORK_DIR = config.workDir
FIT_REGION_LOG_FILE = config.fitRegionLogPath
FIT_REGION_KEEP_TEMP = 1/0
FIT_REGION_SKIP_REMESH = 1/0
FIT_REGION_QUICK_SMOOTH = 1/0
FIT_REGION_RELAX = 1/0
FIT_REGION_RELAX_ITERATION = config.relaxIterations
FIT_REGION_RELAX_STRENGTH = config.relaxStrength
FIT_REGION_AUTOSURFACE_TARGET = config.numPatches
FIT_REGION_AUTOSURFACE_TOLERANCE = config.tolerance
FIT_REGION_DETAIL_LEVEL = config.detail
FIT_REGION_GEOMETRY_MODE = config.geometry
FIT_REGION_AUTO_MERGE = 1/0
FIT_REGION_ADAPTIVE_FIT = 1/0
FIT_REGION_STRICT_PATCH_TARGET = 1/0
FIT_REGION_REPAIR_MESH = 1/0
FIT_REGION_FILL_HOLE_MAX_EDGES = 80
FIT_REGION_FILL_HOLE_LENGTH_RATIO = 1.0
```

兼容环境变量：

```text
FIT_REGION_OUTPUT_IGES = config.outputIgesPath      # 当前 Python 脚本不再要求
FIT_REGION_CONFIG_JSON = config.configJsonPath      # 当前 Python 脚本不再读取
FIT_REGION_RESULT_JSON = config.resultJsonPath      # 当前 Python 脚本不再写出
```

autoMerge / adaptiveFit 处理：

```text
如果 config.autoMerge == true 且 config.adaptiveFit == true：
  backend 传给进程的 FIT_REGION_ADAPTIVE_FIT 必须为 0。
  result.message 或 stdout log 中记录：autoMerge=True forces adaptiveFit=False。
```

runtime 检查：

```text
1. validateGeomagicAutoSurfaceConfig(config) 必须通过。
2. inputStlPath 必须存在。
3. scriptPath 必须存在。
4. workDir 不存在时尝试创建。
5. configJsonPath 若非空，写出 config JSON。
6. stdoutLogPath / stderrLogPath 父目录不存在时尝试创建。
7. resultJsonPath 父目录不存在时尝试创建。
8. outputStepPath / outputIgesPath 父目录不存在时尝试创建。
```

result.json 策略：

```text
1. 如果 resultJsonPath 存在：优先读取 result.json。
2. 如果 result.json 不存在：outputStepPath 存在时 success=true，即使 wrapCore.exe 进程 exitCode 非 0。
3. outputStepPath 不存在时必须 success=false。
4. outputIgesPath 不存在时标记 warning；若 outputStepPath 已成功生成，不一定失败。
```

mock 测试至少覆盖：

```text
mock success。
mock failure。
timeout。
missing input STL。
missing script file。
output file missing。
crop_stl → crop_stp / crop_igs 路径规则。
autoMerge + adaptiveFit 时 FIT_REGION_ADAPTIVE_FIT == 0。
```

验收：

```text
mock success 通过。
mock failure 通过。
timeout 通过。
输出文件不存在判失败。
crop_stl → crop_stp / crop_igs 路径规则通过。
真实 Geomagic 不参与自动测试。
```

## T4.3 Geomagic Python 脚本

文件：

```text
scripts/geomagic_wrap/autosurface_pipeline.py
scripts/geomagic_wrap/autosurface_config.example.json
scripts/geomagic_wrap/README.md
tests/test_geomagic_pipeline_script.cpp   # 可选，仅做静态检查
CMakeLists.txt                            # 如新增测试则修改
```

任务：

```text
1. 新增 Geomagic Wrap 内置 Python 环境可执行脚本。
2. 通过环境变量读取参数。
3. ReadFile 输入 local STL。
4. 默认执行 RepairMesh / RemoveNonManifoldVertices / FillSmallHoles。
5. optional Remesh / QuickSmooth / Relax。
6. AutoSurface 输出临时 IGS。
7. keepTemp 时保留 `<output>_autosurface.igs`。
8. ReadFile(IGS).cadModel。
9. WriteFile(STEP214) 输出 STP 到 FIT_REGION_OUTPUT。
10. 写单一诊断 log，不写 result.json。
```

脚本入口：

```text
wrapCore.exe --script scripts/geomagic_wrap/autosurface_pipeline.py
```

真实默认路径：

```text
E:\Geomagic Wrap\wrapCore.exe
```

必填环境变量：

```text
FIT_REGION_INPUT
FIT_REGION_OUTPUT
FIT_REGION_LOG_FILE
```

默认 AutoSurface 策略：

```text
numPatches = 1
autoMerge = True
adaptiveFit = False
detail = 0.10
geometry = Mechanical
tolerance = 0.03
repairMesh = True
fillHoleMaxEdges = 80
fillHoleLengthRatio = 1.0
```

fallback 策略：

```text
第一轮：requested autoMerge=<FIT_REGION_AUTO_MERGE>
然后尝试：
  one-patch autoMerge detail=0.0
  one-patch autoMerge Mechanical
  one-patch autoMerge larger tolerance=max(tolerance, 0.08)
  one-patch no autoMerge
如果 FIT_REGION_STRICT_PATCH_TARGET=0：
  fallback numPatches=2/4/8
  fallback automatic numPatches，如 Geomagic 支持 0 则尝试，否则跳过
```

IGS 临时路径策略：

```text
为避免中文路径导致 Geomagic 输出 IGS 不稳定，AutoSurface.fileName 优先写入 ASCII 临时路径：
scripts/geomagic_wrap/fit_region_temp/<safe_name>_<uuid>.igs
或：FIT_REGION_WORK_DIR/fit_region_temp/<safe_name>_<uuid>.igs
```

IGS 保存策略：

```text
1. AutoSurface 输出 temp IGS。
2. 如果 FIT_REGION_KEEP_TEMP=1：将 temp IGS copy 到 `<output_stp_stem>_autosurface.igs`。
3. 当前脚本不要求 FIT_REGION_OUTPUT_IGES。
```

IGS → STP：

```python
reader = ReadFile()
reader.filename = temp_igs_path
reader.run()
cad_model = reader.cadModel

writer = WriteFile()
writer.cadModel = cad_model
writer.filename = output_stp
writer.filterId = 5
writer.run()
```

日志至少包含：

```text
FIT_REGION_INPUT / FIT_REGION_OUTPUT / FIT_REGION_LOG_FILE
Before RepairMesh / After RepairMesh
FillSmallHoles numFilled
AutoSurface attempt label
geometry / tolerance / detail / adaptiveFit / autoMerge / numPatches
WriteFile STEP result
Final exit code
```

成功条件：

```text
1. input STL 存在。
2. ReadFile(STL) 成功并得到 mesh。
3. 修复后内部孔洞 / 非流形顶点数量下降，至少不恶化。
4. AutoSurface 生成 IGS。
5. ReadFile(IGS) 成功并得到 cadModel。
6. WriteFile(STEP214) 成功。
7. output STP 文件存在。
8. PatchImportService 能导入 output STP 并给出 face/edge/bbox/BRepCheck 统计。
```

README 要求：

```text
1. 说明脚本定位：local STL patch → IGS/STP，不做 final CAD replacement。
2. 说明 wrapCore 路径：E:\Geomagic Wrap\wrapCore.exe。
3. 给出默认运行命令。
4. 给出 crop 目录同步规则：data/crop_stl → data/crop_stp。
5. 给出放宽 patch 数命令：FIT_REGION_STRICT_PATCH_TARGET=0。
6. 给出实验性平滑命令。
7. 说明小 patch 默认不建议 remesh / relax / quick smooth。
8. 说明 STL 裁剪边界不是最终 CAD 边界。
9. 说明 STP 原始 boundary wire 才是后续 trim/replacement 依据。
10. 说明 AutoSurface 输出 STP/IGS 是拟合曲面来源，不是最终贴回结果。
11. 说明默认 Mechanical + autoMerge 的原因：真实 crop 样例中 Organic 会产生大量 STEP face。
```

手动验证命令示例：

```bat
set "FIT_REGION_INPUT=D:\pyProject\step-patch-optimizer\data\crop_stl\local_candidate_0179.stl" && set "FIT_REGION_OUTPUT=D:\pyProject\step-patch-optimizer\data\crop_stp\local_candidate_0179.stp" && set "FIT_REGION_LOG_FILE=D:\pyProject\step-patch-optimizer\data\crop_stp\local_candidate_0179_fit_region.log" && set "FIT_REGION_REPAIR_MESH=1" && set "FIT_REGION_AUTO_MERGE=1" && set "FIT_REGION_STRICT_PATCH_TARGET=0" && "E:\Geomagic Wrap\wrapCore.exe" --script "D:\pyProject\step-patch-optimizer\scripts\geomagic_wrap\autosurface_pipeline.py"
```

验收：

```text
真实 Geomagic 环境可手动验证。
fit_region log 可定位失败阶段。
成功时 output STP 存在。
成功时 PatchImportService 可导入 output STP。
自动测试不调用真实 Geomagic。
```

---

# P1：Geomagic patch 导入与叠加预览

> 本阶段是“应用前确认”阶段。  
> 用户必须先看到 Geomagic 得到的 STP/IGS patch 与原 candidate 的空间关系，再进入 Apply。  
> T5 已经完成 patch 导入、artifact 动态定位、overlay、preview report 和 Apply 占位状态机；T6 从 `ApplyPending` 入口开始做真实贴回。

## T5.1 PatchImportService

状态：

```text
已完成。
已拆分纯单元测试与真实文件/真实 Geomagic 接入测试。
真实 Geomagic 测试通过 SPO_ENABLE_REAL_GEOMAGIC_TESTS=1 显式启用，默认 ctest 不调用 wrapCore.exe。
```

文件：

```text
src/patch/ImportedPatchInfo.h
src/patch/PatchImportService.h
src/patch/PatchImportService.cpp
tests/test_patch_import_service.cpp
tests/test_patch_import_service_real.cpp
CMakeLists.txt
```

任务：

```text
导入 T4 生成的 Geomagic 输出 STEP 或 IGS。
优先使用 GeomagicAutoSurfaceResult。
返回 patch shape、face count、edge count、bbox、BRepCheck。
不修改主 ShapeDocument。
```

优先级：

```text
1. 优先导入 result.outputStepPath，即 data/crop_stp/<relative_dir>/<name>.stp。
2. 如果 STEP 不存在或导入失败，再尝试 result.outputIgesPath / result.preservedIgesPath，即 data/crop_igs/<relative_dir>/<name>.igs。
3. 导入失败不影响主模型。
```

当前真实样例验证：

```text
输入 STL：data/crop_stl/local_candidate_0179.stl
输出 STEP：data/crop_stp/local_candidate_0179_mechanical.stp
IGS sidecar：data/crop_stp/local_candidate_0179_mechanical_autosurface.igs
fit_region log：data/crop_stp/local_candidate_0179_mechanical_fit_region.log

RepairMesh / RemoveNonManifoldVertices / FillSmallHoles 后：
boundaryCycles: 11 -> 1
nonManifoldVertices: 2 -> 0
FillSmallHoles numFilled: 22

默认 AutoSurface：
geometry=Mechanical
autoMerge=True
adaptiveFit=False
numPatches=1

PatchImportService / step_stats：
faces=12
edges=50
shells=1
BRepCheck valid=true
```

验收：

```text
导入成功 valid=true。
导入失败不影响主模型。
bbox 和 face count 可显示。
patch bbox 与 candidate bbox 偏差过大时标记 HighRisk。
```

## T5.2.0 PatchArtifactLocator

状态：

```text
已完成。
已新增 PatchArtifactLocator，根据 local STL / GeomagicAutoSurfaceResult 动态定位 patch STEP/IGES sidecar/fit_region log。
生产逻辑不写死 local_candidate_0179 或任何固定 candidate 文件名；真实样例仅保留为 optional test fixture。
```

文件：

```text
src/patch/PatchArtifactLocator.h
src/patch/PatchArtifactLocator.cpp
tests/test_patch_artifact_locator.cpp
CMakeLists.txt
```

目标：

```text
根据 local STL / GeomagicAutoSurfaceResult / selected candidate artifact 动态定位 Geomagic patch。
生产逻辑不得写死当前真实样例 `local_candidate_0179_mechanical.stp` 或任何固定 candidate 文件名。
当前真实样例只能作为 optional integration test / manual verification fixture。
```

建议结构：

```cpp
struct PatchArtifactPaths {
    bool success = false;
    std::string message;

    std::filesystem::path localStlPath;
    std::filesystem::path patchStepPath;
    std::filesystem::path patchIgesSidecarPath;
    std::filesystem::path fitRegionLogPath;

    bool foundStep = false;
    bool foundIgesSidecar = false;
    bool foundFitLog = false;
};

class PatchArtifactLocator {
public:
    PatchArtifactPaths locateFromResult(const GeomagicAutoSurfaceResult& result) const;
    PatchArtifactPaths locateFromLocalStl(const std::filesystem::path& localStlPath) const;
};
```

定位规则：

```text
1. locateFromResult(result)：
   - 优先使用 result.outputStepPath。
   - 如果 result.outputStepPath 存在，则 patchStepPath = result.outputStepPath。
   - localStlPath = result.inputStlPath。
   - IGS sidecar 优先寻找 patchStepPath.parent_path() / (patchStepPath.stem() + "_autosurface.igs")。
   - fit log 优先寻找 patchStepPath.parent_path() / (patchStepPath.stem() + "_fit_region.log")。
   - result.outputIgesPath / result.preservedIgesPath 仅作为兼容 fallback。

2. locateFromLocalStl(localStlPath)：
   - localStlPath 必须位于 data/crop_stl。
   - stem = localStlPath.stem()。
   - relative_dir = data/crop_stl 下的相对目录。
   - 先找 data/crop_stp/<relative_dir>/<stem>.stp。
   - 再找 data/crop_stp/<relative_dir>/<stem>_mechanical.stp。
   - 再找 data/crop_stp/<relative_dir>/<stem>_organic.stp。
   - 再找 data/crop_stp/<relative_dir>/<stem>_*.stp。
   - 多个匹配时优先 mechanical，然后按最近修改时间选择最新。
   - 不允许随机选择。

3. sidecar：
   - data/crop_stp/<relative_dir>/<patch_step_stem>_autosurface.igs
   - data/crop_stp/<relative_dir>/<patch_step_stem>_fit_region.log
   - data/crop_igs 只作为兼容 fallback。
```

验收：

```text
不写死任何具体 candidate 文件名。
能从 localStlPath 找到对应 patch。
能从 GeomagicAutoSurfaceResult 找到对应 patch。
能识别 strategy suffix，例如 _mechanical / _organic。
能找到 IGS sidecar 和 fit log。
找不到 patch 时返回失败信息，不崩溃。
```

## T5.2 Patch overlay 叠加预览

状态：

```text
已完成。
GUI 已提供“导入当前候选 Patch（local STL）”、“从文件导入 Patch”和“清除 Patch Overlay”入口。
OccViewWidget 使用独立 AIS_Shape 显示 patch overlay，多次导入会先清理旧 overlay，清除 overlay 不修改主 ShapeDocument。
当前阶段只做 overlay 预览，不执行真实 replacement / sewing。
```

文件：

```text
src/gui/OccViewWidget.h
src/gui/OccViewWidget.cpp
src/app/AppController.h
src/app/AppController.cpp
src/app/MainWindow.h
src/app/MainWindow.cpp
```

任务：

```text
通过 PatchArtifactLocator 动态定位当前 candidate 对应的 patch。
调用 PatchImportService 导入 patch。
显示 imported patch overlay。
保持原 candidate 高亮。
支持显示 / 隐藏 / 清除 patch overlay。
支持重新生成 patch 后替换旧 overlay。
```

验收：

```text
Import Patch For Current Candidate 不写死样例文件。
patch overlay 能显示。
candidate highlight 与 patch overlay 可同时存在。
清除 overlay 后主模型不变。
多次导入不残留旧 AIS 对象。
```

## T5.3 Patch preview report

状态：

```text
已完成。
PatchPreviewReport 输出 candidate/source 统计、artifact 路径、patch 拓扑统计、candidate/patch bbox、bbox deviation、BRepCheck、warning 和 recommended action。
multi-face patch 只作为 warning；明显 bbox 偏离、BRepCheck 失败或 face count 过高标记 HighRisk。
GUI Inspect/Log 面板已显示报告。
```

文件：

```text
src/patch/PatchPreviewReport.h
src/patch/PatchPreviewReport.cpp
src/app/MainWindow.h
src/app/MainWindow.cpp
```

任务：

```text
输出 patch preview 报告：
- candidate id
- source face count
- source boundary edge count
- local STL path
- patch STEP path
- patch IGS sidecar path
- fit_region log path
- patch face count
- patch edge count
- patch bbox
- candidate bbox
- bbox deviation
- import BRepCheck result
- recommended action
```

验收：

```text
用户在 Apply 前能看到 patch 是否明显偏离原 candidate。
patch import 失败时报告明确。
patch bbox 明显异常时阻止 Apply 或标记 HighRisk。
multi-face patch 不直接判失败；当前真实样例 faces=12 应作为 warning，而不是 overlay 阻塞条件。
```

## T5.3.1 一键 Patch cutout overlay preview

状态：

```text
已完成。
GUI 新增“生成并预览当前 Patch”入口：自动裁剪当前 FeatureBoundedRefit candidate 的 local STL、调用 Geomagic 后端、动态导入 patch，并显示 visual-only cutout overlay。
Viewer cutout 只临时隐藏 candidate source faces 的显示，不修改 ShapeDocument，不执行 sewing，不导出最终 STEP。
默认输出路径为 data/crop_stl/<step文件stem>/<step文件stem>_candidate_%04d.stl 及对应 data/crop_stp / data/crop_igs 文件。
Geomagic 后端使用 workspace root 作为工作目录，并通过绝对路径传递输入 STL、输出 STEP 和脚本路径，避免 FileWrite 受 crop_stp 工作目录影响。
```

任务边界：

```text
不实现真实 Apply。
不实现 PatchReplacementCommand。
不从 B-rep 删除 source faces。
不让 redo 重新运行 Geomagic。
```

## T5.4 Apply 按钮和候选状态

状态：

```text
已完成。
本阶段只实现 Patch Apply 前状态机、AppController Apply 请求入口和 GUI 按钮门控。
点击“应用当前 Patch（T6 占位）”后只进入 ApplyPending，并返回“T6 replacement is not implemented”。
不修改 ShapeDocument，不创建 Command，不执行 replacement / sewing / ShapeFix / StrictTopologyGate，不导出最终 STEP。
```

文件：

```text
src/patch/PatchApplyState.h
src/patch/PatchApplyState.cpp
tests/test_patch_apply_state.cpp
src/app/MainWindow.h
src/app/MainWindow.cpp
src/app/AppController.h
src/app/AppController.cpp
CMakeLists.txt
tests/test_validation.cpp
```

新增状态：

```cpp
enum class RegionPatchStatus {
    NotGenerated,
    Generated,
    PreviewReady,
    PreviewHighRisk,
    ApplyBlocked,
    ApplyPending,
    Applied,
    ApplyFailed
};
```

任务：

```text
1. Patch preview ready 后，根据 PatchPreviewReport 计算 PatchApplyDecision。
2. 未生成 preview、preview report 失败、HighRisk、bbox 无效、BRepCheck 失败、face count <= 0 时禁用 Apply。
3. multi-face patch 不阻止 T5.4 Apply request，只保留 PatchPreviewReport warning。
4. AppController::requestApplyCurrentPatchPreview() 只更新状态到 ApplyPending，并返回 T6 未实现错误。
5. GUI Apply 入口是 Geomagic patch apply 占位，不复用或修改旧 applyMergeAction_。
6. Report 中显示 patch status、canRequestApply 和 decision reason/message。
```

验收：

```text
未生成 patch 时 Apply 禁用。
patch import 失败时 Apply 禁用。
PreviewReady 后 Apply 可点击。
PreviewHighRisk / ApplyBlocked 时 Apply 禁用并显示原因。
点击 T6 占位 Apply 后状态为 ApplyPending。
Apply 请求不会修改主模型，不会导出最终 STEP，不会调用 Geomagic。
```

## T5.4.1 stale patch state 安全清理

状态：

```text
待实现。
进入 T6 前必须补齐，防止旧 patch preview state 跨 STEP 文件、跨 undo/redo 或跨模型刷新残留。
```

文件：

```text
src/app/MainWindow.cpp
src/app/AppController.h
src/app/AppController.cpp
tests/test_patch_apply_state.cpp 或 tests/test_commands.cpp
```

任务：

```text
1. 打开新 STEP 成功后，必须清除 viewer patch overlay 和 AppController patch state。
2. refreshDocumentViews() 刷新主 ShapeDocument 后，必须清除 patch overlay 和 patch state。
3. undo / redo 成功后，必须清除 patch overlay 和 patch state。
4. 任意旧合并命令成功修改主 ShapeDocument 后，必须清除 patch overlay 和 patch state。
5. 清除后 refreshPatchApplyAction()，Apply Patch 按钮必须禁用。
```

建议新增 helper：

```cpp
void MainWindow::clearPatchPreviewStateOnly();
```

语义：

```text
viewer_->clearPatchOverlay();
controller_.clearCurrentPatchOverlay();
refreshPatchApplyAction();
```

验收：

```text
打开新 STEP 后 patchPreviewReady=false，currentPatchStatus=NotGenerated。
undo / redo 后 patchPreviewReady=false，Apply Patch 按钮禁用。
旧 patch 不可能被用于新 ShapeDocument 的 T6 replacement。
清理逻辑不删除 data/crop_stp / data/crop_stl 文件，只清理当前 UI/controller 状态。
```

---

# P1：用户点击 Apply 后的真实贴回

> T6 是第一个真正修改 `ShapeDocument` 的阶段。  
> 与早期设想不同，T6 **不能假设 Geomagic 只输出 1 个 face**。当前真实 Mechanical + AutoMerge 样例约为 12 faces，后续任何真实流程都不能因为 `patchFaceCount > 1` 直接拒绝。  
> T6 的基础目标是：支持 Geomagic 输出的 multi-face / multi-shell patch，以“多面片 replacement fragment”为主路径；one-face patch 只是特例。

## T6 总体原则：复杂 patch / multi-face patch 必须是主路径

必须支持：

```text
1. imported patch shape 是 TopoDS_Compound / TopoDS_Shell / TopoDS_Solid / TopoDS_Face 任一形式。
2. patchFaceCount 可以大于 1；不能以 patchFaceCount > 1 作为 unsupported。
3. Geomagic patch 内部 seam / internal edges 允许保留，作为 replacement fragment 的内部拓扑。
4. T6 不追求强制合成 1 张 B-spline face。
5. 真实贴回后允许 candidate 区域被一个 multi-face patch fragment 替换。
6. 是否接受最终结果由 StrictTopologyGate 判断，而不是由 patch face 数直接判断。
```

仍然禁止：

```text
1. 不直接信任 Geomagic patch 外边界作为最终 CAD 边界。
2. 不使用 STL 裁剪边界作为最终 CAD 边界。
3. 不绕过原 STP boundary wire。
4. 不绕过 StrictTopologyGate 提交结果。
5. 不让 redo 重新运行 Geomagic。
6. 不从固定路径读取 patch。
```

T6 推进顺序：

```text
T5.4.1 stale patch state cleanup
→ T6.0 PatchReplacement 输入/报告结构 + MultiFacePatchAnalyzer
→ T6.1 BoundaryConstrainedPatchBuilder：multi-face replacement fragment
→ T6.2 StrictTopologyGate 最小可用版
→ T6.3 PatchReplacementCommand
→ T6.4 Sewing / ShapeFix / SameParameter 集成
→ T6.5 AppController / GUI 接入真实 Apply
```

## T6.0 PatchReplacement 输入结构与 MultiFacePatchAnalyzer

文件：

```text
src/patch/PatchReplacementInput.h
src/patch/PatchReplacementReport.h
src/patch/MultiFacePatchAnalyzer.h
src/patch/MultiFacePatchAnalyzer.cpp
tests/test_multiface_patch_analyzer.cpp
CMakeLists.txt
```

目标：

```text
在真正构造 replacement 之前，先把当前 candidate、boundary、imported patch、preview report、artifact paths 统一成一个不可歧义的输入结构，并分析 imported patch 的多面片拓扑。
```

建议结构：

```cpp
struct PatchReplacementInput {
    const ShapeDocument* document = nullptr;
    const MergeCandidate* candidate = nullptr;
    const RegionBoundaryAnalysis* boundary = nullptr;
    const ImportedPatchInfo* importedPatch = nullptr;
    const PatchArtifactPaths* artifactPaths = nullptr;
    const PatchPreviewReport* previewReport = nullptr;
};

struct MultiFacePatchAnalysis {
    bool success = false;
    std::string message;

    int faceCount = 0;
    int edgeCount = 0;
    int shellCount = 0;
    int solidCount = 0;

    std::vector<TopoDS_Face> faces;
    std::vector<TopoDS_Edge> outerEdges;
    std::vector<TopoDS_Edge> internalEdges;

    bool bboxValid = false;
    bool brepCheckValid = false;
    bool hasAtLeastOneFace = false;
    bool isSingleFace = false;
    bool isMultiFace = false;
};
```

任务：

```text
1. validatePatchReplacementInput：检查 document/candidate/boundary/importedPatch/previewReport 是否齐全。
2. 检查 candidate boundary 必须是单 closed outer wire，无 holes，无 non-manifold / branch boundary。
3. 检查 previewReport.success=true 且 highRisk=false。
4. 检查 importedPatch.success=true，shape 非空，bbox valid，BRepCheck valid。
5. MultiFacePatchAnalyzer 从 importedPatch.shape 提取所有 face/edge/shell/solid。
6. patchFaceCount > 1 必须 success=true，只设置 isMultiFace=true。
7. 输出 MultiFacePatchAnalysis，供 T6.1 构造 replacement fragment。
```

验收：

```text
one-face patch 可分析成功。
multi-face box / shell / Geomagic patch 可分析成功。
empty patch 失败，reason 清楚。
BRepCheck 失败时失败，reason 清楚。
patchFaceCount > 1 不失败。
输入缺失不崩溃。
```

## T6.1 BoundaryConstrainedPatchBuilder：multi-face replacement fragment

文件：

```text
src/patch/BoundaryConstrainedPatchBuilder.h
src/patch/BoundaryConstrainedPatchBuilder.cpp
tests/test_boundary_constrained_patch_builder.cpp
CMakeLists.txt
```

输入：

```text
- PatchReplacementInput
- MultiFacePatchAnalysis
- candidate source faces
- original STP outer boundary wire
- imported Geomagic patch shape / patch faces
```

输出：

```text
- replacementShape：TopoDS_Shape，可为 Face / Shell / Compound
- replacementFaces：multi-face replacement fragment 的 faces
- sourceFaceIds：被替换的原 candidate faces
- internalPatchEdges：保留的 patch 内部 seam / internal edges
- report：构造过程、失败原因、是否使用 multi-face path
```

核心策略：

```text
1. one-face patch 走简单 face surface replacement path。
2. multi-face patch 走主路径：保留 Geomagic patch 的内部 face network，构造 replacement fragment。
3. replacement fragment 的外部边界必须最终与原 STP candidate outer boundary 对齐或可 sewing。
4. Geomagic patch 的外边界只作为辅助几何，不作为最终合法性依据。
5. 不强制把 multi-face patch 合并为 1 张面。
6. 不因为 patchFaceCount > 1 返回 unsupported。
7. 如果无法在原 boundary 内构造可 sewing fragment，应返回明确失败原因，而不是 silent fallback。
```

第一版允许的工程化策略：

```text
A. 直接 multi-face fragment strategy：
   - 提取 imported patch 的所有有效 faces。
   - 根据 candidate bbox / patch bbox 过滤明显离群 faces。
   - 构造 TopoDS_Compound 或 TopoDS_Shell 作为 replacement fragment。
   - 保留 patch 内部边。
   - 后续由 T6.4 sewing + T6.2 gate 判断是否可提交。

B. Boundary bridge strategy：
   - 原 STP outer boundary wire 必须作为最终边界参考。
   - 如果 patch outer boundary 与 original outer wire 不一致，记录 boundary mismatch。
   - 可以生成需要 sewing 的 replacement fragment，但不能直接提交；必须经过 StrictTopologyGate。

C. one-face special strategy：
   - 如果 imported patch 只有一个 face，可使用该 face surface 与原 boundary wire 构造 replacement face。
   - 这是特例，不是 T6 主假设。
```

明确不做：

```text
不实现全自动自由曲面重拟合。
不要求 multi-face patch 被合并成单面。
不绕过 StrictTopologyGate。
不直接把 Geomagic patch outer wire 当作最终 boundary。
不使用 STL 裁剪边界作为最终 boundary。
```

验收：

```text
one-face patch 可生成 replacement face 或 fragment。
multi-face patch 可生成 replacement fragment，至少不因 faceCount>1 被拒绝。
replacement fragment 保留内部 patch seam 信息。
boundary invalid 时失败。
imported patch 无 face 时失败。
失败 reason 可读。
不修改主 ShapeDocument。
```

## T6.2 StrictTopologyGate 最小可用版

> StrictTopologyGate 应提前于 PatchReplacementCommand 落地。只要 T6 开始修改模型，就必须先有 gate，否则真实替换失败很难回滚和定位。

文件：

```text
src/validate/StrictTopologyGate.h
src/validate/StrictTopologyGate.cpp
tests/test_strict_topology_gate.cpp
CMakeLists.txt
```

检查：

```text
BRepCheck。
free edge 不增加。
multiple edge 不增加。
solid count 不变或符合 explicit fragment replacement 规则。
shell closure。
bbox 异常。
STEP export。
STEP roundtrip。
source face count 与 replacement face count 记录，但 replacement face count > 1 不是失败条件。
```

multi-face 规则：

```text
1. replacementFaceCount 可以大于 1。
2. Gate 不以 replacementFaceCount > 1 作为失败。
3. Gate 重点检查拓扑合法性、free edges、multiple edges、shell/solid 一致性和 STEP roundtrip。
4. 如果 face count 没有下降，也不一定失败；先记录 warning，由后续优化策略决定。
```

验收：

```text
gate 失败返回清晰原因。
report 可写入 JSON。
gate 失败不允许 Command 提交。
STEP roundtrip 失败时拒绝。
multi-face replacement fragment 可进入 gate。
```

## T6.3 PatchReplacementCommand

文件：

```text
src/command/PatchReplacementCommand.h
src/command/PatchReplacementCommand.cpp
tests/test_patch_replacement_command.cpp
CMakeLists.txt
```

任务：

```text
1. 输入 PatchReplacementInput，imported patch 必须来自当前 candidate 关联的 PatchArtifactPaths / PatchPreviewReport / GeomagicAutoSurfaceResult。
2. 不允许写死 data/crop_stp/local_candidate_0179_mechanical.stp 或任何固定 patch 路径。
3. 保存 beforeDocument。
4. 删除 / 替换 candidate source faces。
5. 接入 BoundaryConstrainedPatchBuilder。
6. 支持 multi-face replacement fragment。
7. 尝试 sewing / ShapeFix / SameParameter。
8. 调用 StrictTopologyGate。
9. Gate 成功才提交 afterDocument。
10. Gate 失败 rollback。
11. 支持 undo/redo。
12. redo 不重新运行 Geomagic，只复用已生成的 T4 result 和 patch 文件。
```

限制：

```text
第一版只处理单 closed outer wire candidate。
第一版不处理 holes。
第一版不处理 boundary 自交。
第一版不处理跨 shell candidate。
但第一版必须处理 multi-face imported patch，不能把 multi-face 归类为 unsupported。
```

验收：

```text
成功替换可 undo/redo。
gate 失败不改变 document。
redo 不重新运行 Geomagic。
失败报告包含 rollback_applied=true。
multi-face patch replacement path 至少有 synthetic test 覆盖。
```

## T6.4 Sewing / ShapeFix 集成

文件：

```text
src/patch/BoundaryConstrainedPatchBuilder.cpp
src/command/PatchReplacementCommand.cpp
src/validate/StrictTopologyGate.cpp
```

任务：

```text
对替换后的临时 shape 执行必要修复：
- BRepLib::SameParameter
- ShapeFix_Face
- ShapeFix_Wire
- 可选 BRepBuilderAPI_Sewing
```

multi-face 要求：

```text
1. sewing 必须允许 replacement fragment 内部存在多张 patch faces。
2. 内部 seam 不应被当作 free edge 直接判错。
3. 只有 replacement fragment 与周围原模型连接处产生新的 free edge / multiple edge 时，才应由 Gate 拒绝。
4. sewing 前后必须记录 face/edge/shell/solid 统计。
```

验收：

```text
替换后如果 free edge 增加，Gate 拒绝。
sewing 成功但 BRepCheck 失败，Gate 拒绝。
sewing 后 solid count 非预期改变，Gate 拒绝。
multi-face patch 内部 seam 可保留。
```

## T6.5 AppController / GUI 接入 Apply 流程

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/gui/MainWindow.cpp
src/gui/ModelTreePanel.cpp
src/gui/LogPanel.cpp
```

新增操作：

```text
Apply Current Patch To Candidate
```

内部流程：

```text
selected candidate
→ check PreviewReady
→ check boundary analysis
→ check imported patch
→ build PatchReplacementInput
→ MultiFacePatchAnalyzer
→ BoundaryConstrainedPatchBuilder
→ PatchReplacementCommand
→ StrictTopologyGate
→ report
```

验收：

```text
用户必须先看到 overlay preview，才能 Apply。
单候选区域可以从 GUI 完成真实贴回。
失败时日志可复盘。
成功后模型更新且 undo/redo 可用。
multi-face patch 可进入真实 Apply 流程。
```


# P1：workspace 与日志规范

## T7.1 workspace 规范

每个 candidate 一个目录：

```text
workspace/session_YYYYMMDD_HHMMSS/region_0001/
    candidate.json
    boundary_report.json
    local_input.stl
    crop_report.json
    autosurface_stdout.log
    autosurface_stderr.log
    fit_region.log
    local_output.stp
    local_output_autosurface.igs
    crop_stl_path.txt
    crop_stp_path.txt
    patch_artifact_report.json
    patch_import_report.json
    patch_preview_report.json
    patch_apply_state.json
    multiface_patch_analysis.json
    replacement_report.json
    validation_report.json
```

data 目录镜像输出：

```text
data/crop_stl/<relative_dir>/<name>.stl
data/crop_stp/<relative_dir>/<name>.stp
data/crop_stp/<relative_dir>/<name>_autosurface.igs
# strategy suffix is allowed, e.g. <name>_mechanical.stp / <name>_mechanical_autosurface.igs
```

验收：

```text
失败时能根据 workspace 复盘。
成功时能保存完整过程文件。
每个阶段都有 report。
workspace 中记录 data/crop_stl、data/crop_igs、data/crop_stp 的真实路径。
```

## T7.2 最小同步 RegionPatchJob

文件：

```text
src/jobs/RegionPatchJob.h
src/jobs/RegionPatchJob.cpp
```

状态：

```text
Pending
AnalyzingBoundary
CroppingStl
RunningGeomagic
ImportingPatch
PreviewReady
ApplyPending
AnalyzingPatch
Replacing
Validating
Applied
Rejected
Failed
Cancelled
```

验收：

```text
成功路径状态顺序正确：
Pending → AnalyzingBoundary → CroppingStl → RunningGeomagic → ImportingPatch → PreviewReady → ApplyPending → AnalyzingPatch → Replacing → Validating → Applied。
失败路径进入 Failed 或 Rejected。
Cancelled 不继续执行后续阶段。
redo 不重新运行 Geomagic。
redo 复用已有 fit_region.log、crop_stp 文件和导入后的 patch 信息。
```

---

# P2：批量处理与缓存

## T8.1 RegionJobManager

文件：

```text
src/jobs/RegionJobManager.h
src/jobs/RegionJobManager.cpp
tests/test_region_job_manager.cpp
CMakeLists.txt
```

任务：

```text
管理多个 accepted candidates。
默认 Geomagic 串行。
支持取消。
支持状态查询。
支持多个 patch overlay preview。
```

验收：

```text
多个 candidate 可排队生成 patch。
任一 candidate 生成失败不影响其他 candidate。
GUI 能查看每个 candidate 状态。
```

## T8.2 GeomagicJobCache

文件：

```text
src/external/geomagic/GeomagicJobCache.h
src/external/geomagic/GeomagicJobCache.cpp
```

cache key：

```text
source STEP hash
source STL hash
local crop STL hash
candidate face ids
boundary edge ids
crop margin
AutoSurface 参数
autosurface_pipeline.py version
```

说明：

```text
不能只按 candidate id 缓存。
如果 local STL、crop margin 或 AutoSurface 参数变化，必须缓存失效。
```

验收：

```text
cache 命中不重复运行 wrapCore。
参数变化后 cache 失效。
candidate 变化后 cache 失效。
local crop STL 内容变化后 cache 失效。
```

## T8.3 批量 patch 生成与批量应用

文件：

```text
src/app/AppController.h/.cpp
src/gui/MainWindow.cpp
src/gui/LogPanel.cpp
```

任务：

```text
1. 对多个 accepted candidates 批量生成 local STL。
2. 批量运行 Geomagic backend。
3. 将输出同步写入 data/crop_stl、data/crop_igs、data/crop_stp。
4. 批量导入 patch。
5. 用户逐个或批量预览。
6. 用户确认后逐个或批量 Apply。
```

验收：

```text
批量生成失败不会破坏已有成功结果。
批量 Apply 前必须能逐个查看 preview。
任一 Apply 失败不影响其他未 Apply candidate。
批量 redo 不重新运行 Geomagic。
```

---

# 当前不要做

```text
1. 不继续把 OCCT PlaneRegionMerge 作为主线增强。
2. 不实现 OCCT 自由曲面拟合。
3. 不直接用 STL 裁剪边界作为最终 CAD 边界。
4. 不无条件信任 Geomagic patch 外边界。
5. 不在 patch preview 前自动替换主模型。
6. 不默认并行启动多个 wrapCore.exe。
7. 不把临时 STL / IGS / STEP / log 提交到仓库。
8. 不在单候选 Apply 闭环稳定前做全模型批量自动合并。
9. 不删除 Plane/Sphere 旧代码，只保留为 experimental / baseline。
10. 不把真实 Geomagic 调用加入自动单元测试。
11. 不让 redo 重新运行 Geomagic。
12. 不把 T6 设计成只支持 one-face patch；multi-face / complex patch 必须作为主路径。
12. 不在生产逻辑中写死 `local_candidate_0179_mechanical.stp` 或任何当前样例路径。
```