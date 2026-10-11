# 版本与结果目录

结果目录与外层 `native-vN` 使用相同编号。整件入口为各版 `model.step`，开发说明及证据入口为 `RUN.md`，机器摘要为 `run.json`。

| 版本 | 当前结果目录 | 整件与说明 |
|---|---|---|
| v2 | `native-v2/result_v2` | [整件 STEP](native-v2/result_v2/model.step) · [运行说明](native-v2/result_v2/RUN.md) · [交付摘要](native-v2/result_v2/run.json) |
| v3 | `native-v3/result_v3` | [整件 STEP](native-v3/result_v3/model.step) · [运行说明](native-v3/result_v3/RUN.md) · [交付摘要](native-v3/result_v3/run.json) |
| v4 | `native-v4/result_v4` | [整件 STEP](native-v4/result_v4/model.step) · [运行说明](native-v4/result_v4/RUN.md) · [交付摘要](native-v4/result_v4/run.json) |
| v5 | `native-v5/result_v5` | [整件 STEP](native-v5/result_v5/model.step) · [运行说明](native-v5/result_v5/RUN.md) · [交付摘要](native-v5/result_v5/run.json) |
| v6 | `native-v6/result_v6` | [整件 STEP](native-v6/result_v6/model.step) · [运行说明](native-v6/result_v6/RUN.md) · [交付摘要](native-v6/result_v6/run.json) |
| v7 | `native-v7/result_v7` | [整件 STEP](native-v7/result_v7/model.step) · [运行说明](native-v7/result_v7/RUN.md) · [交付摘要](native-v7/result_v7/run.json) |
| v8 | `native-v8/result_v8` | [整件 STEP](native-v8/result_v8/model.step) · [运行说明](native-v8/result_v8/RUN.md) · [交付摘要](native-v8/result_v8/run.json) |
| v9（基于 v8 的边界优化） | `native-v9/result_v9` | [整件 STEP](native-v9/result_v9/model.step) · [运行说明](native-v9/result_v9/RUN.md) · [实际状态与限制](native-v9/result_v9/run.json) |
| v10（过渡光顺与特征边约束） | `native-v10/result_v10` | [整件 STEP](native-v10/result_v10/model.step) · [运行说明](native-v10/result_v10/RUN.md) · [实际状态与限制](native-v10/result_v10/run.json) |

## 目录编号调整

本次仅统一目录编号并修正当前入口引用，没有重新拟合模型、改写测量值或重跑 CAD 验收。历史版本及原始材料保留。

| 外层目录 | 原结果目录 | 当前结果目录 |
|---|---|---|
| `native-v2` | `result_v3` | `result_v2` |
| `native-v3` | `result_v4` | `result_v3` |
| `native-v4` | `result_v5` | `result_v4` |
| `native-v5` | `result_v6` | `result_v5` |
| `native-v6` | `result_v7` | `result_v6` |
| `native-v7` | `result_v8` | `result_v7` |
| `native-v8` | `result_v9` | `result_v8` |

`metrics/`、日志、Creo 原始报告及重建压缩包中的旧绝对路径和结果编号保留为运行来源，可按上表定位当前版本；已清理中间路径的状态不因改名而改变。当前可打开的产物位置以本索引及各版 `run.json` 为准。

各版 `regions/` 保存按原标签导出的开放曲面；闭合实体检查对象是整件 `model.step`。光顺性限制、距离与 Creo 检查以及 Apply 状态仍分别按各版记录判断。
