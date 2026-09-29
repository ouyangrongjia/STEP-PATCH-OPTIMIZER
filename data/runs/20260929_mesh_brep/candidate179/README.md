# candidate-179：只看这四个 STEP

| 路线 | 局部面片 | 缝合回原模型的整件 |
|---|---|---|
| A，原 CAD 辅助网格参数化 | [A_local.step](A_local.step) | [A_sewn.step](A_sewn.step) |
| B，网格形状重建后适配原接口 | [B_local.step](B_local.step) | [B_sewn.step](B_sewn.step) |

**本次修正请打开 B_sewn.step。** B 已重新拟合，保留原区域全部 26 条边界并重新缝合；成排褶皱明显降低。查看 [修正前后真实曲面及线框](wrinkle_comparison.png)。旧的有褶皱版本已从这个交付目录替换。

A、B 整件均已在 Creo 导入，有非零体积；OCCT 与 STEP 回读均为 1 个有效 Solid、零自由边。局部 STEP 各含 5 张面，本身保留外边界。B 仍有接缝邻域的残余光顺问题，Creo GEOM_CHECKS、短边及单方向控制点预算仍未通过；保持 `DiagnosticOnly/can_apply=false`。

A 增加网格 UV 来替代逐点参数反求，拟合目标仍来自原 CAD。B 的原始后端只读 STL 与给定分区；为按此次要求拼回旧模型，随后增加了 **CAD 辅助的精确外边界约束、光顺重拟合和整件缝合**，所以本目录 B 整件不能称为完全不依赖 CAD 的重建。

精确数值、耗时、失败版本及复跑命令见 [本次修正记录](../candidate179_assembly/RUN.md)。此前无关夹具不属于当前交付；自动审批阻止了物理删除，清理范围和脚本见该记录。
