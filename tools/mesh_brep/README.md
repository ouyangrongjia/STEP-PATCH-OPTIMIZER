# 给定分区的 STL 曲面网络重建

## 共用平滑骨架的分区重建入口

`skeleton_reconstruct.py` 先构造唯一的三次样条公共边界，再让相邻分区拟合同一套边界。大盘状区域采用中央域和外围域，狭窄或回折区域按源法向锥选择投影方向；只在原标签内部调整拟合域，不改变 GUI 分区。全部几何目标来自原始 STL，不调用 Geomagic Wrap。参考 STEP 仅由独立计数器读取面数。

```powershell
$env:OPENBLAS_NUM_THREADS='1'
$env:OMP_NUM_THREADS='1'
python -m tools.mesh_brep.skeleton_reconstruct `
  --stl <原始STL> --partition <已有分区.npz> `
  --reference <对应参考STEP> --output <不存在的新目录> `
  --smoothing .12 --workers 8 `
  --boundary-mode fair-chains --support-solver bounded `
  --normal-field crease-fans --normal-filter-steps 2 --normal-weight .01 --area-weighted `
  --repair-smooth-joins `
  --creo-root <Creo安装目录> --native-probe <creo_body_probe.exe>
```

边界平滑区分沿原表面的移动和离开两侧源三角形平面的误差；默认分别为 0.12 mm 和 0.003 mm。共享交汇点保留，连续边界极值进入拟合约束，避免只检查离散采样点漏掉窄峰。相邻面共用三维曲线；交换时使用该曲线的精确短子段，保持几何光顺而不增加曲面数。输出 `step/model.step`、全部 `regions/region_*.step`、独立 `quality/` 与可选 Creo 原生体及显示证据。

上述改进配置需要 `osqp>=1.0,<2`（本轮实测 1.1.3）。`fair-chains` 在位置与两侧源平面约束内最小化公共三次曲线的弯曲能量；拓扑失败时只调整同一条公共边及其两侧支撑。连续交点检查会复核内核的重合候选，防止短回环漏检，也避免把近停滞曲线误判后反复缩回锯齿。`bounded` 使用物理尺度下的法向与曲率目标，以及独立的位置和边界约束；窄条区域的法向项不会随宽度缩小而失效。法向滤波保留 STL 坐标和固定分区标签，几何评估另用未滤波的源法向。

有界拟合还检查节点区间内部的 Gauss 探针及源网格边中点。发现顶点和重心样本之间的鼓包时，先加入实际超限位置重新求解，再决定是否增加控制点或细分域。旧缓存缺少这项检查时会重新验证。统一入口现在默认调用 `distance_guard.py`，将装配后的超预算检测和局部修复包含在同一次运行中；无需另写补救脚本。

整件每轮同时做面积抽样和实际 STEP 逐面抽样（默认每面 1024 点），保存每个面的最差位置，不以全局前若干名代替全覆盖。首轮预防性选择超过原距离预算 95% 的面；后续只在实际超预算时启动下一轮修复，并纳入接近预算的候选。通过实际支撑控制点、节点及裁剪顶点核对 STEP 面与拟合域的对应关系。局部优先回退到兼容旧系数，否则以原距离预算的 90% 重拟合；局部加密探针要求回到 95% 以下，原连续边界约束保持。共享同一支撑的面片整体修复，原曲线、主曲线、分区归属不变。新曲面重新装配并使用新随机种子验证，最多修复三轮。次数耗尽、身份不明或无合格候选时保留失败记录，不导出通过的整件与分区件；没有提高最终距离预算。局部重拟合使用未滤波的源折角法向，目前不支持非零边界法向耦合目标的自动修复。

曲线网络已经完成、支撑已经验证的缓存，使用以下统一入口；它保留现有主曲线和移动交点，不再经旧骨架生成器：

```powershell
python -m tools.mesh_brep.skeleton_reconstruct `
  --stl <原始STL> --partition <已有分区.npz> `
  --surface-fit <完整拟合目录> --reference-faces <独立测得的参考面数> `
  --output <不存在的新目录> --no-render
```

`--surface-fit` 与 `--from-fit` 互斥：前者保留已完成曲线网，后者继续旧骨架拟合。可选 `--prior-fit <旧拟合目录>` 提供系数回退候选，缺少或不兼容的旧系数自动改为局部重拟合。`--distance-repairs`、`--distance-samples-per-face`、`--distance-seed` 分别控制修复上限、逐面样本数和初始种子。`distance_guard/run.json` 保存各轮选择、原预算、局部试算、失败和新采样结果，最终入口仍为 `step/model.step` 与 `regions/`。距离抽样通过仅代表已测位置合格，不代表认证 Hausdorff、G1/G2、外部 CAD 或可见质量通过。

有界求解分别记录求解器状态与候选的实际约束残差；对病态系统可做等价变量缩放重试，也可保留满足全部线性约束的边界罚项候选，再进入上述连续几何检查。求解器停止容差严于候选可行性检查。自适应节点聚集导致最大控制点预算仍求解失败时，允许在相同预算内用均匀 C2 节点重试，保留已经加密的约束，不放宽几何公差。

`--repair-smooth-joins` 检测相对于原 STL 异常增大的接缝法向差，只对小邻域提出共用支撑面方案；每组最多四个拟合域，仍分别保留原分区的裁剪边界和三角形归属。通过距离、裁剪拓扑、邻接接缝及区域内部法向检查后才接受。成功组的内部接缝位于同一张 C2 曲面上；源锐边、非单值投影或导致邻区变差的方案会被拒绝。它不会把整件模型放进同一个拟合系统，也不代表未修复接缝全部满足 G1。可单独运行 `python -m tools.mesh_brep.shared_support --run <round_N> --output <新目录> --workers 8`，再把输出传给统一入口的 `--from-fit` 完成 CAD 验收。`--charts` 仅用于显式局部诊断，自动模式按实测接缝排序。

`--boundary-normal-weight` 是额外边界切平面罚项的实验选项，默认 0；本轮强加该项曾引入新的局部褶皱，未作为推荐配置。`--repair-charts` 可依据外部 CAD 诊断选择需重新构造公共边界的源拟合域；诊断只选择位置，不向拟合器提供 CAD 几何。修复会沿两条公共链围成的裁剪域补齐另一侧，避免只回退窄小面的一边形成陡坡。较大邻面的其他边不随之重构，但共享曲线变化仍使相邻支撑缓存失效。报告分别记录请求域与实际扩展域；修复后仍须完整复验。

共用支撑提案的数值接受不是视觉验收：本轮有追加提案降低了局部 P95、也通过了 Creo，却产生窄带波纹，最终未采用。统一入口保留 `visual_review_required=true` 和 `can_apply=false`，实际交付仍依据同视角原生法向图复核。

历史默认 `chain` / `least-squares` 保留用于复现。`fair-network` 是移动整段标签接口交汇点的实验模式，在复杂主样例上尚未通过全部边界拓扑检查，不属于当前推荐配置。`--boundary-plane-budget` 显式控制离开源平面的预算；本轮放宽到 0.009 mm 的对照效果变差，推荐保持默认 0.003 mm。

`--from-fit <本路线的 round_N>` 可复用相同输入、配置和公共曲线的支撑面；变化的共享边界会使两侧缓存失效。缓存只复用已通过连续边界检查的域，仍重新执行实体装配、STEP 回读和外部 CAD 检查。不完整分区禁止导出。目录拒绝覆盖；失败保留原因。默认 `--profile-mode installed` 使用安装自带导入配置；可显式选择 `preserve_splines` 使用产物内配置，不修改 Creo 安装。只有原生体检查通过才能报告 Creo 实体；可见质量仍需查看实际对比图。

`bounded` 的公平项使用精确积分的张量 Gram 矩阵，缓存带有公平项版本；旧版本支撑会重算。`--area-weighted` 按源三角形面积分配位置与法向目标权重，源坐标和硬误差约束保持不变。`--fairness-domain source` 是可选源多边形积分实验，需要 Shapely 2.1；`--fairness-order 3` 是可选三阶导公平项，仅支持默认矩形域。`--normal-angle-limit` 为采样法向上限。这些选项要求 `bounded`；除面积加权外，尚无证据支持把它们作为通用改进设置。

`python -m tools.mesh_brep.surface_boundary --run <round_N> --output <新目录> --regions <区域列表>` 在相邻源三角形域的并集距离管内优化公共曲线。`--tube-limit <mm>` 显式指定真实三角形距离预算，默认沿用原平面预算；它与原折线移动预算分别检查，不能把源平面距离当作有限三角形距离。连续区间距离上界、几何总转角和曲率平方积分一起检查，拒绝参数弯曲能下降但几何波动反而增大的提案。`--include-creases` 允许对含源折角的链提出优化，仍须通过后续特征与 CAD 检查；`--separate-domains` 改为逐个相邻源域约束，保留为诊断选项，可能锁住原三角网格造成的阶梯边界。

输出只是 `boundary_candidate`，保存需重算的全部相邻区域和原曲线回退数据。用 `skeleton_reconstruct --from-fit <候选> --refit-regions <required_refit_regions 中的全部区域>` 保留整件覆盖并重拟合受影响支撑，或用 `skeleton_graph --from-fit <候选> --refit-supports` 全部重算。必须重新通过裁剪环交叉、原环绕序、装配和实际 CAD 检查，不允许套用旧支撑直接导出。新提案导致拓扑冲突时，优先恢复该提案的原曲线，再复核邻边；避免通过反复收紧无关邻边放大锯齿。`--tangent-planes` 保留为端点切平面的诊断选项。

`python -m tools.mesh_brep.normal_repair --run <完整拟合目录> --output <新目录> --regions <区域列表> --angle-limit 45 --workers 4` 保持公共曲线和源三角形归属，只重拟合域内探针超限的支撑面。除原训练位置外，法向锥还检查节点区间 Gauss 点和源边中点，新增约束后重解；失败保留原支撑和原因。这里的角度针对拟合源法向场，不代表全件原始法向、G1/G2 或自交检查通过，输出仍须完成整件验证。

`boundary_quality.compare` 在相同源边链上比较真实几何转角与弯曲量；未配对的拆分链另计。`boundary_quality.seams` 在实际共享曲线参数上比较两侧支撑法向；源二面角仅作锐边代理。`python -m tools.mesh_brep.step_stratified --step <实际STEP> --partition <原分区.npz> --output <新JSON> --samples-per-face 1024` 逐面分层抽样，核对真实裁剪域并使用未滤波源法向，补充面积采样可能漏掉的微小面；这套逐面均衡统计须与原面积采样分开报告。

`python -m tools.mesh_brep.network_fairing --run <完整拟合目录> --output <新边界目录> --regions <区域列表> --feasible-seed-only` 联合优化公共曲线和可移动交点，锁定锐角及编辑范围外的接口。连续有限源域距离与移动预算仍分别验证，数值求解超时只产生待检查提案。随后使用 `network_support.guard(..., geometric_guard=True)` 逐边复核曲率、裁剪交叉、绕序和源域距离；回退共享交点时同步协调全部相邻曲线，避免短边卷曲被总体改善掩盖。`network_fairing.straighten` 仅在源域、端点切向及两侧支撑均允许时接受直线替代。

`python -m tools.mesh_brep.network_support --run <已检查边界目录> --output <新拟合目录> --workers 4` 验证并按需重拟合两侧支撑，保留相容系数；失败时保存已接受提案，禁止进入 CAD 装配。移动过交点的缓存不能用旧 `skeleton_graph` / `skeleton_reconstruct --from-fit` 重新生成骨架。完成支撑验证后，使用 `skeleton_reconstruct --surface-fit` 保留精确系数和公共交点，自动完成装配、距离修复闭环、分区导出及可选 Creo 验证；`graph_reassemble` 和 `graph_export` 保留为单步诊断工具。

`python -m tools.mesh_brep.seam_fairing --run <完整拟合目录> --output <新目录> --regions <区域列表> --smooth-degrees 30 --seam-weight .1 --iterations 2` 在公共曲线不变时联合拟合两侧实际单位法向，包含不同参数坐标系的导数变换。源折角以上的接缝不参与光滑目标，保留源位置、内部探针、连续落面误差和局部法向退化检查。逐面回溯后还须通过整体实际法向能量检查；不能把这一采样优化称为精确 G1/G2。实现测试包含法向 Jacobian 有限差分、共享端点、有限源域孔洞、连续裁剪交叉及有效裁剪内新增边界鼓包的回退。

`python -m tools.mesh_brep.curve_routes --run <完整拟合目录> --output <新边界目录> --regions <区域列表> --movement <mm> --controls 24` 沿同一分区接口跨越人工面片交点，拟合一条低控制点的主曲线；交点随后取主曲线上的位置，每条公共边是主曲线的精确子区间。真实转角和有歧义的分支保留，闭环暂不改造。添加 `--internal-only` 可重构同一分区内的光滑切分线，固定已有主曲线的交点并排除源锐边；两类主曲线分阶段保存，后续阶段不会覆盖前一阶段的系数。这改变了曲线网的几何组织，不再要求新曲线插值每个旧交点。源网格坐标、三角形和标签仍不变，尚未执行切网格。

主曲线候选依次交给 `route_layout`、`route_branches`、`route_compact`，均使用 `--run <上一阶段> --output <新目录>`。前者检查连续裁剪环，并在同一原分区内吸收不可裁剪的小域；`--absorb-transport-slivers` 另外吸收交点移动后留下的细碎域。`route_branches --movement <mm>` 修复受交点牵动的支线；`route_compact` 只移除相同两侧面片、同一主曲线上的二度节点。随后使用 `network_support --boundary-normal-weight 0` 重拟合受影响曲面，再进行固定曲线的 `seam_fairing`。主曲线候选不能套用逐边独立回退：应保留主曲线及其子段的一致性，任何失败均须解决后再装配。

`python -m tools.mesh_brep.route_audit --before <基线拟合> --after <候选拟合> --output <新JSON> --movement <mm> --tube .012` 通过原始网格边身份配对重构前后的曲线，分别检查端点一致、主曲线子段的一至二阶导数、有限源三角形距离管和相对源链的移动。源域使用沿网格连通的局部距离带，避免把移动后的边界锁在原标签边线上。默认移动预算仍为 0.12 mm，扩大预算必须显式声明；它与曲线离开源表面的预算、曲面整体距离预算分别验收。曲线网减少节点不等于 STEP 短边数量同步减少，曲线 C2 也不等于两侧曲面 G1/G2。

`python -m tools.mesh_brep.source_features --run <round_N> --output <新目录> --regions <区域列表>` 在源折角处分开支撑域，不改变给定分区和三角形归属。输出 `layout_candidate` 必须重拟合。`skeleton_reconstruct --from-fit <候选> --refit-regions <区域列表>` 可选择性重算这些区域，同时保留全件覆盖；它与仅生成局部诊断的 `skeleton_graph --regions` 不同。改变的公共边会使邻侧缓存一起失效。更细的布局仍须单独通过 Creo，不能以源折角保留或 OCCT 水密代替交换验收。

本路线已在复杂主样例上获得区域合面与边界改善。最新 [主曲线与共享网络重构](../../data/runs/whole-partition-brep-v1/native-v7/result_v8/RUN.md) 保存曲线网重组、拟合域合并、两侧支撑重拟合、真实 STEP 图、独立检查和可恢复缓存。历史 [共享网络与过渡曲面优化](../../data/runs/whole-partition-brep-v1/native-v6/result_v7/RUN.md)、[边界与法向诊断](../../data/runs/whole-partition-brep-v1/native-v5/result_v6/RUN.md)、[Creo 零提示基线](../../data/runs/whole-partition-brep-v1/native-v4/result_v5/RUN.md) 和原 [902 面版](../../data/runs/whole-partition-brep-v1/native-v3/result_v4/RUN.md) 保留。它支持给定分区重建，不承诺任意输入必定通过全部几何和 CAD 验收。

## 保留的先拟合后合并入口

当前入口完全使用本仓库算法、NumPy/SciPy、网格工具和 OCCT，不调用 Geomagic Wrap，也不复用其曲面。输入为原始 STL 和固定分区 NPZ；公共边界统一构造，曲面拟合保留源区域。参考 STEP 仅由独立评估器读取实际面数，拟合阶段只接收整数预算。

```powershell
python -m tools.mesh_brep.graph_reconstruct `
  --stl <原始STL> --partition <已有分区.npz> `
  --reference <对应参考STEP> --output <不存在的新目录> `
  --creo-root <Creo安装目录> `
  --native-probe <creo_body_probe.exe>
```

这是实验入口，已有少面主样例通过 Creo 原生闭合实体检查，但局部几何质量仍未全部满足要求。验收同时要求面数严格小于参考、弯折处无明显褶皱、实际 Creo 水密实体，以及平滑后仍共用的区域边界。所有阶段保留状态和失败，不用拓扑通过代替几何通过。

入口依次执行分区内高度曲面拟合、同标签域合并、共享边界装配、三维主边界 STEP 导出、独立几何评估及可选 Creo 验证。装配器保留内腔和多个闭合壳层；已测内腔样例通过共享边界平滑形成 Creo 原生实体，光顺性仍单独检查。默认增加源三角形内部采样与二阶公平项，减少顶点之间的鼓包。`--from-fit <已完成的拟合目录>` 可以复用相同 STL 和分区的原生曲面缓存，缓存需通过投影检查。所有输出目录必须尚不存在。

对自生成 STEP 无法离散化的面，先核对完整 B-spline 控制点、节点矢量及输入身份。唯一匹配成功时只收紧对应源拟合域，避免最近点投票扩大修复范围；身份不明或经外部 CAD 改写的面，仍从实际边界定位源域及其一圈邻居。Creo 再导出件的自由边、无效面，以及无自由边的闭合无效 quilt 都纳入诊断。`--max-refinements` 默认允许两轮，每轮另存完整证据，并重新检查实际面数。诊断 CAD 只用于选择待处理域；重拟合的几何数据仍来自原 STL。无法定位故障、超过轮数、面数超预算或未通过原生体检查，都不能标为合格成品。结果路径以最终 `run.json` 的 `model` 字段为准。

STEP 回读中出现的无效裁剪面也进入平滑边界局部回退。重复试验可用 `--boundary-history <先前运行目录> [更多目录]` 复用已定位的边界失败；入口检查 STL 和分区身份，再按原三角形归属映射到当前拟合域，不复用旧支撑几何。该提示不会跳过新 STEP 与 Creo 的检查。

Creo 失败定位到启用了平滑的曲线时，先让相关共用曲线恢复源折线表达，再考虑增加拟合面数。几何重拟合后按原三角形身份保留已有曲线回退，避免编号变化使同一失败被重复尝试；每轮仍独立重测。

Creo 仅承担独立验收；安装有 Pro/TOOLKIT 的环境可用以下命令编译只读原生体检查器。只有导入检查不足以确认水密，需保留原生体记录与实际显示证据。

当前自动检查使用产物内的 `import_mm.dip`，设置外部精度、保留 B-spline 和 XYZ 边界偏好。安装自带配置另行验证；主样例已发现默认配置不能形成活动实体，不能把自定义配置下成功推广为任意导入设置成功。

```powershell
cmake -S tools/mesh_brep/creo_body_probe -B build/creo-body-probe `
  -G "Visual Studio 17 2022" -A x64 "-DCREO_ROOT=<Creo安装目录>"
cmake --build build/creo-body-probe --config Release
```

`whole_reconstruct.py` 保留早期多项式路线的复现接口。公共 C2 边界平滑可通过 `--boundary-smoothing <毫米预算>` 显式启用；曲线按原网格边分段但精确保留连续几何，裁剪失败时只回退相关曲线。该选项尚未通过复杂模型的全部验收，不默认启用。

`--compact-edges` 在启用边界平滑时将整条共用链表示为一个拓扑边，并沿用局部裁剪失败回退；这是减少边数量及交换体积的实验选项，最终仍检查真实 STEP 和 Creo。边界历史也可读取原生装配运行中的无效面，按源三角形映射到新域。

可选 `graph_fair_optimize` 对已拟合支撑面做有界二次规划平滑，保持面数及分区，另需 OSQP。其输出仍需交给统一入口完成独立 STEP 与 Creo 检查，不以优化目标下降作为合格结论：

```powershell
python -m tools.mesh_brep.graph_fair_optimize --run <原生拟合目录> --output <新平滑目录> `
  --normal-field crease-fans --boundary-normal-weight .1
python -m tools.mesh_brep.graph_reconstruct --stl <原始STL> --partition <已有分区.npz> `
  --reference <对应参考STEP> --from-fit <新平滑目录> --output <新验收目录> `
  --creo-root <Creo安装目录> --native-probe <creo_body_probe.exe>
```

`crease-fans` 按 STL 折角连接顶点周围的三角形法向，避免区域标签在原本光滑的边上引入不同目标法向；保留 `regional` 用于既有试验复现。`graph_join_audit` 在固定源边采样上检查拟合支撑面的接缝，仅用于导出前诊断。`step_outliers.localize_forward` 直接复核既有最差正向采样点到原生面的距离，区分曲面偏差和离散化遗漏，不覆盖原指标。局部修复新生成的缓存保存各域边界预算，后处理不得放松该预算。

平滑后处理还可显式试验 `--boundary-normal-limit <度>` 和 `--boundary-smoothing <毫米>`。前者在有限边界样本上施加法向锥硬约束，不能保证连续 G1，已有复杂样例多数域无解或未收敛；后者先拟合唯一共用 C2 曲线，再将边界法向投影到垂直于曲线切向的平面，仍独立检查原始 STL 边界误差。失败域保留原曲面；这些选项不作为质量合格的默认承诺。

`--boundary-target-tolerance <毫米>` 可另外收紧共用曲线样本到支撑面的残差上限；不放松原 STL 边界预算，也不等价于连续曲线精确落面。无可行解的域保留原支撑和失败记录。

`--normal-filter-steps <次数>` 在 `crease-fans` 下启用原网格上的双边法向滤波。空间尺度取相邻三角形重心距离中位数，角度尺度取特征角的一半，原锐边阻断关系固定；仅改变拟合目标法向，不移动 STL 顶点或改变区域标签。默认关闭，独立质量检查仍使用原始 STL。

统一入口也可通过 `--fairing --condition-normals` 串联上述平滑及狭长域参数方向修复。后者从实际接缝异常中选择法向投影接近退化的域，重新选取法向锥中心方向；仅在必要时增加原分区内部的拟合域，仍受最终参考面数门约束。两个开关保持显式启用，尚不能作为任意曲面质量保证。

`--feature-split-angle <度>` 允许在已有拟合域内部沿原 STL 折角断开支撑面。按平滑程度聚合源三角形时，对折角两侧施加不能同组的约束，避免普通连通分量通过折角端点绕行后仍落入同一面。它不改 GUI 区域标签或移动源网格，也不跨区域合并；新增面数仍受同一参考预算约束。原二面角只是特征代理，过度切分会超预算；该选项保持显式启用，与 `--condition-normals` 合用时采用法向锥参数方向，不能仅凭导出前预览判定合格。

`python -m tools.mesh_brep.export_regions --assembly <装配目录> --output <新目录>` 从实际原生装配件导出各标签的开放分区 STEP。导出前逐一核对支撑曲面和原拟合域身份，导出后核对每件回读面数。整体实体检查仍由整件 STEP 承担。

`step_outliers.track_source_points` 可通过固定源三角形及重心坐标跨版本追踪已知几何异常，核对真实 STEP 支撑身份、裁剪域内外、对应位置距离及原三角形法向差；不会用新的随机样本覆盖已知失败，也不把对应位置距离混写为最近距离。

`regional_target.py` 将公共边界与区域内部控制点分别求解。`regional_rectangle.py` 另行验证把拓扑边分段和曲面支撑布局分开：一条支撑边可包含多个共用的拓扑边，避免每个区域交汇点都增加一张曲面。两条路线均仍在实验中；旧试验产物已按要求清理，保留 [历史指标摘录](../../data/runs/whole-partition-brep-v1/native-v2/result_v3/metrics/previous_experiments.md)。

NPZ 包含 `mesh_vertex_original`、`mesh_triangle`、逐三角形 `face_instance_id`，须与 STL 坐标及顺序匹配。当前使用毫米单位、闭合定向二流形输入，分区由调用者提供。依赖 NumPy、SciPy、cadquery-ocp、VTK、Pillow、Numba、meshoptimizer 和 PyMeshLab；Creo 仅用于独立验收。

此前商业试验保存在 `legacy_wrap_reconstruct.py` 和运行目录历史记录中，不能作为当前重建后端。清理仅涉及本工作树继承的旧产物；其余工作树及原始 STL/STEP 未改动。

## 冻结的 candidate-179 局部实验

当前 candidate-179 推荐入口为 `compact.py`，交付统一见 最终 B 目录（历史路径：`../../data/runs/20260929_mesh_brep/candidate179_B_final/README.md`）。它保留五片连接关系，用 C2 自适应外链、较小的共享接缝基底和 XYZ 联合拟合减少支撑控制点，避免把原 CAD 的重复节点直接传入曲面。独立接口挂接 `attach_boundary.py` 只生成与原 3D 曲线相容的 pcurve，不改变网格重建的支撑系数。具体数值、失败方案和两档网格复核见 本轮记录（历史路径：`../../data/runs/20260929_mesh_brep/compact_b/RUN.md`）。

```powershell
python -B -m tools.mesh_brep.compact --stl <input.stl> --layout <layout.json> --output <新目录>
python -B -m tools.mesh_brep.attach_boundary --mesh-result <新目录> --region <原接口区域.step> --output <接口目录>
python -B -m tools.mesh_brep.assemble_candidate --audit <原整件输入记录.json> --patch <接口目录/network.step> --output <整件目录>
```

默认径向与中央方向各 13 点；外链平滑残差预算取输入接口公差的 1/10。所有表面为三次简单节点样条，G1 仍按实际法向检查。`isolate.py --backend compact` 复跑此入口；`creo_export.ps1` 与 `creo_topology.py` 核对 Creo 往返后的真实面数。此专用原型要求已声明的五片布局，并非任意拓扑或无标签 STL 的自动分区。

以下为保留的原双路线与精确 CAD 接口对照；它们继续用于复现实验历史。

`reconstruct.py` 接收二进制 STL 和离散布局 JSON，运行真实网格邻接上的 mean-value 参数化、共享边界系数的三维 B-spline 联合拟合、OCCT 构造与 STEP 往返。输出始终为 `DiagnosticOnly/can_apply=false`；公差验收由独立评估给出。当前证据见 本轮报告（历史路径：`../../data/runs/20260929_mesh_brep/RUN.md`）。这不是无标签 STL 的自动分区器，也不调用原 CAD 或既有高度场拟合入口。

## 复跑 B

从仓库根目录运行，Python 环境沿用 [已有依赖](../candidate179_research/requirements.txt)。上面的有界重建配置另需 OSQP；原始 B 入口不依赖它。

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

独立整件缝合仍使用既有 `0.001 mm` 容差，成功条件是有效 Solid、零自由边；不放大容差补缝。局部原边界落面使用更严格的 `1e-7 mm` 检查。`check_interface.py` 对原邻面单独验证接口法向，`render.py --before <STEP> --after <STEP> --output <PNG> --run <运行目录>` 生成同视角真实曲面及全部参数线。当前四个文件及残余问题见 交付入口（历史路径：`../../data/runs/20260929_mesh_brep/candidate179/README.md`）。

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
