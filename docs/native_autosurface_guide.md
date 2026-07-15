# 原生局部 AutoSurface 实验归档

## 当前状态

`native-autosurface` 冻结为四边布局实验参考，不再继续实现 AS-3，也不接入 GUI 或 `AppController`。

分支关系：

```text
native-surface-refit 1076082
└─ native-autosurface
   └─ 额外三个 AS-0～AS-2 提交
```

两条路线不是完全独立历史，但当前运行路径隔离。AS 新增代码只被探针和测试使用；`native-surface-reduction` 不合并这三个完整提交。

## 冻结原因

AS-1 以从 STEP 采样得到的全局三角网络为拓扑、布局和参数化前提。真实 STEP 已证明该三角网络质量不足，继续实现 AS-3 只会把输入缺陷传给曲面网络拟合器，无法满足原始 B-rep 形态与拓扑质量门。

允许按需移植的内容仅限：

- 与原始面局部自适应采样有关、且不依赖全局三角拓扑的独立逻辑；
- 报告格式和诊断辅助代码；
- 不包含 QuadriFlow、多图参数化或全局三角邻接假设的测试工具。

当前替代路线见 `docs/surface_reduction_guide.md`。
