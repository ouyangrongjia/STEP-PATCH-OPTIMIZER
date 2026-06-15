# Codex Instructions for step-patch-optimizer

本文件适用于 `D:\pyProject\step-patch-optimizer` 及其所有子目录。

## 必须先读的全局文档

在执行任何代码修改、调试、文档整理、规划、复盘或状态同步之前，必须先阅读：

1. `D:\Desktop\WorkSpace\00_Global\00_WORKBENCH.md`
2. `D:\Desktop\WorkSpace\00_Global\logs\01_COMPOUND_LOG.md`
3. `D:\Desktop\WorkSpace\00_Global\logs\02_PITFALL_LOG.md`
4. `D:\Desktop\WorkSpace\00_Global\sop\03_GLOBAL_SOP.md`

## 必须先读的项目记忆文档

然后阅读：

1. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\00_PROJECT_OVERVIEW.md`
2. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\01_REQUIREMENTS.md`
3. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\02_TECH_STACK.md`
4. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\03_DEV_LOG.md`
5. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\04_PITFALL_LOG.md`
6. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\05_PROJECT_SOP.md`
7. `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\06_REVIEW.md`

## 必须先读的仓库文档

最后按顺序阅读仓库内文档：

1. `README.md`
2. `docs/module_design.md`
3. `docs/implementation_status.md`
4. `docs/run_gui.md`
5. `docs/project_structure.md`
6. `docs/TODO.md`

如果任务涉及 Geomagic AutoSurface / patch preview / Apply 主线，还必须阅读：

1. `docs/refactor_geomagic_autosurface/03_TODO.md`
2. `docs/refactor_geomagic_autosurface/04_PROJECT_WORKFLOW.md`

阅读完成后，必须先向用户说明已阅读哪些文档，再开始执行。

## 工作原则

- 以真实仓库 `D:\pyProject\step-patch-optimizer` 为准，不要把临时工作区当成主项目。
- 当前仓库可能已有用户未提交修改。不要回滚、覆盖或清理未明确属于自己的改动。
- 优先使用项目脚本：`.\scripts\build_debug.ps1`、`.\scripts\test.ps1`、`.\scripts\run_gui.ps1`。
- 底层验证优先使用 CMake preset：`windows-msvc-debug`。
- 修改任务状态时，同步 `docs/TODO.md` 和 `docs/implementation_status.md` 的具体子项，不要笼统标记完成。
- 发现跨项目可复用规则，写入 `D:\Desktop\WorkSpace\00_Global\` 下的对应文档。
- 发现本项目专属经验，写入 `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\` 下的对应文档。
