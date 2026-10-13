# Levels (Q70)

Command module `levels`. Design: `/mnt/project-files/build/levels-brief.md`; user and module notes:
`docs/levels.md`. Registers through `module.cmake`.

Core pieces live outside this folder because every layer of the program needs them: the `Level`
table, `EntityCommon::level`, `Header::currentLevel` / `levelView`, `Document::effective(EntityCommon)`
(libs/core), the `LVLS` / `ELVL` chunks (libs/io), the pick / scene / snap hooks (libs/ui, libs/drafting),
the XDATA round trip (libs/dxf) and the MCP tools (libs/mcp). This module holds the headless
operations (`level_ops.h`), the commands `LEVEL`, `-LEVEL`, `CHLEVEL`, `LEVELUP`, `LEVELDOWN` and the
Levels panel. The status bar switcher is in apps/tbcad (`Gui::statusBar`).
