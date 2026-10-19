# tbcad::cmd — command framework, undo/redo, aliases

Library `libs/cmd` (`tbcad::cmd`), registration glue in `cmake/command_modules.cmake`. Depends on
`tbcad::core` only. Everything is single threaded.

## Writing a command

```cpp
class LineCmd final : public tbcad::cmd::Command {
public:
    std::string_view name() const override { return "LINE"; }            // upper-case ASCII
    std::vector<std::string> defaultAliases() const override { return {"L"}; }
    Task run(CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("Specify first point");
        if (!a.ok()) co_return;                       // Esc or Enter
        auto b = co_await ctx.getPoint("Specify next point",
                                       PointOptions{.base = a.point, .keywords = {"Undo"},
                                                    .preview = [&](Vec2 c, DisplayList& out) { ... }});
        if (b.ok()) ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
    }
};
```

* Command objects live in the `Registry` for the whole session and are shared by every run. Keep
  run state in locals of `run()`, not in members.
* Prompts: `getPoint`, `getDistance`, `getAngle` (radians), `getReal`, `getInteger`, `getString`,
  `getKeyword`, `getSelection`, `getEntity`. Each result has `status`:
  `Ok` (value), `Cancel` (Esc), `Keyword` (one of the prompt's keywords, see `keyword`),
  `None` (Enter without a value and no default). Keywords match case-insensitively by exact name
  or unique prefix.
* Helper coroutines: `Task helper(CommandContext&)` and `co_await helper(ctx);` (symmetric transfer).
* Avoid `co_await` inside `catch`, and avoid coroutine lambdas that capture by reference (MSVC).
* After an Esc every later prompt in the same command answers `Cancel` at once, so a loop that
  ignores the status still ends.

## Transactions and undo

* `ctx.tx()` is the command's `Transaction`, opened on first use. When `run()` returns normally it is
  committed and the result becomes **one** undo step labelled with the command name
  (`ctx.setUndoLabel("Move 3 objects")` to change it).
* **Esc discards everything the command did.** LINE-style commands that keep finished segments call
  `ctx.commitOnCancel()` first. An exception thrown by `run()` rolls back and is reported on the
  command line.
* `ctx.checkpoint()` commits the work so far and remembers that state; `ctx.rollbackToCheckpoint()`
  returns to the most recent remembered state and forgets it (call `checkpoint()` before each
  segment, `rollbackToCheckpoint()` for the Undo option). Observers see each step; at the end all
  steps are still one undo entry.
* `UndoStack`: unlimited count, memory bound (default 512 MB estimated from snapshot sizes, oldest
  steps dropped with a message through `setMessageSink`); `undo(doc)`, `redo(doc)`, `canUndo`,
  `undoLabel()`/`redoLabel()`, `beginGroup/endGroup` (commands `UNDOBEGIN`/`UNDOEND`). The stack
  must be the only way undo/redo is applied to the document.
* Built-in commands from `registerBuiltinCommands`: `UNDO` [U], `REDO`, `UNDOBEGIN`, `UNDOEND`,
  `ALIASES`.

## Running commands: CommandRunner

The host (the app, or `ScriptHost`) owns a `CommandRunner(doc, undoStack, host, registry)` and feeds
it `Input` events: `Input::typed("0,0")`, `enter()`, `cancel()` (Esc), `click(RawInput)`,
`pick(entityId)`, `select(ids)`.

* Idle: typed text is a command name or alias; Enter repeats the last command.
* Running: input answers the active prompt. Bad input is refused with a message and the prompt
  stays (`SubmitResult::Rejected`).
* `pointerMoved(RawInput)` runs the point filters and the prompt's `preview`, and calls
  `host.setPreview(...)` / `host.feedbackChanged(...)`. `host.promptChanged(...)` tells the command
  line what to show (`PromptInfo::display()` gives `Specify next point or [Close/Undo]: `).
* Transparent commands (`isTransparent()` is true, e.g. ZOOM): typed with a leading apostrophe
  (`'ZOOM`) or started with `startTransparent()` while another command waits for input. They share
  the parent's transaction, so their document edits belong to the parent's undo step.
* Typed points: `x,y`, `@dx,dy` (relative to the last point entered), `@d<a`, `d<a` (polar from
  the origin), `#x,y`. Lengths accept units (`1m`, `3in`); angles are in degrees by default.
  Drafting aids extend this by installing a `CommandLineParser` with `runner.setLineParser(...)`
  (asked first; return `nullopt` to fall back to the built-in grammar).
* Point modifiers at any point prompt: `FROM` (base point, then an offset such as `@10,0`), `M2P`
  / `MTP` (midpoint of two points), `TT` (temporary tracking point; its horizontal and vertical
  rays are tracked until the next click). A snap name typed at a point prompt (`END`, `MID`,
  `INT`, `APP`, `CEN`, `QUA`, `PER`, `TAN`, `NEA`, `NOD`, `INS`, `EXT`, `PAR`, `NON`) overrides the
  running snaps for one pick (`CommandLineParser::snapOverride`, stored in `drafting.osnap.once`).
* Point filters: implement `PointFilter`, add to a `PointFilterChain` with an `order`, and hand the
  chain to `runner.setFilterChain(...)`. The filter with the lowest `order` that returns a point
  wins (osnap before polar/ortho before grid), and its `FilterFeedback` goes to the overlay.
* Noun-verb: `getSelection` returns the host's current selection immediately when it is non-empty
  (`SelectionOptions::allowPreselection`); otherwise picks accumulate until Enter.

## Commit hooks and the window query

* `Registry::addCommitHook(fn(Document&, Transaction&))`: runs just before a command's open
  transaction is committed (every commit, including checkpoints; never for a cancelled or failed
  command). Edits made through `tx` join the command's undo step. Use `tx.touchedEntities()` (the
  pre-existing entities modified or erased so far) to find out what changed, and return at once when
  nothing relevant was touched. Associative hatches use it to follow their boundary.
* `IHost::entitiesInWindow(box)` returns the model-space ids whose box meets the window, from the
  host's spatial index, or `nullopt` when the host has none (callers scan the document then).
  `ScriptHost::setWindowQuery` fakes it in tests.

## Aliases

Built-in aliases come from `defaultAliases()`. The user file `aliases.json` in the settings folder
(`{"L": "LINE", "CC": "CIRCLE"}`) is applied over them by `Registry::loadUserAliases(path)`; the
`ALIASES` command reloads it. Problems never abort: the returned `AliasReport` lists errors
(broken JSON, unknown target, alias equal to a command name) and `Registry::conflicts()` lists
built-in aliases claimed by two commands (neither gets it until a user alias decides). A broken
file means the built-in defaults stay in force. Show the report at startup.

## Command modules (registration without a central list)

Each module is a folder `libs/commands/<m>/` with a `CMakeLists.txt` (the library, target
`tbcad_<m>`) and a `module.cmake`:

```cmake
tbcad_command_module(NAME draw
                     REGISTER tbcad::draw::registerCommands        # void(tbcad::cmd::Registry&)
                     UI_REGISTER tbcad::draw::registerUi)          # optional, void(tbcad::ui::UiRegistry&)
```

CMake collects all `module.cmake` files and generates `register_all.cpp/.h` plus the static library
`tbcad_command_registry` (alias `tbcad::command_registry`), which links `tbcad::cmd` and every
module. The app links it and calls `tbcad::registerAllCommands(registry)` (built-ins plus every
module) and `tbcad::registerAllUi(uiRegistry)` from `<tbcad/cmd/register_all.h>`. The UI registry
class name is the cache variable `TBCAD_UI_REGISTRY_TYPE` (default `tbcad::ui::UiRegistry`).
`libs/commands/_example/` is the template (see its README); it is registered while
`TBCAD_REGISTER_EXAMPLE_COMMANDS` is ON (default) and adds one demo command `EXAMPLELINE`.

## Testing commands: ScriptHost

```cpp
Document doc; Registry reg; registerBuiltinCommands(reg); reg.add(std::make_unique<LineCmd>());
ScriptHost host(doc, reg);
auto r = host.run("LINE", {"0,0", "@100,0", ScriptItem::esc()});   // r.finished, r.rejected ...
host.undo(); host.redo();
host.log(); host.prompts(); host.preview(); host.setSelection({...});
```

Items are text (`""` = Enter), `ScriptItem::esc()`, `enter()`, `click(Vec2)`, `pick(id)`,
`select(ids)`. `run` stops feeding when the command finishes; `feed` continues a command that is
still waiting.
