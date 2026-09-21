"""Write the human-readable report from the retained measurements."""
import argparse
import json
from pathlib import Path


def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    run=read(out/'run.json');rows={r['name']:r for r in run['comparison']};a=rows['control'];b=rows['aligned_inner_cubic'];at=read(out/'attribution_audit.json')
    independent=read(out/'confirmation.json');local=read(out/'local_evaluation.json');remaining=read(out/'confirmed_worst_local_evaluation.json');reverse=read(out/'reverse_confirmation.json')
    fmt=lambda x:f'{x:.6f}'
    labels={'control':'直接对照 five_knot_smooth','same_start_replay':'同起点复算（验证）','hard_g1':'原 XY + 共面等式','hard_g1_normal':'原 XY + 共面等式 + 法向权重×10','soft_normal10':'原 XY + 软法向权重×10','linear_xy_soft':'线性横向 XY + 原软约束','linear_xy_g1':'线性横向 XY + 共面等式','aligned_refined_control':'同几何起点加密对照（验证）','aligned_inner_cubic':'保留：内接缝三次曲线 + 节点对齐'}
    table=['| 实验 | STEP 面 / 控制点 | 正向最大 mm | 反向最大 mm | 法向 P95 / 最大 ° | C0 节点跳变 ° |','|---|---:|---:|---:|---:|---:|']
    for name,label in labels.items():
        r=rows[name];table.append(f"| {label} | {r['cad_faces']} / {r['control_points']} | {r['forward_max_mm']:.6f} | {r['reverse_max_mm']:.6f} | {r['normal_p95_deg']:.3f} / {r['normal_max_deg']:.3f} | {r['knot_jump_max_deg']:.6f} |")
    comparison='\n'.join(table)
    note=f'''# candidate-179：连续性、参数映射与节点传播实证

本轮达到阶段性结果 A：保留 **aligned_inner_cubic，5 面 / 4324 控制点**作为下一轮研究候选。原始 26 条外边、顶点位置、连接关系和边容差保持，原公差整件替换及 STEP 回读通过。最大法向门、单方向 25 控制点预算和 Creo GEOM_CHECKS 仍未通过；状态明确为 **DiagnosticOnly / can_apply=false**，不是可直接 Apply 的产品结果。

直接对照是归档提交 `3c810bfe07b2396c36e8338a255c9516524be83e` 的 five_knot_smooth。旧四片 / 9645 点仍在原归档中，未作为拟合真值，也未被替换。旧 49→46 产品回归不属于本轮，未改动或重跑。

**1. 输入、环境与预算。** 原始整件取自 input_audit.json 指定的本地 `03_配件_Clay.stp`（98,478,416 字节，2739 面），原始选区 49 面。原始表面、参考采样和外侧邻面均来自原模型；没有用任何候选整件充当原始输入。源码在独立分支 `experiment/candidate179-continuity-20260921`，从上述干净归档提交建立。主项目当时位于 `experiment/stl-structural-partition`，已有的 CMake、文档和 STL 工具修改均保留。旧四片几何的原运行 dirty 状态不等于归档提交状态。

预算为 100 分钟、最多 6 个新拟合配置，实际采用 6 个新配置和 2 个验证配置；包含数值求解与中央映射修正后的重跑，共保留 12 条拟合执行记录。没有新增付费依赖或上传几何。Python 3.11.9、OCCT 7.9.3 / cadquery-ocp 7.9.3.1.1；其余准确版本见 dependencies.json。采用 Creo 11.0.0.0 和既有 ModelCHECK 模板。C++ 产品模块、StrictTopologyGate、Apply 及 GUI 未修改或执行。

**2. 统一协议与实际结果。** 继续使用原始 12000 个训练点（seed 179）、18000 个评估点（seed 180）、6000 个反向面积采样点（seed 181），边界及接缝每条 385 点。距离单位是毫米，法向是角度；不是相对包围盒误差。保留距离 0.03 mm、Curve-on-Surface 与缝合 0.001 mm、法向 15°、接缝 5°、单方向 25 点的既有门。原边实际容差一直为 1e-7 mm。下表最大值都是采样最大值，不是连续域上界。反向面积样本依赖各候选的真实三角化，采用同一方法和种子，并非相同三维位置的配对样本。

{comparison}

最终控制网为 **99×10、66×10、131×10、130×10、8×8**，外围 4260 点，中央 64 点。相较直接对照，正向 RMS 从 {fmt(a['forward_rms_mm'])} 降到 {fmt(b['forward_rms_mm'])} mm；人工接缝最大法向差从 {a['seam_normal_max_deg']:.3f}° 降到 {b['seam_normal_max_deg']:.3f}°，14 条要求 G1 的外边对原外侧面从 {a['outer_g1_max_deg']:.3f}° 降到 {b['outer_g1_max_deg']:.3f}°。公平化存在代价：正向位置误差 P95 从 {fmt(a['forward_p95_mm'])} 略升到 {fmt(b['forward_p95_mm'])} mm；C1 节点的最大物理高度 Hessian 跳变从约 4.75 升至 5.15 /mm。这些指标没有删去或改成通过。

最终修订后另取 **30000 个全新原始表面样本，seed 9180180**，不用于拟合或选型。直接对照 / 候选的最大法向差为 **{independent['control']['normal_degrees']['max']:.3f}° / {independent['aligned_inner_cubic']['normal_degrees']['max']:.3f}°**，P95 为 {independent['control']['normal_degrees']['p95']:.3f}° / {independent['aligned_inner_cubic']['normal_degrees']['p95']:.3f}°；最大位置误差为 {independent['control']['distance_mm']['max']:.6f} / {independent['aligned_inner_cubic']['distance_mm']['max']:.6f} mm。新增 12000 个独立反向样本（seed 9180181），最大偏差为 {reverse['control']['distance_mm']['max']:.6f} / {reverse['aligned_inner_cubic']['distance_mm']['max']:.6f} mm，最坏点与固定点均与原始裁剪 B-Rep 距离交叉检查。首版中央映射修正前的 seed 9180179 结果也保存在 initial_center 证据中；没有把它当成最终未见样本。

对上一轮最坏点及两个外边转角所在的三个原始 Face 作 77763 点 UV 网格复核，最大法向差从 **{local['control']['normal_degrees']['max']:.3f}° 降至 {local['aligned_inner_cubic']['normal_degrees']['max']:.3f}°**。原最坏 XYZ 约 (-27.223248, 38.647706, -8.050425) mm，在固定评估点上从 25.769° 降到约 0.516°。剩余最坏区域转到原 FaceId 1706 附近，但通过原始 UV / XYZ 对应关系复核，不依赖跨导入 ID 稳定性。该处 25921 点复核最大值为 {remaining['control']['normal_degrees']['max']:.3f}° / {remaining['aligned_inner_cubic']['normal_degrees']['max']:.3f}°：主要是原先已存在的边界窄带问题，仍未解决，不是新的大折痕。

**3. 归因与失败机制。** 复测确认，旧最坏点紧邻第 3 张外围面的 C0 节点；两个 C0 节点都起于原外边约 31.80°、30.26° 的方向转折，但对应原始曲面法向连续。边界转角必须保留，向面内延伸的折痕不能据此当作真实特征。1001 点以上的节点剖面将直接对照跳变峰值细化为约 1.086°；统一 385 点表仍使用原协议的 1.077°。有限偏移两侧的位置差不是曲面裂缝，采样法向差也不等于严格 C1/G1 证明。

仅提高原映射下的法向权重，平均误差略有改善，但节点跳变增大，未保留。把切平面共面条件写成固定 XY 下的线性等式后，节点法向差可以很小，但形状严重变差；最初 Schur 系统病态，正交化独立约束后条件数和残差恢复，形状失败仍在。因此该负结果不是单纯编译或第一版数值失败，也不证明五面或四面表示不可能。线性 XY 下的等式版本虽较好，仍明显损伤形状；没有继续加权调参。

保留的构造把外围面内部 XY 改为外边与人工内边之间的线性横向混合，再用原始形状拟合高度。外边、人工边的 XY 路径、角点 XY、带宽、节点向量和目标权重保持；内部映射与物理空间可表达函数族发生变化。该实验初始点位最大改变 {at['mapping_design']['max_xyz_mm']:.6f} mm，明确属于映射设计对照，**不是原曲面的等几何重参数化**，不将它包装成纯初始化或纯接缝自由度的单因素结果。OCCT 的 [7.9.3 Coons 实现](https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/V7_9_3/src/GeomFill/GeomFill_Coons.cxx)按边界控制点数组构造内部混合系数；本轮数值检查再次证实节点插入前后不能默认保持该映射。

节点传播实验则严格使用同一几何起点：先构造公共粗种子，再在现有曲面中插节点形成加密对照，起点差见 matched_start_audit.json（约 1e-11 mm）。首版还误带入中央 Coons 映射变化，已恢复中央原 XY，保存首版全部几何和 CAD 原始报告后重跑。修正后另一高度初始化与当前直接对照映射的 XY 差不超过 {at['initialization_change_same_final_space']['max_projected_xy_mm']:.2e} mm，终态同参数位置差不超过 {at['final_sensitivity_to_initialization']['max_xyz_mm']:.2e} mm。这是有限精度的稳定性证据，不称为逐位相同。

采用三次人工内接缝后，四个外围面各去掉原来由人工内边引入的 U 节点 0.2、0.4、0.6、0.8，**没有删除任何原外边节点或改变原外边曲线**。控制点只减少 160，但原先最坏的相邻外侧节点段弦长比从约 41.76 降至 1.23，最短外侧节点段从 0.00113 mm 提高到至少 0.03835 mm；内侧最短段由约 0.000503 mm 提高到至少 0.02082 mm。可见主要收益是移除近邻节点叠加造成的参数挤压，不是把网格强制等距或变成方形。人工接缝三维路径最大改动 {max(s['max_same_parameter_movement_mm'] for s in at['artificial_seam_movement']):.6f} mm，XY 仍保持在数值精度内，详见 attribution_audit.json。

剩余外围数量大，主要仍来自原外边复合参数表达和张量积传播。这里没有证明这些数量是原物理形状的最小自由度；只证明本轮这 16 个传播节点可消除而不损失关键质量。

**4. 工程与 CAD 检查分层。** 26 条外边的次数、节点、重数、权重、控制点、参数范围、顶点位置与原边容差逐项比对保持；一对一边界匹配为 26/26。裁剪域检查确认原始 49 面和最终 5 面均为完整矩形等参边界，因而本轮支撑面投影就在实际 Face 域内。15836 个原始三角形的 PCA 平面投影没有翻转或零面积；这不是运行某个网格 UV 优化器的结果。所有支撑面的各节点单元 XY Jacobian 浮点 Bernstein 下界均为正，Face 方向与原参考一致，局部 BRepCheck 通过。该多项式界没有采用区间算术，不写成严格数值证明；也没有用 Jacobian 代替物理尺度分布诊断。

固定 0.001 mm、禁止局部公差累积的整件缝合与 STEP 回读得到 **2695 Face、1 个有效 Solid、0 自由边、0 多重边**。回读后接口仍是 26 条，双侧曲线落面与 SameParameter 检查通过，边容差仍为 1e-7 mm。实际最大落面残差见 integrated_boundary_audit.json，约 1e-11 mm。局部五面片自身是开放区域，不能将它的 26 条外边当作整件漏缝。

同模板、同整件尺度下原始模型 GEOM_CHECKS=0、直接对照=1、最终候选=1；原始 SHORT_EDGES=1501、候选=1492，整件报告的绝对 Accuracy 均为 0.004240749674。原模型因既有短边等项也不是 ModelCHECK 全绿。所有原始 XML、HTML、PRT 和关联文件保留，入口为 cad_summary.json。[PTC GEOM_CHECKS 说明](https://support.ptc.com/help/creo/creo_pma/r11.0/usascii/model_analysis/modelcheck/geomchk.html)本身不提供唯一缺陷类型，因此不把 Feature 4 当作具体曲面编号。

实际拆面定位：四张外围面分别报 GEOM_CHECKS=1，中央面=0。仅在两个 C0 节点分段仍报错；失败外围面的完整支撑、以及不写 pcurve 的 STEP 也仍报错。进一步把第 2 张外围面沿已有 C1 复合拼接处分成四个 C2 支撑块，四块均为 0；其中一块等几何插节点至 84×10 控制点仍为 0，插入冗余二重节点、保持真实光顺性也为 0。这排除了“仅控制点数多”或“仅名义重节点”对该面的解释，支持重点检查复合拼接处的实际二阶连续性/表示。20 个 C1 节点的真实物理高度 Hessian 跳变已另测，见 c1_knot_curvature_audit.json。**这仍不是 Creo 返回了具体曲率错误代码，也没有把分段探针作为新的低面数交付。** 开放块的模型尺度和自动绝对 Accuracy 与整件不同，定位证据不替代整件验收。

**5. 产物与复现。**

- [最终局部面片](aligned_inner_cubic/patch_faces.step)、[五张支撑面](aligned_inner_cubic/support.step)、[整件诊断 STEP](aligned_inner_cubic/whole_model_diagnostic.step)。
- [整体及两个局部的同视角真实几何图](figures/geometry_comparison.png)、[独立样本法向差对照图](figures/normal_error_comparison.png)。图像来自实际 STEP 或原始几何样本，无生成式示意。
- [机器可读汇总](run.json)、[全部成功与失败比较](comparison.csv)、[归因检查](attribution_audit.json)、[原边和映射检查](geometry_validation.json)、[原始 CAD 报告索引](cad_summary.json)。
- `*_initial_solver`、`*_initial_center` 是被修正实现的保留证据，不能当成当前候选；目录迁移原因保存在 relocation.json。其他失败曲面和日志也保留。本轮没有清理既有归档。

复现实验使用新目录，脚本遇到已有输出会拒绝覆盖。原始整件不假定包含在远程归档中，需要提供真实原模型。以下入口复现最终六配置、两个验证配置和整件验证；本轮各条底层命令均已实际执行，包装入口本身仅完成语法与输入路径检查，没有再重复整轮。CAD 分段/表示探针可分别运行 continuity_split_probe.py、continuity_transfer_probe.py、continuity_span_probe.py、continuity_multiplicity_probe.py，再调用 run_creo.ps1 检查对应 STEP；其实际输入与版本写在各 probe 的 protocol.json 中。

```powershell
./tools/candidate179_research/reproduce_continuity.ps1 `
  -Python 'D:/pyProject/3d_engineering/astra_modeling_test/.venv/Scripts/python.exe' `
  -SourceStep 'D:/pyProject/step-patch-optimizer/data/stp/03_配件_Clay.stp' `
  -OutputDir 'data/runs/candidate179_continuity_repro' `
  -CreoRoot 'E:/Proe/Creo 11.0.0.0'
```

关键代码为 continuity_fit.py（固定起点、共面等式、显式 XY、节点兼容）、continuity_validate.py（原边/域/映射及独立采样）、continuity_local.py（最坏区域）、各 CAD probe 和 continuity_report.py。现有 designer_layout.solve 只抽出装配函数，原默认方程不改；hard_boundary_audit 修正局部样本不覆盖某张 Face 时空数组崩溃，未删去任何评估点；diagnose 只增加可选反向采样种子/数量，默认协议保持。

**6. 下一步决定。** 继续原生路线，保留原边复用、共享人工边变量和整件验收链；采用显式外围 XY 映射及人工内边节点兼容处理。当前证据不支持继续在原映射上硬压 G1，也不支持为了缩点而统一放大中央面。下一轮应优先处理复合边界拼接向面内传播的二阶连续性，并以剩余外边窄带的法向误差和 Creo 分段证据共同验收；先解决这层限制，再决定是否需要局部可裁剪支撑或布局改变。尚不能将全部 Creo 错误归为同一个已证实原因，也不承诺五面、四面或某种表示必然可行。
'''
    (out/'RUN.md').write_text(note,encoding='utf-8')
    print(out/'RUN.md',flush=True)


if __name__=='__main__':main()
