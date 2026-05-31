# STEP-PATCH-OPTIMIZER 项目模块文档

> 草案版本：v0.1  
> 目标：明确重构后每个模块的功能、执行标准、测试基准。

---

## 1. 总体依赖方向

```text
GUI
 ↓
AppController
 ↓
Command / Jobs
 ↓
Core Modules
 ├── IO
 ├── B-rep
 ├── Feature
 ├── Merge Candidate
 ├── STL Extraction
 ├── Geomagic Backend
 ├── Patch Import / Replacement
 └── Validation
 ↓
OCCT / wrapCore.exe
```

硬约束：

```text
1. GUI 不直接修改 TopoDS_Shape。
2. 所有真实修改模型的操作必须通过 Command。
3. Geomagic 后端只生成局部 patch 文件，不直接改主模型。
4. Patch 导入和叠加预览不等于替换。
5. 替换必须通过 StrictTopologyGate。
6. Gate 失败必须回滚。
```

---

## 2. App 模块

目录：`src/app/`

职责：

```text
连接 GUI、Command、Job 和 Core 模块。
不实现复杂几何算法。
不直接修改 TopoDS_Shape。
```

建议新增接口：

```cpp
Result openStlFile(const std::filesystem::path& path);
MergePlannerResult previewFeatureBoundedRegions(const FeatureBoundedRegionOptions& options);
Result generatePatchForCandidate(int candidateId, const GeomagicAutoSurfaceConfig& config);
Result importPatchForCandidate(int candidateId);
Result previewPatchForCandidate(int candidateId);
Result replaceCandidateWithPatch(int candidateId);
RegionJobStatus queryRegionJobStatus(int candidateId) const;
```

执行标准：

```text
1. 无 STP 时，候选生成失败。
2. 无 STL 时，STL crop / Geomagic patch generation 失败。
3. 非破坏性预览不进入 undo 栈。
4. patch replacement 成功后进入 undo 栈。
```

测试基准：

```text
打开 STP、打开 STL、预览候选、生成 patch、替换失败回滚、undo/redo。
```

---

## 3. GUI 模块

目录：`src/gui/`

职责：

```text
1. 显示 STP 主模型。
2. 显示 feature edges / protected edges。
3. 显示 FeatureBoundedRegion candidates。
4. 显示 local patch overlay。
5. 显示 Geomagic job 状态、日志、错误信息。
```

执行标准：

```text
1. 预览候选区域不得启动 Geomagic。
2. accepted candidate 才允许生成 local patch。
3. Geomagic 任务不能阻塞 GUI 线程。
4. patch overlay 可清除。
5. replacement 前必须有可读日志或预览结果。
```

测试基准：

```text
候选显示、候选隐藏/接受/拒绝、patch overlay、清除 overlay、失败日志显示。
```

---

## 4. Command 模块

目录：`src/command/`

新增核心命令：

```text
PatchReplacementCommand
```

职责：

```text
1. 接收 candidate + imported patch。
2. 在临时 shape 上尝试替换。
3. 调用 StrictTopologyGate。
4. 成功提交 afterDocument。
5. 失败恢复 beforeDocument。
6. 支持 undo/redo。
```

执行标准：

```text
1. redo 不重新运行 Geomagic。
2. Gate 失败不改变当前 document。
3. replacement 前必须有合法 closed boundary report。
4. 必须优先使用原 STP boundary wire，而不是 Geomagic patch 外边界。
```

测试基准：

```text
成功 execute / undo / redo。
失败 rollback。
source faces 为空失败。
invalid boundary 失败。
```

---

## 5. IO 模块

目录：`src/io/`

现有：

```text
StepReader
StepWriter
ProjectSerializer
```

新增建议：

```text
StlReader
StlWriter
IgesReader，可后置
```

执行标准：

```text
1. STEP 写出后必须支持 roundtrip。
2. STL 读写不做自动对齐或修复。
3. local STL 写出后必须能再次读入。
4. IGS/STEP patch import 不修改主模型。
```

测试基准：

```text
读 STEP、写 STEP、STEP roundtrip、读 STL、写 local STL、读 patch。
```

---

## 6. B-rep 模块

目录：`src/brep/`

职责：

```text
ShapeDocument
FaceIndex
EdgeIndex
TopologyGraph
boundary wire 构造
```

新增建议：

```cpp
class BoundaryWireBuilder {
public:
    Result<TopoDS_Wire> buildClosedOuterWire(
        const ShapeDocument& document,
        const std::vector<EdgeId>& orderedBoundaryEdges) const;
};
```

执行标准：

```text
1. 只能使用 RegionBoundaryAnalyzer 输出的 ordered_boundary_edges 构造 wire。
2. 不允许直接用未排序 candidate.boundary_edges MakeWire。
3. open / branch / multiple loop / hole 均失败。
```

测试基准：

```text
单闭环成功；open boundary、multiple-loop、non-manifold boundary 失败。
```

---

## 7. Feature 模块

目录：`src/feature/`

职责：

```text
只回答哪些边不能跨越。
不负责拟合。
不负责替换。
```

protectedEdges 来源：

```text
automatic feature edges
+ topological feature edges
+ user locked edges
+ model boundary edges
```

执行标准：

```text
1. free edge / multiple edge 必须视为 protected。
2. user locked edge 优先级最高。
3. 检测宁可保守，不要跨越明显特征线。
```

测试基准：

```text
用户锁边进入 protectedEdges；二面角大于阈值进入 StrongFeature；free edge 进入 TopologicalFeature。
```

---

## 8. Merge 模块

目录：`src/merge/`

当前已有：

```text
MergeCandidate
MergePlanner
MergeRegionGrower
RegionBoundaryAnalyzer
SameDomainUnifier
Plane/Sphere/Cylinder/Cone/Torus region merger
```

新增主线：

```cpp
enum class MergeCandidateType {
    SameDomain,
    PlaneLike,
    CylinderLike,
    ConeLike,
    SphereLike,
    TorusLike,
    FreeformG1,
    FreeformG2,
    FeatureBoundedRefit,
    Unknown
};
```

执行标准：

```text
1. FeatureBoundedRefit 由 protectedEdges 分割生成。
2. 候选区域不跨越 feature / locked / topological boundary。
3. MVP 只允许单 closed outer wire、无洞候选进入真实替换。
4. PlaneLike/SphereLike 保留为 experimental。
```

测试基准：

```text
不跨越 protectedEdges；单闭环可执行；多闭环/hole 高风险或拒绝。
```

---

## 9. STL 模块

目录：`src/stl/`

职责：

```text
保存原始 STL mesh。
根据 candidate bbox / boundary 裁剪局部 STL。
输出 crop report。
```

第一版裁剪策略：

```text
candidate bbox
→ expand by margin
→ keep triangle if triangle bbox intersects expanded bbox
→ write local_input.stl
```

执行标准：

```text
1. STL 只用于拟合点集，不决定最终 CAD 边界。
2. local STL 应略大于 candidate 区域。
3. 空裁剪结果失败。
```

测试基准：

```text
triangle count > 0；margin 增大 triangle count 不减少；local bbox 覆盖 candidate bbox。
```

---

## 10. Geomagic Backend 模块

目录：`src/external/geomagic/` 与 `scripts/geomagic_wrap/`

职责：

```text
1. 调用 wrapCore.exe。
2. 运行 autosurface_pipeline.py。
3. 传入 JSON config。
4. 输出 IGS / STEP patch。
5. 捕获 stdout/stderr/log。
6. 支持 mock backend 测试。
```

建议配置：

```cpp
struct GeomagicAutoSurfaceConfig {
    std::filesystem::path wrapCorePath;
    std::filesystem::path scriptPath;
    std::filesystem::path inputStlPath;
    std::filesystem::path outputIgesPath;
    std::filesystem::path outputStepPath;
    std::filesystem::path workDir;
    bool autoMerge = true;
    bool adaptiveFit = false;
    int numPatches = 1;
    std::vector<int> fallbackNumPatches = {2, 4, 8};
    double detail = 0.35;
    double tolerance = 0.05;
    std::string geometry = "Organic";
    int timeoutSeconds = 1800;
    bool convertIgesToStep = true;
};
```

执行标准：

```text
1. 默认 Geomagic worker = 1。
2. 每个 candidate 独立 workspace。
3. 必须输出 result.json。
4. AutoSurface 成功不等于 replacement 成功。
```

测试基准：

```text
mock success、mock failure、timeout、output missing。
```

---

## 11. Patch 模块

目录：`src/patch/`

职责：

```text
1. 导入 Geomagic 输出 patch。
2. 生成 ImportedPatchInfo。
3. Viewer overlay 预览。
4. 为 replacement 提供 patch shape / surface。
```

执行标准：

```text
1. patch import 不修改主模型。
2. patch preview 可清除。
3. imported patch 外边界不能直接作为最终边界。
4. replacement 优先使用原 STP boundary wire。
```

测试基准：

```text
有效 patch 导入成功；无效 patch 失败；主模型不变；bbox 合理。
```

---

## 12. Validate 模块

目录：`src/validate/`

新增主 gate：

```text
StrictTopologyGate
```

硬检查：

```text
BRepCheck valid
free edge 不增加
multiple edge 不增加
solid count 不变
shell closure 不破坏
bbox 不异常
STEP export 成功
STEP re-import 成功
re-import 后再次 valid
```

执行标准：

```text
1. Gate 是最终提交条件。
2. Gate 失败必须 rollback。
3. Gate report 必须可写入日志和 JSON。
```

测试基准：

```text
free edge 增加失败；BRepCheck false 失败；solid count 改变失败；STEP roundtrip 失败。
```
