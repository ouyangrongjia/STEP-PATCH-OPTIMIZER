# candidate-179 Geomagic 逆向优化可视化

## 结论

最新运行只证明参数布局子目标改善：实际 `surfaceParameters` 的翻转和退化由此前完整诊断的 116 降为 0，畸变 99.5% 分位为 `69.0536461`，但最大畸变仍为 `201.442103 > 100`，参数化产品门未通过。该次运行 `full_fit_count=0`，状态为 `NoSewableGeometry`，没有生成新支撑曲面、替换 Face、B-Rep、缝合结果或可回读 STEP。

旧的 `20260728_205420_144` 基线只用于说明历史上存在可缝合诊断几何：1 次完整拟合、4 张替换 Face、固定 `0.001 mm` 缝合后 STEP 往返得到 1 个 Solid、0 条自由边，Creo 导入为 `OK/PASS`，但 ModelCHECK 未通过，且 `can_apply=false`。这些旧几何没有被标记或渲染为本次新结果。

## 文件

- `01_overview_comparison.png`：高分辨率总览，对比旧的可缝合诊断基线与最新参数布局运行。
- `02_latest_curve_network.png`：直接从最新 `curve_network.json` 的 `edge.samples` 投影；蓝色为 28 条外边界曲线，橙色为 5 条内部 `RegularizedCubic`，圆点为 30 个节点。
- `03_layout_gating_matrix.png`：严格区分“已通过”“改善但未通过”“未执行 / 不可评价”，并说明未执行阶段的默认 0 值不是质量证据。
- `render_visualizations.js`：可复现渲染脚本。
- `visualization_manifest.json`：输入、指标来源、生成命令、文件哈希与限制。
- `STEP_README.md`：当前结果与历史 STEP 的来源、用途和审阅边界。
- `artifact_status.json`、`run.json`、`evidence_manifest.json`：运行式证据包入口。
- `evidence/latest_layout_20260729_131649/`：最新布局审计快照和按原哈希保留的曲线网络 JSON。
- `evidence/previous_full_diagnostic_20260729_032156/`：用户指定的完整旧诊断运行，包含三个曲面／实体 STEP 及全部 JSON 证据。
- `evidence/sewable_baseline_20260728_205420/`：此前可缝合基线的完整 final 证据。

没有增加旧基线 STEP 几何截图：当前任务环境没有经核验的无头 STEP 渲染器。实际 STEP 已按来源完整收录，但没有被渲染或冒充为本次新曲面。

## 分阶段状态

| 阶段 | 最新运行 | 旧基线上下文 |
|---|---|---|
| 曲线网络 | 通过；30 nodes、33 curves、5 条内部曲线，投影与交叉门通过 | 同一网络门通过 |
| 参数布局 | 零翻转、零退化；p99.5 改善；最大畸变仍超门，整体未通过 | 仅作历史诊断上下文 |
| 拟合几何 | 未执行；`full_fit_count=0`，无新支撑曲面 | 1 次完整拟合，4 张诊断替换 Face |
| B-Rep / 拓扑 | 未执行；无可构造几何 | 诊断 B-Rep 可构造，但几何产品门未通过 |
| 缝合 | 未执行；`NoSewableGeometry` | 固定 `0.001 mm` 诊断缝合成功 |
| STEP 回读 | 未执行；没有本次新 STEP | 往返为 1 Solid、0 自由边、有效 B-Rep |
| Creo | 未尝试 | 导入 `OK/PASS`，ModelCHECK 未通过 |
| 产品 / Apply | `product_validated=false`，`can_apply=false` | `product_validated=false`，`can_apply=false` |

## 复现命令

在 `D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit` 中执行：

```powershell
$node = 'C:\Users\27836\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin\node.exe'
$env:NODE_PATH = 'C:\Users\27836\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules'
& $node 'data\runs\20260729_candidate_0179_geomagic_reverse_optimization_visualization\final\render_visualizations.js' `
  --latest 'data\runs\20260729_131649_203__native-region-refit__03_配件_Clay__cand-0179__target\final' `
  --baseline 'data\runs\20260728_205420_144__native-region-refit__03_配件_Clay__cand-0179__target\final' `
  --spec 'D:\pyProject\step-patch-optimizer-worktrees\geomagic-autosurface-oracle-candidate-0179\data\runs\20260729_recovered_fitting_specification_v8\recovered_fitting_specification.json' `
  --previous-flips 116 `
  --latest-degenerate 0 `
  --output 'data\runs\20260729_candidate_0179_geomagic_reverse_optimization_visualization\final'
```

`116` 与“退化为 0”的来源是当前工作树 `docs/implementation_status.md` 中记录的此前完整诊断与最新参数域验证；最新 JSON 直接提供 `flipped_triangles=0`、`distortion_p995`、`maximum_distortion`、`full_fit_count=0` 和 `NoSewableGeometry`。

## 验证与限制

- 所有输入 JSON 均已解析校验。
- 三张 PNG 均使用本地图片查看工具按原始分辨率逐张检查，中文字体、图例、裁切和文字清晰度正常。
- 曲线图只表达真实曲线采样点的二维投影，不表达不存在的新支撑曲面。
- 逆向规格仍为 `ResearchPartial`，`behavior_equivalent_ready=false`；不能宣称已复现 Geomagic 的完整拟合行为。
- 水密拓扑、几何质量、Creo ModelCHECK 与产品 Apply 是独立验收层，不能互相替代。
