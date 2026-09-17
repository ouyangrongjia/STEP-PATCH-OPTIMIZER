# STEP 证据说明

## 最新优化结果

最新参数布局运行 `20260729_131649_203` 没有进入正式拟合：

- `full_fit_count=0`
- `NoSewableGeometry`
- 没有新支撑曲面
- 没有新替换 Face
- 没有新 B-Rep
- 没有可缝合的新曲面 STEP

因此本目录不能提供一个名为“最新优化曲面”的 STEP。若生成这种文件，只能是把旧诊断几何改名冒充新结果。

最新曲线网络的精确 JSON 保存在：

- `curve_network.json`
- `evidence/latest_layout_20260729_131649/curve_network.json`

最新运行原始 `curve_network.step` 的生成时哈希记录在 `latest_layout_snapshot.json` 中，但原始运行目录在补充本证据包时已不在工作树内，因此没有用其他运行的 STEP 替换它。

## 可实际打开的历史 STEP

用户指定的完整诊断运行 `20260729_032156_431` 已整体复制到：

`evidence/previous_full_diagnostic_20260729_032156/`

其中：

| 文件 | 内容 | 审阅边界 |
|---|---|---|
| `curve_network.step` | 当时的曲线网络 | 历史曲线网络，不是最新序列化文件 |
| `untrimmed_support_surfaces.step` | 4 张未裁剪诊断支撑曲面 | 旧诊断曲面 |
| `trimmed_patch_faces.step` | 4 张裁剪替换 Face | 旧诊断 Face |
| `diagnostic_sewn.step` | 缝合后的完整诊断实体 | 旧诊断整件；不是最新优化结果 |

该运行实际有 116 个翻转或退化 `surfaceParameters` 三角形，虽然固定 `0.001 mm` 缝合和 STEP 往返得到 1 个 Solid、0 条自由边，但几何门和 Creo ModelCHECK 没有通过，`can_apply=false`。

另一个可缝合基线 `20260728_205420_144` 完整保存在：

`evidence/sewable_baseline_20260728_205420/`

它同样只是历史诊断证据。

## 推荐审阅顺序

1. 先看 `01_overview_comparison.png` 和 `03_layout_gating_matrix.png`，确认最新运行的阶段边界。
2. 打开 `evidence/previous_full_diagnostic_20260729_032156/trimmed_patch_faces.step` 查看旧的 4 张替换 Face。
3. 打开同目录 `diagnostic_sewn.step` 查看旧诊断整件。
4. 同时核对该目录的 `validation_summary.json`、`artifact_status.json` 和 `evidence_manifest.json`，不要只凭 STEP 显示效果判断成功。

## 复制后 STEP 读取验证

`build/windows-msvc-debug/Debug/step_stats.exe` 已对 `20260729_032156` 的四个复制文件重新读取：

| 文件 | Face | Edge | Shell | Solid | BRepCheck |
|---|---:|---:|---:|---:|---|
| `curve_network.step` | 0 | 33 | 0 | 0 | 有效 |
| `untrimmed_support_surfaces.step` | 4 | 16 | 4 | 0 | 有效 |
| `trimmed_patch_faces.step` | 4 | 38 | 1 | 0 | 有效 |
| `diagnostic_sewn.step` | 2694 | 10800 | 1 | 1 | 有效 |

机器可读结果位于 `evidence/previous_full_diagnostic_20260729_032156/packaging_step_validation.json`。
