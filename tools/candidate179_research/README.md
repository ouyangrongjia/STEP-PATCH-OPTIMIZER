# candidate-179 独立实证工具

这是针对指定四片基线的独立 OCP/SciPy 原型，不接入产品 Apply，也不改变原生 C++ 质量门。当前保留策略与产物见 [最新实证](../../data/runs/20260921_candidate179_designer_layout/RUN.md)。前两轮继续保留数值对照，当前几何为五片、4484 控制点的 `five_knot_smooth`。

按用户要求，归档仅保留当前策略的局部面、支撑面、缝合整件 STEP，原始区域和三方对比图，以及各轮协议和指标。旧候选几何、ZIP、日志和 Creo 临时资源已清理，原始共享样本仅保留在最新运行目录。下面的工具用于在新目录重现实验，会生成中间产物；不要在精简归档目录直接重跑汇总或拟合入口。原始整件需要另外提供，指定基线归档保持原样。

用 `requirements.txt` 准备 Python 环境后，从仓库根目录执行：

```powershell
./tools/candidate179_research/run.ps1 `
  -PythonExe <python.exe> `
  -SourceStep <原始整件.stp> `
  -BaselineDirectory <指定四片证据目录> `
  -OutputDirectory data/runs/<新的运行号> `
  -CreoRoot <可选的Creo安装目录>
```

`prepare.py` 沿归档 FaceId 提取原始曲面和固定采样；`fit_graph.py` 运行六组消融；`refine_extrapolation.py` 运行两组已声明的求解器/外域控制。两次精确缩小支撑域不计作新拟合。`build_faces.py` 保留四片分区，将旧曲线投影到新曲面，因此必须按自由重建诊断解释外边移动。

`evaluate.py`、`diagnose.py`、`boundary_audit.py` 与 `partition_audit.py` 分别保存统一投影、局部/反向误差、真实外边及相邻面、投影裁剪环重叠证据。`integrate.py` 用固定 0.001 mm 尝试整件缝合；`run_creo.ps1` 复用项目现有 Creo 函数，对每个导入零件和根装配分别检查。

两个数值测试独立于真实样例：

```powershell
<python.exe> tools/candidate179_research/test_numerics.py
```

注意：289/625 是母曲面设计控制点数，不能直接当作导出 STEP 复杂度。共享母曲面经 STEP 写出会生成四份支撑记录；精确分域后的实际控制点总数以 `crop.json` 为准。所有结果都保留 `can_apply=false`，单独的 STEP 可读、BRepCheck 或 SameParameter 通过不构成原边界验收。

水密优先的补充实证位于 `data/runs/20260921_candidate179_hard_boundary/RUN.md`。`hard_boundary.py` 固定基线 knot、平面投影和全部边界控制点，仅优化面内高度；两种配置分别保留位置、位置及已有一阶导数。`hard_boundary_audit.py` 沿同一批原始区域样本检查形状、反向偏差和原边/相邻面关系。`test_hard_boundary.py` 用解析曲面独立检验弯曲参数域中的物理一、二阶导数。该补充实验没有降低 9645 控制点，也不将旧导数冻结称为已经实现相邻面 C1。

放开旧人工缝和控制网的实证位于 `data/runs/20260921_candidate179_designer_layout/RUN.md`。`designer_layout.py` 原样保留原外边曲线，构造新布局，以共享主变量约束人工缝；`designer_audit.py` 检查原形状、真实外侧面、面内节点跳变及实际 Face 轮廓。`designer_visuals.py` 输出真实控制网、误差图和起始映射对照；`designer_report.py` 汇总已完成结果。`integrate.py`、`render.py` 支持可选 `--patch-file patch_faces.step`，旧默认文件名保持。

`integrated_boundary_audit.py` 从完整整件 STEP 回读中匹配替换面及外侧邻面，再查双侧曲线落面和边容差，补充缝合前的局部检查。

运行全部独立数值测试：`<python.exe> -m unittest discover -s tools/candidate179_research -p 'test_*.py'`。先复制输入与协议到新的运行目录，再执行报告内命令；拟合入口拒绝覆盖已有候选目录。当前五片结果都是诊断，不能以闭合、平均误差或少控制点替代完整验收。
