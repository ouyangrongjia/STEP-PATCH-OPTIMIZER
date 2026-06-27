# Codex Instructions for step-patch-optimizer

本文件适用于 `D:\pyProject\step-patch-optimizer` 及其所有子目录。

## 全局 Workspace 文档规则

本项目不再强制读取 `D:\Desktop\WorkSpace\00_Global\` 下的全局工作文档。除非用户明确要求，不要在本项目任务开始时读取这些全局文档；这些文件也可能已经被删除。

## 项目记忆文档规则

本项目不再要求在任务开始时读取 `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\` 下的项目记忆文档。

除非用户明确要求，不要把以下文件作为启动依赖读取：

1. `00_PROJECT_OVERVIEW.md`
2. `01_REQUIREMENTS.md`
3. `02_TECH_STACK.md`
4. `05_PROJECT_SOP.md`
5. `03_DEV_LOG.md`
6. `04_PITFALL_LOG.md`
7. `06_REVIEW.md`

后续以仓库内文档作为当前事实来源。

## 必须先读的仓库文档

最后按顺序阅读仓库内文档：

1. `README.md`
2. `docs/module_design.md`
3. `docs/implementation_status.md`
4. `docs/completed_features.md`
5. `docs/run_gui.md`
6. `docs/project_structure.md`
7. `docs/TODO.md`

如果任务涉及 Geomagic AutoSurface / patch preview / Apply 主线，还必须阅读：

1. `docs/geomagic_patch_workflow.md`
2. `docs/geomagic_patch_cleanup_plan.md`

阅读完成后，必须先向用户说明已阅读哪些文档，再开始执行。

## 工作原则

- 以真实仓库 `D:\pyProject\step-patch-optimizer` 为准，不要把临时工作区当成主项目。
- 当前仓库可能已有用户未提交修改。不要回滚、覆盖或清理未明确属于自己的改动。
- 优先使用项目脚本：`.\scripts\build_debug.ps1`、`.\scripts\test.ps1`、`.\scripts\run_gui.ps1`。
- 底层验证优先使用 CMake preset：`windows-msvc-debug`。
- 修改任务状态时，同步 `docs/TODO.md` 和 `docs/implementation_status.md` 的具体子项，不要笼统标记完成。
- 本项目后续只维护项目记忆文档和仓库文档；不要再把经验写入全局 Workspace 文档，除非用户另行明确要求。
- 发现本项目专属经验，写入 `D:\Desktop\WorkSpace\10_Projects\step-patch-optimizer\` 下的对应文档。

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **STEP-PATCH-OPTIMIZER** (5495 symbols, 16912 relationships, 300 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> Index stale? Run `node .gitnexus/run.cjs analyze` from the project root — it auto-selects an available runner. No `.gitnexus/run.cjs` yet? `npx gitnexus analyze` (npm 11 crash → `npm i -g gitnexus`; #1939).

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows. For regression review, compare against the default branch: `detect_changes({scope: "compare", base_ref: "feature-bounded-refit"})`.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `query({search_query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `context({name: "symbolName"})`.
- For security review, `explain({target: "fileOrSymbol"})` lists taint findings (source→sink flows; needs `analyze --pdg`).

## Never Do

- NEVER edit a function, class, or method without first running `impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `rename` which understands the call graph.
- NEVER commit changes without running `detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/STEP-PATCH-OPTIMIZER/context` | Codebase overview, check index freshness |
| `gitnexus://repo/STEP-PATCH-OPTIMIZER/clusters` | All functional areas |
| `gitnexus://repo/STEP-PATCH-OPTIMIZER/processes` | All execution flows |
| `gitnexus://repo/STEP-PATCH-OPTIMIZER/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
