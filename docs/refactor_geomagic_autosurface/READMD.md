当前重构阶段，Codex 先阅读本目录下文档；
不要直接覆盖根 docs 下旧文档；
旧文档作为历史架构与现有实现参考；
真实代码修改以 02_refactor_plan.md 和 03_todo.md 为准。

# STEP-PATCH-OPTIMIZER 重构指导文档索引

包含 5 份核心 Markdown 文档：

| 文件                      | 用途                                                        |
| ------------------------- | ----------------------------------------------------------- |
| `00_PROJECT_STRUCTURE.md` | 项目结构文档：当前结构、推荐新增目录、CMake 修改边界。      |
| `01_MODULE_GUIDE.md`      | 项目模块文档：各模块职责、执行标准、测试基准。              |
| `02_REFACTOR_PLAN.md`     | 重构方案文档：指导 Codex 分阶段重构。                       |
| `03_TODO.md`              | TODO 文档：当前阶段任务、子任务和验收标准。                 |
| `04_PROJECT_WORKFLOW.md`  | 项目流程文档：STP+STL 输入到 patch 生成、预览、替换和验证。 |

推荐阅读顺序：

```text
1. 02_REFACTOR_PLAN.md
2. 04_PROJECT_WORKFLOW.md
3. 01_MODULE_GUIDE.md
4. 03_TODO.md
5. 00_PROJECT_STRUCTURE.md
```

当前推荐先做：

```text
MVP-1：local STL 裁剪 + Geomagic AutoSurface patch generation + Viewer overlay preview。
```

暂时不要直接做大规模真实贴回。