# STEP-PATCH-OPTIMIZER 当前阶段 TODO

> 草案版本：v0.7-t6-multiface-replacement
> 当前主线：**候选区域预览 → STL 局部裁剪 → Geomagic AutoSurface 生成 IGS/STP patch → patch 叠加预览 → 用户点击 Apply → 真实贴回与边界缝合 → StrictTopologyGate 验证**。  
> 核心调整：Geomagic 后端采用 `wrapCore.exe --script` + `FIT_REGION_*` 环境变量传参；当前真实脚本只要求 input/output/log，`config.json` / `result.json` 只作为 C++ 后端兼容和 mock 测试结构，不作为真实 wrapCore 调用的必需输入输出。新增 `PatchArtifactLocator` 作为 T5 入口，生产逻辑必须根据 local STL / GeomagicAutoSurfaceResult / candidate artifact 动态定位 patch，禁止写死当前真实样例文件名。T5.4 已完成 Apply 占位状态机；T5.4.1 已完成 stale patch preview state 安全清理；T6 必须以 multi-face / complex patch replacement fragment 为主路径，不能假设 Geomagic 输出 1 个 B-rep face。

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
T6.5 已将该占位入口替换为真实 AppController / GUI Apply 流程。
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
已完成。
进入 T6 前已补齐，防止旧 patch preview state 跨 STEP 文件、跨 undo/redo、跨旧合并命令或跨模型刷新残留。
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
[x] 打开新 STEP 成功后，清除 viewer patch overlay 和 AppController patch state。
[x] refreshDocumentViews() 刷新主 ShapeDocument 时，清除 patch overlay 和 patch state。
[x] undo / redo 成功后，清除 patch overlay 和 patch state。
[x] 旧 SameDomain / Plane / Sphere 合并命令成功修改主 ShapeDocument 后，清除 patch overlay 和 patch state。
[x] 清除后 refreshPatchApplyAction()，Apply Patch 按钮禁用。
```

当前实现：

```text
1. AppController 在 openStepFile / undo / redo / 旧合并命令成功后清理 current patch state。
2. MainWindow::refreshDocumentViews() 刷新主模型时清理 viewer overlay 和 controller patch state。
3. 清理后刷新 Patch Apply 按钮门控。
```

验收：

```text
[x] 打开新 STEP 后 patchPreviewReady=false，currentPatchStatus=NotGenerated。
[x] undo / redo 后 patchPreviewReady=false，Apply Patch 按钮禁用。
[x] 旧 patch 不可能被用于新 ShapeDocument 的 T6 replacement。
[x] 清理逻辑不删除 data/crop_stp / data/crop_stl 文件，只清理当前 UI/controller 状态。
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
T5.4.1 stale patch state cleanup（DONE）
→ T6.0 PatchReplacement 输入/报告结构 + MultiFacePatchAnalyzer（DONE）
→ T6.2 StrictTopologyGate 最小可用版（DONE，提前于 T6.1 落地）
→ T6.1 BoundaryConstrainedPatchBuilder：multi-face replacement fragment（DONE）
→ T6.3 PatchReplacementCommand（DONE，最小可用 Command 管线）
→ T6.4 Sewing / ShapeFix / SameParameter 集成（DONE，最小 repair + gate 管线）
→ T6.5 AppController / GUI 接入真实 Apply（DONE，真实 Apply 已接入 CommandHistory + 严格水密 Gate）
→ T6.5.1 Apply failure diagnostics（DONE，repair / Gate / roundtrip 诊断已进入 GUI report）
→ T6.6 Industrial Adaptive Sewing（DONE，最小 C++ adaptive repair 已接入）
→ T6.6.1 Crop Boundary Diagnostics（DONE，验证原 STP loop → STL crop → Geomagic patch 是否丢边界）
→ T6.6.2 Process Status Panel（TODO，实时显示当前阶段和参数）
```

## T6.0 PatchReplacement 输入结构与 MultiFacePatchAnalyzer

状态：

```text
已完成。
本阶段只实现 PatchReplacementInput、PatchReplacementReport、validatePatchReplacementInput 和 MultiFacePatchAnalyzer。
未构造 replacement，未修改 ShapeDocument，未接入 Command / GUI / StrictTopologyGate / sewing / ShapeFix。
```

文件：

```text
src/patch/PatchReplacementInput.h
src/patch/PatchReplacementReport.h
src/patch/MultiFacePatchAnalyzer.h
src/patch/MultiFacePatchAnalyzer.cpp
tests/test_multiface_patch_analyzer.cpp
CMakeLists.txt
tests/test_validation.cpp
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
    std::vector<TopoDS_Edge> edges;
    std::vector<TopoDS_Edge> outerEdges;
    std::vector<TopoDS_Edge> internalEdges;

    bool bboxValid = false;
    bool brepCheckValid = false;
    bool hasAtLeastOneFace = false;
    bool isSingleFace = false;
    bool isMultiFace = false;
};
```

当前实现：

```text
[x] PatchReplacementInput 只保存 const 指针输入：document / candidate / boundary / importedPatch / artifactPaths / previewReport。
[x] PatchReplacementReport 记录 candidate/source 统计、patch 统计、replacement 统计、failureReason、message 和 warningMessage。
[x] PatchReplacementFailureReason 已覆盖缺失输入、invalid boundary、preview not ready / high risk、import failed、empty patch shape、invalid BRep/bbox、no patch faces 等 T6.0 失败原因。
[x] validatePatchReplacementInput 不要求 patchFaceCount == 1；patchFaceCount > 1 设置 usedMultiFacePatch=true 并允许继续。
[x] MultiFacePatchAnalyzer 遍历 importedPatch.shape 中的 TopoDS_Face / TopoDS_Edge / TopoDS_Shell / TopoDS_Solid。
[x] MultiFacePatchAnalysis 保存 faces 和 edges，并设置 bboxValid / brepCheckValid / hasAtLeastOneFace / isSingleFace / isMultiFace。
[x] outerEdges / internalEdges 第一版按 edge usage count 粗分：被一个 face 使用为 outer，被两个及以上 face 使用为 internal。
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
[x] one-face patch 可分析成功。
[x] multi-face box patch 可分析成功。
[x] compound patch 可分析成功。
[x] empty patch 失败，message 清楚。
[x] BRepCheck 失败时 input validation 失败，reason 清楚。
[x] patchFaceCount > 1 不失败。
[x] 输入缺失不崩溃。
[x] highRisk preview 被拒绝。
```

## T6.1 BoundaryConstrainedPatchBuilder：multi-face replacement fragment

状态：

```text
已完成。
本阶段实现 BoundaryConstrainedPatchBuilder 最小可用版。
multi-face patch 走主路径：从 MultiFacePatchAnalysis.faces 构造 replacement compound fragment，并保留 internal patch seams。
one-face patch 走特例路径：BoundaryConstrainedPatchBuilder 先输出 imported patch face 作为 replacement fragment，并记录后续仍需 StrictTopologyGate 验证的 warning；T6.5 PatchReplacementCommand 会在 one-face source / one-face patch 场景中用 imported surface + 原 STP boundary wire 重建 trimmed face。
本阶段不修改 ShapeDocument，不创建 Command，不调用 Geomagic，不接入 GUI，不做 sewing / ShapeFix / final submit。
```

文件：

```text
src/patch/BoundaryConstrainedPatchBuilder.h
src/patch/BoundaryConstrainedPatchBuilder.cpp
tests/test_boundary_constrained_patch_builder.cpp
CMakeLists.txt
tests/test_validation.cpp
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
[x] one-face patch 走简单 face fragment special path。
[x] multi-face patch 走主路径：保留 Geomagic patch 的内部 face network，构造 replacement compound fragment。
[x] replacement fragment 的外部边界仍以原 STP candidate outer boundary 为参考。
[x] Geomagic patch 的外边界只作为辅助几何，不作为最终合法性依据。
[x] 不强制把 multi-face patch 合并为 1 张面。
[x] 不因为 patchFaceCount > 1 返回 unsupported。
[x] 如果输入或 boundary 不合法，返回明确失败原因，而不是 silent fallback。
```

第一版允许的工程化策略：

```text
A. 直接 multi-face fragment strategy：
   - [x] 提取 imported patch 的所有有效 faces。
   - [x] 根据 candidate bbox / patch bbox 做基本 mismatch warning，不作为最终提交依据。
   - [x] 构造 TopoDS_Compound 作为 replacement fragment。
   - [x] 保留 patch 内部边。
   - 后续由 T6.4 sewing + T6.2 gate 判断是否可提交。

B. Boundary bridge strategy：
   - 原 STP outer boundary wire 必须作为最终边界参考。
   - 如果 patch outer boundary 与 original outer wire 不一致，记录 boundary mismatch。
   - 可以生成需要 sewing 的 replacement fragment，但不能直接提交；必须经过 StrictTopologyGate。

C. one-face special strategy：
   - [x] 如果 imported patch 只有一个 face，第一版直接使用该 face 作为 replacement fragment。
   - 这是特例，不是 T6 主假设。
   - [x] Builder 未直接 trim 时记录 warning；T6.5 Command 层可使用原 STP boundary wire 重新 trim，后续仍必须经过 StrictTopologyGate。
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
[x] one-face patch 可生成 replacement face 或 fragment。
[x] multi-face box patch 可生成 replacement fragment，且不因 faceCount>1 被拒绝。
[x] synthetic patchFaceCount=12 不因 multi-face 被拒绝。
[x] replacement fragment 保留内部 patch seam 信息。
[x] boundary invalid 时失败。
[x] imported patch analysis 无 face 时失败。
[x] 失败 reason/message 可读。
[x] 不修改主 ShapeDocument。
```

## T6.2 StrictTopologyGate 最小可用版

> StrictTopologyGate 应提前于 PatchReplacementCommand 落地。只要 T6 开始修改模型，就必须先有 gate，否则真实替换失败很难回滚和定位。

状态：

```text
已完成。
本阶段实现 StrictTopologyGate 最小可用版，并按更稳推进顺序提前于 T6.1 / PatchReplacementCommand 落地。
Gate 只评估 before/after ShapeDocument 与 replacement report，不修改 ShapeDocument，不构造 replacement，不调用 Geomagic，不重新裁剪 STL。
```

文件：

```text
src/validate/StrictTopologyGate.h
src/validate/StrictTopologyGate.cpp
tests/test_strict_topology_gate.cpp
CMakeLists.txt
tests/test_validation.cpp
```

检查：

```text
[x] beforeDocument / afterDocument 必须存在且有 shape。
[x] afterDocument 必须通过 BRepCheck。
[x] after free edge 不得比 before 增加。
[x] after multiple edge 不得比 before 增加。
[x] solid count 默认必须不变。
[x] before 有 shell 时 after shell count 不能变成 0。
[x] before/after bbox 必须有效，且 after bbox 不能相对 before 异常偏移或缩放。
[x] requireStepRoundtrip=true 时必须 STEP export 成功并 readback 通过 BRepCheck。
[x] source face count 与 replacement face count 记录，但 replacement face count > 1 不是失败条件。
```

multi-face 规则：

```text
[x] replacementFaceCount 可以大于 1。
[x] Gate 不以 replacementFaceCount > 1 作为失败。
[x] replacementFaceCount > 1 时设置 multiFaceReplacement=true，并记录 warning。
[x] Gate 重点检查拓扑合法性、free edges、multiple edges、shell/solid 一致性和 STEP roundtrip。
[x] 如果 face count 没有下降，不失败，只记录 warning，由后续优化策略决定。
```

验收：

```text
[x] gate 失败返回清晰 reason/message。
[x] report 结构包含 before/after stats、free/multiple edge、BRepCheck、STEP export/roundtrip 和 replacementFaceCount。
[x] gate 失败不允许后续 Command 提交。
[x] STEP roundtrip 失败时拒绝。
[x] multi-face replacement fragment 可进入 gate。
[x] identical valid box passes。
[x] missing after shape fails。
[x] free edge increase fails。
[x] multi-face replacement accepted。
[x] face count not reduced only warns。
[x] valid box export + roundtrip passes。
```

## T6.3 PatchReplacementCommand

状态：

```text
已完成最小可用版。
本阶段新增 PatchReplacementCommand，通过 Command 层执行 PatchReplacementInput 校验、MultiFacePatchAnalyzer 分析、BoundaryConstrainedPatchBuilder 构造 replacement fragment、StrictTopologyGate 验证、成功提交 afterDocument、失败 rollback，并支持 undo/redo。
multi-face imported patch 是主路径：patchFaceCount > 1 不作为 unsupported，report.usedMultiFacePatch / replacementFaceCount 会记录 multi-face fragment。
当前最小 afterDocument 策略：保留 imported patch top-level shape 作为 gated afterDocument candidate；若它不能保持 before/after 拓扑、bbox、solid/shell、STEP roundtrip，StrictTopologyGate 会失败并 rollback，主 ShapeDocument 不变。
当前没有删除 candidate source faces，没有执行 sewing / ShapeFix / SameParameter，没有接入 GUI，没有调用 Geomagic，没有重新裁剪 STL，没有从固定路径读取 patch。
redo 只重新提交缓存的 afterDocument，不重新运行 Geomagic、不重新读取 patch、不重新裁剪 STL。
```

文件：

```text
src/command/PatchReplacementCommand.h
src/command/PatchReplacementCommand.cpp
tests/test_patch_replacement_command.cpp
CMakeLists.txt
tests/test_validation.cpp
```

任务：

```text
1. [x] 输入 PatchReplacementInput，并在 Command 构造时复制 candidate / boundary / importedPatch / artifactPaths / previewReport，避免持有 GUI 临时对象裸指针。
2. [x] 不允许写死 data/crop_stp/local_candidate_0179_mechanical.stp 或任何固定 patch 路径。
3. [x] execute 从 CommandContext 保存 beforeDocument。
4. [ ] 删除 / 替换 candidate source faces 留给 T6.4；T6.3 仅构造 replacement fragment 并构造 gated afterDocument candidate。
5. [x] 接入 BoundaryConstrainedPatchBuilder。
6. [x] 支持 multi-face replacement fragment，不因 patchFaceCount > 1 返回 unsupported。
7. [ ] sewing / ShapeFix / SameParameter 留给 T6.4。
8. [x] 调用 StrictTopologyGate。
9. [x] Gate 成功才提交 afterDocument。
10. [x] Gate 失败 rollback，主 ShapeDocument 不变。
11. [x] 支持 undo/redo。
12. [x] redo 不重新运行 Geomagic、不重新裁剪 STL、不重新读取固定 patch 路径，只复用缓存 afterDocument。
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
[x] invalid input 失败且不修改 document。
[x] multi-face patch 不作为 unsupported。
[x] gate 失败不改变 document。
[x] gate 失败报告包含 rollbackApplied=true，failureReason=GateFailed。
[x] 最小成功路径可 undo/redo。
[x] redo 不重新运行 Geomagic、不依赖固定文件路径。
[x] multi-face patch replacement path 有 synthetic test 覆盖。
[x] production command source 不包含当前真实样例固定 patch 文件名。
```

## T6.4 Sewing / ShapeFix 集成

状态：

```text
已完成最小可用版。
PatchReplacementCommand 现在在 BoundaryConstrainedPatchBuilder 构造 replacement fragment 后，使用 BRepTools_ReShape 对 candidate source faces 执行真实替换尝试：第一个 source face Replace 为 replacementShape，其余 source faces Remove。
replacementShape 可以是 Face / Compound / Shell；multi-face compound fragment 不作为 unsupported。
随后对临时 after shape 执行 repair pipeline：BRepLib::SameParameter、ShapeFix_Wire、ShapeFix_Face、BRepBuilderAPI_Sewing。
repair 前后记录 face/edge/shell/solid、free edge、multiple edge 统计，并写入 PatchReplacementReport。
StrictTopologyGate 仍是最终提交门：Gate passed 才提交 afterDocument，Gate failed 必须 rollback，主 ShapeDocument 不变。
redo 仍只复用缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新读取 patch、不重新运行 repair pipeline。
本阶段不接 GUI，不调用 Geomagic，不读取固定 patch 路径，不绕过用户确认，不把 STL crop boundary 或 Geomagic patch outer boundary 当作最终 CAD boundary。
```

文件：

```text
src/patch/PatchReplacementReport.h
src/command/PatchReplacementCommand.h
src/command/PatchReplacementCommand.cpp
tests/test_patch_replacement_command.cpp
CMakeLists.txt
tests/test_validation.cpp
```

任务：

```text
对替换后的临时 shape 执行必要修复：
- [x] BRepTools_ReShape 替换第一个 candidate source face，并移除其余 source faces。
- [x] BRepLib::SameParameter。
- [x] ShapeFix_Wire 遍历 after shape 中的 wires。
- [x] ShapeFix_Face 遍历 after shape 中的 faces，并通过 ReShape 写回 fixed face。
- [x] BRepBuilderAPI_Sewing 尝试 sewing；如果 sewing 结果为空或丢失 solid 拓扑，则记录 warning 并保留 pre-sewing shape。
- [x] repair 前后统计写入 PatchReplacementReport。
- [x] StrictTopologyGate passed 才提交 afterDocument。
- [x] StrictTopologyGate failed 时 rollback。
```

multi-face 要求：

```text
1. [x] sewing / repair 允许 replacement fragment 内部存在多张 patch faces。
2. [x] 内部 seam 不作为 unsupported；PatchReplacementCommand 不因为 internalEdges 或 patchFaceCount > 1 失败。
3. [x] replacement fragment 与周围原模型连接后若产生新的 free edge / multiple edge，由 StrictTopologyGate 拒绝。
4. [x] repair 前后记录 face/edge/shell/solid、free edge、multiple edge 统计。
```

验收：

```text
[x] invalid input 失败且不修改 document。
[x] multi-face patch 仍不作为 unsupported。
[x] repair pipeline 在最小成功路径中被调用，并记录 SameParameter / ShapeFix / Sewing 字段。
[x] undo/redo 正常；redo 不重新运行 repair pipeline。
[x] repair 后 Gate 失败 rollback。
[x] 替换后如果 free edge 增加，Gate 拒绝，主 document 不变。
[x] sewing / repair 后 BRepCheck、solid/shell、bbox、STEP roundtrip 仍由 StrictTopologyGate 最终判断。
[x] multi-face patch 内部 seam 可保留，不作为 unsupported。
[x] production command source 不包含当前真实样例固定 patch 文件名。
```

## T6.5 AppController / GUI 接入 Apply 流程

状态：DONE。

文件：

```text
src/app/AppController.h
src/app/AppController.cpp
src/app/MainWindow.h
src/app/MainWindow.cpp
src/validate/StrictTopologyGate.h
src/validate/StrictTopologyGate.cpp
tests/test_patch_apply_state.cpp
tests/test_strict_topology_gate.cpp
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

已完成：

```text
AppController::applyCurrentPatchToCurrentCandidate(candidate, outReport) 已接入真实 PatchReplacementCommand。
Apply 前会重新用当前 ShapeDocument 对当前 candidate 执行 RegionBoundaryAnalyzer。
preview report 的 candidateId / sourceFaceCount 必须与当前 candidate 一致，否则阻止 Apply。
boundary 必须是 single closed outer wire，不允许 holes / non-manifold / branching。
GUI Apply 按钮从占位入口改为调用真实 Controller Apply。
成功后主 ShapeDocument 更新为 Command 提交的 afterDocument，viewer 清除 patch overlay。
成功后清除 cached patch artifact / imported patch / preview report，patchPreviewReady=false，状态置为 Applied，防止 stale patch 重复 Apply。
失败后主 ShapeDocument 保持不变，patch overlay / preview state 保留，状态置为 ApplyFailed。
CommandHistory 已接入，成功后 undo/redo 可切换 before/after document。
redo 只复用 PatchReplacementCommand 缓存 afterDocument，不重新运行 Geomagic、不重新裁剪 STL、不重新导入 patch、不重新 repair。
```

严格水密提交规则：

```text
StrictTopologyGateInput 新增 requireWatertightSolid / requireZeroFreeEdges / requireZeroMultipleEdges / requireRoundtripWatertight。
GUI Apply 路径启用这些严格选项。
before 是 solid 时，after solid count 必须保持一致。
after BRepCheck 必须通过。
after free edges 必须为 0。
after multiple edges 必须为 0。
STEP export 必须成功。
STEP roundtrip readback 必须成功。
roundtrip 后 BRepCheck 必须通过。
roundtrip 后 solid count 必须保持 before solid count。
roundtrip 后 free edges / multiple edges 必须为 0。
```

replacement 边界策略：

```text
multi-face patch fragment 仍然进入主路径，不因 patchFaceCount > 1 或 internal seams 返回 unsupported。
multi-face fragment 是否最终提交仍由 repair + StrictTopologyGate 判断。
one-face replacement 成功路径新增最小边界重建：使用 imported patch face 的 surface，加原 STP candidate boundary wire 重新 trim replacement face。
STL crop boundary 不作为最终 CAD boundary。
Geomagic patch outer boundary 不作为最终合法性依据。
```

验收：

```text
用户必须先看到 overlay preview，才能 Apply。
单候选区域可以从 GUI 完成真实贴回。
失败时日志可复盘。
成功后模型更新且 undo/redo 可用。
multi-face patch 可进入真实 Apply 流程。
成功提交必须保持 solid / watertight；否则 GateFailed rollback。
```

## T6.5.1 Apply 失败诊断增强

状态：DONE。

背景：

```text
GUI 真实样例验证中，candidate 179 的 multi-face patch 已进入 Apply 主路径：
sourceFaceCount=49，sourceBoundaryEdgeCount=26，patchFaceCount=12，replacementFaceCount=12。
Command 已执行 source face replacement、SameParameter、ShapeFix_Face、ShapeFix_Wire、Sewing。
但 repair 前后 free edges 均为 38，Sewing result 因为空或丢失 solid 拓扑未被采用，最终 StrictTopologyGate 以 BRepCheckFailed rollback。
这说明失败点不是 patchFaceCount > 1 unsupported，而是 replacement fragment 与主模型没有形成水密拓扑。
```

目标：

```text
让 GUI Apply failure report 可以直接判断失败是 solid 数丢失、shell 异常、free edge 未消除、multiple edge、BRepCheck、STEP export，还是 STEP roundtrip 后再次破坏。
```

任务：

```text
1. GUI Patch Apply report 显示 PatchReplacementReport 已有 repair 前后 face/edge/shell/solid 字段。
2. PatchReplacementReport 记录 StrictTopologyGateReport 关键字段：
   - before / after face edge shell solid
   - before / after free edge
   - before / after multiple edge
   - before / after BRepCheck
   - stepExportOk
   - stepRoundtripOk
   - roundtrip face edge shell solid
   - roundtrip free edge
   - roundtrip multiple edge
   - roundtrip BRepCheck
3. GateFailed message 保留明确 failureReason，同时把 gate stats 写入 report，供 GUI 和日志展示。
4. 可选 debug artifact：GateFailed 时把 rejected after shape 导出到临时/工作区 debug STEP，路径只写入 report，不作为生产输入，不影响 undo/redo。
5. 不放宽 StrictTopologyGate。
6. 不重新运行 Geomagic。
7. 不重新裁剪 STL。
8. 不把真实样例路径写死进生产逻辑。
```

验收：

```text
ApplyFailed report 中能看到 repair 前后 solid/shell/face/edge、after gate stats、roundtrip stats。
对 candidate 179 这类失败样例，GUI 能明确显示 free edge 未消除、after BRepCheck failed、是否保留/丢失 solid。
失败后主 ShapeDocument 不变，overlay / preview state 保留。
```

完成记录：

```text
已把 StrictTopologyGateReport 的关键 before / after / STEP roundtrip 统计写入 PatchReplacementReport：
face / edge / shell / solid、free edge、multiple edge、BRepCheck、STEP export 和 STEP roundtrip。
PatchReplacementCommand 在 Gate 通过或失败时都会保留 gate failure reason、message、warning 和 gate stats。
GUI Patch Apply report 现在显示 repair 前后 face/edge/shell/solid、repair free/multiple edge、Gate before/after/roundtrip stats 和 watertight 选项。
本阶段未放宽 StrictTopologyGate，未重新运行 Geomagic，未重新裁剪 STL，未写死真实样例路径。
可选 rejected after debug STEP artifact 本阶段未实现，留给后续需要更强复盘证据时再加。
```

## T6.6 Industrial Adaptive Sewing 集成

状态：DONE。

来源：

```text
scripts/industrial_sew.py
```

研究结论：

```text
老师脚本能缝成实体的关键不是单个 API，而是三阶段策略：
1. Geometry fix：ShapeFix_Shape 修复 base / patch，并对 face 做 ShapeFix_Face orientation。
2. Topology unify：ShapeUpgrade_UnifySameDomain 统一 base / patch 的同域拓扑。
3. Adaptive sewing loop：多容差试跑 Sewing，记录每次结果，再按有效 Solid、free edge、BRepCheck、face collapse 选择最佳结果。

当前 T6.5 C++ repair pipeline 只执行一次固定 tolerance sewing，缺少 adaptive tolerance loop、ShapeUpgrade_UnifySameDomain、ShapeFix_Shell、BRepBuilderAPI_MakeSolid、ShapeFix_Solid 和 collapsed guard。
```

必须保持的原则：

```text
1. 不绕过 StrictTopologyGate。
2. 不因为 patchFaceCount > 1 返回 unsupported。
3. multi-face replacement fragment 仍是主路径。
4. internal patch seams 允许保留。
5. STL crop boundary 不作为最终 CAD boundary。
6. Geomagic patch outer boundary 不作为最终合法性依据。
7. 最终提交仍必须以原 STP candidate boundary / topology 为参考，并通过 watertight solid gate。
8. redo 不能重新运行 industrial sewing；redo 只复用缓存 afterDocument。
```

推荐实现路线：

```text
1. 新增 PatchReplacementRepairOptions 字段：
   - bool runShapeFixShape = true
   - bool runUnifySameDomain = true
   - bool runAdaptiveSewing = true
   - bool runShellToSolid = true
   - double preferredSewingTolerance = 0.007
   - double minSewingTolerance
   - double maxSewingTolerance
   - double collapseFaceRatio = 0.5

2. 扩展 PatchReplacementRepairReport：
   - selectedSewingTolerance
   - sewingAttemptCount
   - bestSewingFreeEdges
   - bestSewingFaceCount
   - bestSewingEdgeCount
   - bestSewingShellCount
   - bestSewingSolidCount
   - bestSewingBRepCheckValid
   - bestSewingCollapsed
   - shellToSolidApplied
   - unifySameDomainApplied
   - shapeFixShapeApplied

3. repair pipeline 顺序：
   input after shape
   → ShapeFix_Shape
   → ShapeFix_Face orientation / ShapeFix_Wire / ShapeFix_Face
   → ShapeUpgrade_UnifySameDomain
   → adaptive BRepBuilderAPI_Sewing tolerance loop
   → ShapeFix_Shell
   → BRepBuilderAPI_MakeSolid
   → ShapeFix_Solid
   → ShapeUpgrade_UnifySameDomain
   → 记录 stats
   → StrictTopologyGate

4. Sewing 每次尝试必须记录：
   - tolerance
   - face/edge/shell/solid
   - free edge
   - multiple edge
   - BRepCheck
   - collapsed

5. 结果选择规则：
   - 优先 preferred tolerance=0.007 且 valid solid、未塌缩。
   - 否则选择 valid solid、未塌缩、free edge 最少、face count 最接近输入的结果。
   - 如果没有 valid solid，则只允许作为失败诊断，不可绕过 StrictTopologyGate 提交。
   - Face 数严重塌缩的零自由边结果不能被当成成功。
```

测试要求：

```text
1. adaptive sewing 会尝试多组 tolerance，并记录 selected tolerance / attempt count。
2. preferred tolerance 可用时优先选择 preferred tolerance。
3. collapsed result 不会被当成成功。
4. shell-to-solid path 对可闭合 shell 能生成 solid。
5. multi-face patch 不因 internal seam 或 patchFaceCount > 1 被拒绝。
6. free edge 无法消除时仍 GateFailed rollback。
7. redo 不重新运行 adaptive sewing。
8. GUI report 能显示 selected tolerance、attempt count、best result stats。
```

完成记录：

```text
已新增 src/patch/PatchReplacementRepair.h/.cpp，将 T6.4 匿名 repair 管线升级为可测试的 industrial repair pipeline。
默认 repair 顺序为：ShapeFix_Shape → SameParameter → ShapeFix_Wire → ShapeFix_Face → ShapeUpgrade_UnifySameDomain → shell-to-solid 尝试 → adaptive Sewing tolerance loop → ShapeFix_Solid → final ShapeUpgrade_UnifySameDomain → StrictTopologyGate。
adaptive Sewing 会按 bbox 自适应 tolerance、preferredSewingTolerance=0.007、min/max tolerance 生成多组尝试，并记录 selected tolerance、attempt count、best sewing face/edge/shell/solid、free/multiple edge、BRepCheck 和 collapsed 状态。
结果选择规则保持严格：优先 preferred tolerance 下 valid solid 且未塌缩的结果；否则选 valid solid、未塌缩、free edge 最少的结果；没有 valid non-collapsed solid 时只保留诊断，不把塌缩或丢 solid 的 sewing result 作为可提交成功。
PatchReplacementCommand 已改为调用 PatchReplacementRepair 模块，redo 仍只复用缓存 afterDocument，不重新运行 repair / adaptive sewing。
GUI Patch Apply report 已显示 selected sewing tolerance、sewing attempt count、best sewing stats、best BRepCheck 和 collapsed。
本阶段未放宽 StrictTopologyGate，未调用 Geomagic，未重新裁剪 STL，未写死真实样例路径。
```

## T6.6.1 Crop Boundary Diagnostics / 局部 STL 裁剪边界诊断

状态：DONE。

完成记录：

```text
已新增 CropBoundaryDiagnostics 模块，输入 ShapeDocument、RegionBoundaryAnalysis、local STL crop mesh 和 imported patch shape，输出 CropBoundaryDiagnosticsReport。
诊断会先通过 BoundaryWireBuilder 确认原 STP candidate boundary 是 single closed outer loop，再按 ordered boundary edge 采样，记录 edge id、sample count、3D point、edge length 和 singleClosedOuterLoop。
local STL coverage 通过原 boundary sample 到 local STL triangle 的最近距离统计 min / max / average、missing point count、连续超限区间和 suspected gap segment。
patch boundary coverage 通过原 boundary sample 到 imported patch outerEdges 采样 polyline 的最近距离统计 missing / mismatch segment、max distance 和 suspected gap edge ids。
GUI patch preview 成功后会自动显示诊断 overlay：黄色为原 STP boundary loop，青色为 imported patch outer boundary，红色为 local STL crop coverage issue，洋红为 patch boundary mismatch。
Patch preview report 已追加 T6.6.1 字段：originalBoundarySampleCount、stlCoverageMissingPointCount、stlCoverageMaxDistance、patchBoundaryMissingPointCount、patchBoundaryMaxDistance、suspectedGapCount、suspectedGapEdgeIds、message / warningMessage。
本阶段不修改 ShapeDocument，不调用 Geomagic，不重新裁剪 STL，不改变 PatchReplacementCommand / StrictTopologyGate / redo 语义，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary。
已新增 tests/test_crop_boundary_diagnostics.cpp，覆盖完整覆盖、STL 缺覆盖、patch outer boundary mismatch、无 local STL 时的 patch-only 诊断和 GUI overlay 字段扫描。
```

背景：

```text
T6.6 后 candidate 179 仍 ApplyFailed，但失败形态已经更清楚：
adaptive sewing 将 free edges 从 38 降到 26，best sewing BRepCheck=true 且未塌缩，但 best sewing solid=0，最终 after solid=1 仍有 26 条 free edges，StrictTopologyGate 以 BRepCheckFailed rollback。
GUI 截图显示 Geomagic patch / local STL 对应区域在原 STP boundary loop 附近疑似存在小缺口。
这说明继续盲目调 sewing tolerance 价值有限，必须先量化原 STP boundary loop、STL crop 覆盖和 imported patch outer boundary 之间的偏差。
```

目标：

```text
证明或否定“局部 STL 裁剪未完整覆盖原 STP boundary loop，导致 Geomagic patch 边界缺口，最终 replacement seam 无法闭合”这个假设。
```

任务：

```text
1. 基于 RegionBoundaryAnalysis / BoundaryWireBuilder，对原 STP candidate outer boundary wire 按 edge 采样。
2. 对每条 boundary edge 记录：
   - edge id
   - 采样点数量
   - 采样点 3D 坐标
   - edge length
   - 是否属于 single closed outer loop
3. 将 boundary 采样点与 local STL crop mesh 做最近距离检查：
   - min / max / average distance
   - 超过 tolerance 的点数量
   - 连续超限区间
   - 疑似 gap segment 的 edge id 和参数区间
4. 将原 STP boundary 采样点与 imported patch outerEdges 做最近距离检查：
   - patch outer boundary 是否覆盖原 STP loop
   - patch outer boundary 与原 loop 的最大偏差
   - 缺口 segment / mismatch segment
5. 输出 CropBoundaryDiagnosticsReport：
   - originalBoundarySampleCount
   - stlCoverageMissingPointCount
   - stlCoverageMaxDistance
   - patchBoundaryMissingPointCount
   - patchBoundaryMaxDistance
   - suspectedGapCount
   - suspectedGapEdgeIds
   - message / warningMessage
6. GUI overlay 增加诊断显示：
   - 原 STP boundary loop
   - local STL crop boundary / coverage issue
   - imported patch outer boundary
   - suspected gap segment 高亮
7. 不修改 ShapeDocument。
8. 不绕过 StrictTopologyGate。
9. 不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary。
10. 不写死 candidate 179 或任何真实样例路径。
```

验收：

```text
对 candidate 179 这类失败样例，GUI / report 能明确说明：
1. 原 STP boundary loop 是否完整。
2. STL crop 是否在某些 boundary segment 附近缺覆盖。
3. imported patch outer boundary 是否在某些 segment 附近缺口或偏离过大。
4. 26 条 free edges 是否集中在 suspected gap 附近。
如果诊断证明 crop 没问题，则下一步不能继续怪 STL 裁剪，必须转向 replacement boundary trim / sewing 策略。
```

## T6.6.2 Process Status Panel / 当前进程状态面板

状态：DONE。

背景：

```text
当前 GUI 中 Patch Apply / merge / Geomagic 相关操作缺少过程级反馈。
用户只能看到最终 ApplyFailed report，看不到当前卡在 crop、Geomagic、import、replacement build、repair、adaptive sewing 还是 StrictTopologyGate。
这会导致真实样例调试时只能凭最终报告和截图猜。
```

目标：

```text
增加一个轻量进程状态面板，实时显示当前正在执行的阶段、candidate、关键参数和最近事件。
```

建议文件：

```text
src/gui/ProcessStatusPanel.h
src/gui/ProcessStatusPanel.cpp
src/app/AppController.h
src/app/AppController.cpp
src/app/MainWindow.h
src/app/MainWindow.cpp
tests/test_patch_apply_state.cpp
```

任务：

```text
1. 定义 ProcessStage / ProcessStatusSnapshot：
   - Idle
   - AnalyzingBoundary
   - CroppingStl
   - RunningGeomagic
   - ImportingPatch
   - PreviewReady
   - ApplyingPatch
   - BuildingReplacement
   - Repairing
   - AdaptiveSewing
   - ValidatingGate
   - Applied
   - ApplyFailed
   - CachedUndo
   - CachedRedo
2. 状态面板显示：
   - current stage
   - candidate id
   - source face count
   - boundary edge count
   - local STL path
   - patch STEP / IGS path
   - current sewing tolerance
   - sewing attempt index / count
   - best free edges
   - best solid count
   - latest Gate failure reason
   - latest message / warning
3. AppController / MainWindow 在关键阶段更新状态：
   - boundary analysis start / finish
   - STL crop start / finish
   - Geomagic start / finish
   - patch import start / finish
   - Apply start
   - replacement build
   - repair start
   - adaptive sewing attempt
   - StrictTopologyGate start / finish
   - success / failure
4. 长任务至少做到阶段级实时刷新；adaptive sewing 如果在 GUI 线程可见，应显示每次 tolerance 尝试结果。
5. 失败后状态面板保留最后失败阶段和参数，不自动清空。
6. undo / redo 不应伪装成重新运行 Geomagic / crop / import / repair。
7. 不改变 CommandHistory 语义。
8. 不绕过用户确认。
```

验收：

```text
点击 Apply 后，用户能看到当前执行到 replacement build、repair、adaptive sewing 或 StrictTopologyGate。
adaptive sewing 过程中能看到 tolerance attempt 和当前 best stats。
ApplyFailed 后，面板保留失败阶段、Gate reason、free edge / solid / BRepCheck 摘要。
redo 只显示 cached redo，不显示 RunningGeomagic / CroppingStl / AdaptiveSewing。
```

完成记录：

```text
已新增 src/app/ProcessStatus.h/.cpp，定义 ProcessStage / ProcessStatusSnapshot，并由 AppController 保留当前进程状态快照。
已新增 src/gui/ProcessStatusPanel.h/.cpp，在 GUI 底部输出区加入“进程”页，显示 stage、candidate id、source face count、boundary edge count、local STL、patch STEP/IGS、fit_region log、selected sewing tolerance、sewing attempt index/count、best sewing free/multiple edge、best face/edge/shell/solid、best BRepCheck、repair/adaptive sewing、StrictTopologyGate evaluated/passed/failure、message 和 warning。
MainWindow 已在 STL crop start/finish/failure、Geomagic preview pipeline start/failure、patch import start/finish/failure、Apply start/finish/failure、clear overlay、undo/redo 后刷新面板。
AppController 在 import、preview ready、Apply 早退失败、replacement build、PatchReplacementCommand 成功/失败、undo/redo 后同步状态；ApplyFailed 后保留 PatchReplacementReport 中的 repair/adaptive sewing/Gate 参数。
undo/redo 显示 CachedUndo / CachedRedo，并明确不重新运行 Geomagic、STL crop、patch import 或 repair；redo 仍只复用缓存 afterDocument。
第一版为阶段级 UI 状态面板；PatchReplacementCommand 当前仍同步执行，因此 adaptive sewing 每个 tolerance attempt 的逐步实时刷新未做，后续若引入 job/progress callback 再细化。
本阶段不改变 CommandHistory 语义，不调用 Geomagic，不重新裁剪 STL，不放宽 StrictTopologyGate，不把 STL crop boundary 或 Geomagic patch outer boundary 当最终 CAD boundary。
已扩展 tests/test_patch_apply_state.cpp，覆盖 ProcessStage 字符串、PreviewReady 状态、Apply success/failure 后参数保留、GateFailed 后 repair/Gate 诊断保留，以及 undo/redo cached 状态。
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
