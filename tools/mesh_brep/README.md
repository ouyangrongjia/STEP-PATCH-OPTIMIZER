# 给定分区的纯网格曲面网络实验

`reconstruct.py` 接收二进制 STL 和离散布局 JSON，运行真实网格邻接上的 mean-value 参数化、共享边界系数的三维 B-spline 联合拟合、OCCT 构造与 STEP 往返。输出始终为 `DiagnosticOnly/can_apply=false`；公差验收由独立评估给出。当前证据见 [本轮报告](../../data/runs/20260929_mesh_brep/RUN.md)。这不是无标签 STL 的自动分区器，也不调用原 CAD 或既有高度场拟合入口。

## 复跑 B

从仓库根目录运行，Python 环境沿用 [已有依赖](../candidate179_research/requirements.txt)。本轮未安装新的包，实测版本和许可元数据见运行目录 `environment.json`。

```powershell
python -B -m tools.mesh_brep.reconstruct `
  --stl data/runs/20260929_mesh_brep/inputs/candidate_fine/input.stl `
  --layout data/runs/20260929_mesh_brep/inputs/candidate_fine/layout.json `
  --controls 25 --output data/runs/new_mesh_reconstruction

python -B -m tools.mesh_brep.evaluate `
  --stl data/runs/20260929_mesh_brep/inputs/candidate_fine/input.stl `
  --candidate data/runs/new_mesh_reconstruction/network.step `
  --count 30000 --reverse-count 12000 --seed 2909280 `
  --output data/runs/new_mesh_reconstruction/confirmation.json

python -B -m unittest tools.mesh_brep.test_mesh_brep -v
```

入口拒绝覆盖已有输出目录。`--controls` 是每个参数方向的控制点数；本原型各面使用同一均匀三次节点空间。`--normal-policy chart` 仅用于重现首版在人工分区边界处使用单侧邻域的消融；默认 `global` 对明确声明光顺的接缝使用完整网格邻域。

当前交付只保留 candidate-179 的局部和缝合整件入口。STEP 包含新生成的连续样条面，不把输入三角形逐个导出为平面 Face。

## 保持原区域边界、光顺重拟合并缝合

这是原始纯网格后端之后的 **CAD 辅助接口阶段**。`fair_interface.py` 从 B 的五片结果取得连接关系和初始接缝，原 CAD 只提供精确外边界；内部位置与法向目标来自 STL。原边界系数固定，内部接缝变量共享，三维联合拟合使用四点 Gauss 积分及切平面探针，`--corner-planes` 对四个接缝外端点施加硬切平面约束。该五片适配器不适用于任意拓扑。

```powershell
python -B -m tools.mesh_brep.fair_interface `
  --mesh-result data/runs/20260929_mesh_brep/B_fine_25 `
  --stl data/runs/20260929_mesh_brep/inputs/candidate_fine/input.stl `
  --layout data/runs/20260929_mesh_brep/inputs/candidate_fine/layout.json `
  --region data/runs/20260929_mesh_brep/baseline_cold/original_region.step `
  --corner-planes --output data/runs/new_candidate179_interface

python -B -m tools.mesh_brep.assemble_candidate `
  --audit data/runs/20260929_mesh_brep/baseline_cold/input_audit.json `
  --patch data/runs/new_candidate179_interface/network.step `
  --output data/runs/new_candidate179_sewn
```

独立整件缝合仍使用既有 `0.001 mm` 容差，成功条件是有效 Solid、零自由边；不放大容差补缝。局部原边界落面使用更严格的 `1e-7 mm` 检查。`check_interface.py` 对原邻面单独验证接口法向，`render.py --before <STEP> --after <STEP> --output <PNG> --run <运行目录>` 生成同视角真实曲面及全部参数线。当前四个文件及残余问题见 [交付入口](../../data/runs/20260929_mesh_brep/candidate179/README.md)。

## 输入合同与适用范围

`layout.json` 包含 `schema=1`、`units="mm"`、按 STL 三角形顺序排列的 `triangle_labels`、每片四个 `corners` 索引及四条首尾衔接的 `chains`、`smooth_seams=true`、距离/接缝/构造公差，以及文字 `input_kind`。索引对应 STL 顶点按 float32 XYZ 字典序去重后的数组；三角形次序保留。完整实例在本轮 `inputs/*/layout.json`。

后端不接受 CAD 曲线、CAD UV、精确法向或旧控制网字段。法向和目标点均来自输入三角网格；采样是三角形内部重心插值，未回 CAD 求值。STL 只合并数值完全相等的 float32 坐标，不做按距离近邻焊接。生成器对本轮 CAD 面重复节点的合并另行记录误差，不能泛化为任意薄壁焊接策略。

当前支持连通、定向、流形网格，以及已给定的四边盘状参数图；孔洞、分支边界、退化和 UV 翻转会被拒绝。真实锐特征应由独立的两侧法向处理；本原型只接受所有内部接缝被明确声明为光顺的输入，尚未实现混合锐边合同。自接触和输入量化造成的信息损失需要在上游处理。

共享链先确定唯一弧长参数，相邻面的边界控制点按方向映射到同一组未知量；角点固定为网格顶点。位置闭合在缝合前成立。网格法向提供软切平面约束，不能保证端点 G1，实测最大接缝法向差必须单列。OCCT 缝合固定为 `1e-7 mm`，不以提高容差制造水密。

拟合变量为 XYZ；位置、法向切平面、二阶参数导数光顺项共同构成稀疏线性系统。纯网格后端的边界样本为软形状约束，内部接缝系数相等为硬约束。原对照保留 `quadrature_order=1` 以复现；它不能完整积分三次样条的导数平方，新的接口光顺采用 `quadrature_order=4`。四个硬切平面方程通过稀疏约束系统求解，实际残差写入报告。样条面内简单节点为 C2；非零采样 Jacobian、无 UV 翻转、BRepCheck 有效均不足以证明整个连续曲面无自交。

## 数据生成、A 和独立评估

- `generate.py --kind cad --source <原始区域.step> --deflection .003 --output <新目录>`：只在生成阶段读取原 CAD。candidate-179 的生成器声明四个原边角点索引，用原网格全局参数图剪分为五片，然后只发布 STL 和离散结构标签。未使用旧候选曲面作真值。
- `generate.py --kind closed|wrapped --intervals 64 --output <新目录>`：独立网格夹具，生成公式不进入重建包。
- `diagnostic_a.py --reference <profile_baseline 输出目录> --output <新目录>`：CAD 辅助的 A。同一 4324 控制点空间比较曲面 XY 反求和网格 UV，两组均拟合 XYZ；精确 CAD 目标投到三角面取重心 UV，误差保存在 `mesh_correspondence.json`。
- `evaluate.py --source <原始区域.step>`：允许独立评估读取原 CAD。训练/选型后再运行；不能将结果回流到 B 调参。检查器对所有落到真实面边界或未满足驻点条件的投影调用 OCCT，修复单纯截断 Newton 在斜边界上的偏差。
- `extra_checks.py`：原接口、整件 A 缝合、曲面两两求交和外边逼近误差。`localize.py` 定位最终法向失败；`render.py` 输出真实 STEP 同视角表面、全部节点参数线及实际 UV 图。
- `profile_source.py`：计入原整件导入、区域提取、网格及布局生成。`summarize.py` 汇总分项事实，缺失和不适用项保留为 N/A。

旧基线复算仍使用 `tools/candidate179_research/profile_baseline.py`；参数见 [旧工具说明](../candidate179_research/README.md)。A 的两个 XYZ 目标与旧高度拟合在公平项及边界法向目标上不同，因此只能在 A 两组之间归因 UV 的变化；旧基线对照不是单因素实验。

## 文件不可读条件下复跑

Windows 上运行 `isolate.py`。它将五个纯网格模块及两个数据输入复制到独立目录，由父进程使用 `CreateFileW`、共享模式 0 锁住指定根目录中的 CAD/缓存文件；子进程先实际测试 Python 与 OCCT 读取失败，再重建。退出时释放句柄，不修改原文件或 ACL。

```powershell
python -B -m tools.mesh_brep.isolate `
  --stl <input.stl> --layout <layout.json> --controls 25 --output <新隔离目录> `
  --deny-root <原CAD和缓存根目录> --deny-root <另一个缓存根目录> `
  --probe <原始.step> --probe <旧候选.step> --probe <CAD辅助reference.npz>
```

`lock_manifest.json` 记录锁定清单，`result/isolation_evidence.json` 记录读取探针和实际数据文件。该机制验证受信任后端的数据隔离，不作恶意原生代码沙箱声明。

## 算法依据与许可

参数化依据 Floater 的 [Mean value coordinates（2003）](https://doi.org/10.1016/S0167-8396(03)00002-5) 及 [CGAL 官方参数化说明](https://doc.cgal.org/latest/Surface_mesh_parameterization/index.html)、[权重公式](https://doc.cgal.org/latest/Weights/group__PkgWeightsRefMeanValueWeights.html)。实现独立按公式编写，没有复制 CGAL 源码或增加 CGAL 依赖。凸边界、盘状图和正权重的适用条件之外不作无翻转保证；实际翻转及伸长逐图检查。

CAD 检查参考 [OCCT Sewing](https://occt3d.com/dev/doc/refman/html/class_b_rep_builder_a_p_i___sewing.html) 和 [PTC GEOM_CHECKS](https://support.ptc.com/help/creo/creo_pma/r11.0/usascii/model_analysis/modelcheck/geomchk.html)。Creo 报告只说明对应导入对象和模板的检查结果，几何拟合门继续独立执行。
