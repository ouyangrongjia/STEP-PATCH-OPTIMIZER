# 局部距离预算的自动检测与修复

统一入口已自动完成检测、局部修复、重新装配和新采样验证。该样例无需指定异常面或区域编号，自动选中并修复 4 张支撑面；最终 985 张曲面的几何系数、区域归属、2681 条共享曲线和 102 条主曲线，与 native-v7 人工补救后的结果完全一致。本轮交付的是自动化能力，没有新增可见光顺改善的声明。

入口保持最简结构：[整件 STEP](model.step)、[359 份开放分区件](regions/)、[Creo 原生件](creo/model.prt.1)、[摘要](run.json)及 `metrics/`、`images/`。只有整件承载闭合实体证据。几何拟合只读原 STL，不调用 Geomagic API；给定分区仍来自此前 GUI 的 STEP 到 STL 映射。

## 自动流程与实测

输入为上一版人工距离修复前的完整曲线网和支撑缓存。首轮新种子 10200900 的逐面最大距离未超过 0.03 mm，但 95% 预防阈值选出了此前易超限的四个域。自动局部重拟合两张，兼容系数回退两张；共享曲线不变，最终预算仍为 0.03 mm，连续边界预算仍为 0.002 mm。原失败样本和人工修复历史继续保留于 native-v7，本轮不把首轮描述为已经抽到超限。

| 项目 | 本轮结果 |
|---|---:|
| 自动修复轮数 / 支撑面数 | 1 / 4 |
| STEP 面数 / 实体 / 自由边 | 985 / 1 / 0 |
| 新种子逐面样本数 | 1007870 |
| 逐面样本最大距离 / mm | 0.0282013428 |
| 面积样本最大距离 / mm | 0.0293275990 |
| 最少每面有效样本数 | 967 |
| Creo 闭合活动实体 / GEOM_CHECKS | 是 / 0 |
| 后端回归 | 107 项通过；最后局部变更另跑相关 4 项通过 |

最终逐面种子为 10200901。被用于选择和修复的首轮数据属于调参数据；修复后新种子的实际 STEP 抽样单独保存。面积抽样沿用固定种子作为回归检查，不冒充新的独立终验。见 [逐轮记录](metrics/distance_guard.json)、[局部候选](metrics/repair.json)、[逐面终验](metrics/stratified.json)、[精确系数对照](metrics/manual_equivalence.json)、[Creo 复核](metrics/creo_validation.json)。统一入口原始运行未启用可选 Creo 参数，因此其 `native_solid_verified=false` 保留；实际 Creo 复核独立记录，不回写修改运行历史。

测试覆盖真实局部鼓包自动修复、修复次数耗尽拒绝交付、每面保留异常位置、交换面次序变化后的身份核对、共享支撑组完整性及非有限误差拒绝。原预算、曲线和标签均有检查。全局法向、数字内弧的可见起伏、G1/G2 和其他模型泛化仍未通过，保持 `DiagnosticOnly/can_apply=false`。

整件两张 STEP 图来自 native-v7，因几何系数完全一致而复用，来源见 [图片记录](metrics/image_provenance.json)；Creo 图来自本次新导入。不将旧比较图冒充本次新的视觉改进。

## 复现

将 [重建包](metrics/reconstruction.zip) 解压到独立目录，设置 `PYTHONPATH` 为其中的 `source_snapshot`，运行 `restore.py --stl <原始STL>` 验证源坐标和三角形顺序并重绑定路径，然后运行：

```powershell
python -m tools.mesh_brep.skeleton_reconstruct `
  --stl <原始STL> --partition <解压目录>/source_partition.npz `
  --surface-fit <解压目录>/input_fit --prior-fit <解压目录>/prior_fit `
  --reference-faces 2739 --distance-seed 10200900 `
  --output <不存在的新目录> --no-render
```

可加 `--creo-root` 和 `--native-probe` 让同一入口继续执行 Creo 复核。`fit/` 保存本次最终精确系数，可用 `--surface-fit` 检查；`input_fit/` 保存修复前输入，`prior_fit/` 保存已有稀疏回退候选。缺少兼容旧系数时流程自动改为局部重拟合。缓存恢复后不再调用旧骨架生成器。单元测试通过不代表所有曲面或所有模型均可成功修复；次数耗尽、几何身份不明或局部求解失败会保留失败记录并停止发布。
