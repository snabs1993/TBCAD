# The application: `apps/tbcad`, `libs/ui`, `libs/platform` (W3-1)

`tbcad.exe` is a thin window around a headless application model. Almost everything the program
does lives in libraries that run (and are tested) without a window.

| Piece | Where | Needs a window? |
| --- | --- | --- |
| `tbcad::platform`: paths, JSON, settings, key bindings, native file dialogs | `libs/platform` | no |
| `tbcad::ui::UiRegistry`: how command modules add panels, menu items, status toggles | `libs/ui` (target `tbcad_ui`) | no |
| `tbcad::ui::AppCore`: document, undo, selection, viewport, files, command line, autosave | `libs/ui` (target `tbcad_appcore`) | no |
| GLFW + Dear ImGui frame, docking layout, viewport input, `--gate` | `apps/tbcad` | yes |

## `tbcad::platform`

- `defaultAppPaths()`: `%APPDATA%\TBCAD` for settings, key bindings and `aliases.json`,
  `%LOCALAPPDATA%\TBCAD\autosave` for the crash backup. On Linux `$XDG_CONFIG_HOME/tbcad` and
  `$XDG_DATA_HOME/tbcad`. `TBCAD_CONFIG_DIR` and `TBCAD_DATA_DIR` override both.
- `Settings`: one JSON object, dotted lower-case keys (`ui.theme`, `autosave.minutes`,
  `pick.aperturePx`, `drafting.snap` ...). Defaults live in code (`Settings::withDefaults`), the
  file stores changes, unknown keys survive a save, writes are atomic (temp file, then rename), a
  broken file is reported and the defaults stay.
- `Keybindings`: action id to chord (`"Ctrl+Shift+Z"`). Whoever owns an action declares its default
  chord in code (`declare`); `keybindings.json` overrides it, `""` unbinds. Problems (bad chord,
  unknown action, two actions on one chord) come back as warnings, never as a failed start.
- `Json`: small parser and writer used by the two files above.
- `showOpenDialog` / `showSaveDialog`: Windows `IFileDialog`. Elsewhere `nativeDialogsAvailable()`
  is false and the application shows its own ImGui file browser.

Windows headers are only included in `libs/platform/src/file_dialog_win32.cpp` and under `apps/`.

## `tbcad::ui::UiRegistry`

Command modules call these from their `registerUi(UiRegistry&)` function (declare it with
`UI_REGISTER` in `module.cmake`; the generated `registerAllUi` calls it once at start-up):

| Call | Meaning |
| --- | --- |
| `addPanel(id, title, drawFn, visibleByDefault)` | A dockable window. `drawFn(PanelContext&)` draws it with Dear ImGui; include `<imgui.h>` in your own module. |
| `addMenuItem("Draw/Circle/Center, Radius", "CIRCLE", "draw.circle", "C")` | A menu entry that runs a command line. The action id makes it bindable; the last argument is the default chord. A new top-level name becomes a new menu. |
| `addStatusToggle(id, label, get, set)` | A toggle in the status bar (SNAP, GRID ...). The built-in ones store `drafting.snap`, `view.grid`, `drafting.ortho`, `drafting.polar` and `drafting.osnap` in the settings; drafting aids read them through `IHost::setting`. |
| `addOverlayDrawer(fn)` | Draw into the per-frame overlay (grips, tracking lines). |
| `addPropertySection(kind, fn)` | A section of the Properties panel for one entity kind. |

`PanelContext` holds the document, host, selection, camera and a `runCommand` function. Link
`tbcad::ui`, not `tbcad::appcore`: the latter would make a cycle through the generated registry.

## `tbcad::ui::AppCore`

Owns the `Document`, `UndoStack`, `SpatialIndex` (kept current by `DocumentIndexer`), the render
`Scene` (kept current by `SceneSync`), the `CommandRunner`, settings and key bindings, and is the
`cmd::IHost` the commands talk to.

- **Files**: `newDocument`, `openFile`, `save`, `saveAs` (adds `.tbcad`, atomic with a `.bak`),
  recent files, `windowTitle()` (`name.tbcad* - TBCAD`), `modified()`.
- **Command line**: `submitText("LINE 0,0 100,0")`. The first word names a command (aliases work),
  the other words answer its prompts. Empty text repeats the last command or takes the default.
  `'ZOOM` inside a command starts a transparent command. `pressEscape()` cancels, then clears the
  selection.
- **Viewport**: `Camera2D` in framebuffer pixels. `zoomAtScreen(point, notches)` (1.2 per notch
  about the cursor), `panPixels`, `zoomExtents`, `zoomToBox`. Built-in transparent commands `ZOOM`
  (Extents/Window/In/Out or two corners) and `PAN`.
- **Pointer events**: `pointerMoved`, `click`, `dragSelect`. Idle: a click adds the nearest
  entity to the selection, Shift removes, a click on nothing clears; a drag to the right selects
  what lies completely inside (window), to the left what it touches (crossing). During a command
  the same events answer its prompt (point, pick or selection).
- **Selection rules**: `isSelectable` / `isVisible` use `Document::effective(layer)`, so a
  sub-layer follows its parents. Off or frozen layers are invisible and unselectable; locked
  layers are drawn faded and not selectable. The selection drops entities that disappear or become
  unselectable.
- **Scene**: `SceneSync` mirrors every commit, undo and redo into the scene (`geom::buildDisplay`),
  re-tessellates curves when the zoom changes by more than 4x, and redraws entities when their
  layer is locked or unlocked.
- **Autosave**: `tickAutosave` writes `recovery.tbcad` every `autosave.minutes` (default 10) while
  there are unsaved changes; a normal save, new, open or exit removes it. At the next start
  `pendingRecovery()` finds a leftover and the window offers to recover it.
- **Working area**: a commit that puts geometry outside the working area raises
  `outOfAreaNotice()`; the window shows it with "Zoom to" and "Undo" (`outOfAreaZoomTo`,
  `outOfAreaUndo`). Undo and redo by hand do not raise it again.
- **`DEVGEN LINES 1000000`**: hidden command (no description, not completed) that creates the
  reproducible random test drawing of the M1 gate in one undo step.

## The window (`apps/tbcad`)

Layout: menu bar, tool strip, docked panels (Command at the bottom, Properties at the right, plus
whatever modules register), status bar (cursor coordinates in the drawing's units, toggles,
current layer, object and selection counts, fps). The drawing is rendered into an offscreen target
that fills the central dock area. Layout is saved to `imgui.ini` in the settings folder. Light and
dark themes (View menu, saved in settings). Fonts: Lato for Latin and Sarabun for Thai, built into
the ImGui atlas at the monitor's DPI scale, rebuilt when the window moves to another monitor. The
Windows manifest declares per-monitor v2 DPI awareness, UTF-8 as the process code page and long
path support. On Windows the IME candidate window follows the command line's text cursor.

Typing anywhere goes to the command line; Space or Enter on an empty line repeats the last command
or finishes a prompt. Up/Down browse earlier lines, Tab completes a command name.

Help > About shows the MIT notice and the third-party credits, including the FreeType line
required by its license, and opens `THIRD_PARTY_NOTICES.txt` (copied next to the program).

### Command line options

```
tbcad [file.tbcad] [--script "CMD;CMD"] [--frames N] [--screenshot out.ppm]
      [--gate [--report out.txt]] [--self-test] [--no-vsync]
```

`--self-test` opens a hidden window, runs the script, draws the frames and checks that something
was drawn; it exits with 77 (skipped by ctest) when there is no display or OpenGL 3.3, unless
`TBCAD_REQUIRE_GUI` is set. `--gate` runs the M1 measurements (see the pull request for the limits).

## Key bindings

Defaults: Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+Z, Ctrl+Y, Ctrl+A, Esc. Action ids:
`file.new`, `file.open`, `file.save`, `file.saveAs`, `edit.undo`, `edit.redo`, `edit.selectAll`,
`edit.deselect`, `view.zoomExtents`, `view.toggleCommandLine`, plus every menu item's own id.

## UX layer (follow-up to W3-1)

- **Theme tokens** (`libs/ui/theme.h`): Dark, Light and "follow Windows" (View > Theme, setting `ui.theme`). Panels and the overlay take colours from the tokens, never from literals. `ui.viewportBackground` (`#RRGGBB`) overrides the drawing background.
- **Command bar**: typing shows a ranked list (exact alias, alias prefix, name prefix, fuzzy); Up/Down, Tab or Enter pick. Ctrl+Shift+P opens it with an empty query as a command palette. Esc closes the list first.
- **Option chips** replace the `[A/B]` text of a prompt, with the key letters underlined; click or type them.
- **Cursor HUD** (F12, setting `hud.enabled`): prompt, typed text, snap label / tracking tooltip (`FilterFeedback::tooltip`) and clickable options next to the cursor.
- **Document tabs**: several drawings open at once, `●` marks unsaved ones, middle-click or ✕ closes (asks first), Ctrl+W closes, Ctrl+Tab switches. Each tab keeps its own undo history, view and selection. Autosave covers the active drawing only.
- **Start page** while only an untouched "Untitled" drawing is open.
- A command can copy text to the system clipboard by setting `clipboard.text`.

## Settings dialog, command log, clean screen

- **Tools > Settings** (`SETTINGS`, aliases `OPTIONS`, `OP`): tabs General, Display, Drafting, Files, Aliases and
  Keybindings. Everything is saved as you change it. The Aliases tab lists built-in and user aliases
  (clashes marked), adds, changes and removes user aliases, and writes `aliases.json` through
  `ui::AliasEditor`. The Keybindings tab captures the next key press for an action, warns when the keys are
  already used (Take over / Cancel), and can unbind or restore the default, through
  `ui::KeybindingEditor` and `keybindings.json`. A hidden `SETTINGSPAGE <page>` opens a tab directly.
- **Ctrl+L** (`view.expandLog`): the whole command history in one read-only box that can be selected and
  copied. **F11** (`view.cleanScreen`): hides menus, tool strip, status bar and panels, leaving the
  viewport and the command line. Both are rebindable like any other action.
- `IHost::requestUi(what)` lets a command ask the window to open something; `AppCore::takeUiRequests()`
  hands the requests to the window.

## Right-click menu and autosave of every drawing

- **Right-click in the drawing** opens a menu that depends on the state: while a command runs it lists
  Enter, Cancel, the prompt's options and Zoom Extents; with a selection it lists Move, Copy, Rotate, Scale,
  Mirror, Erase and Deselect All; otherwise Repeat <last command>, Select All, Undo, Redo and Zoom Extents.
  Setting `ui.rightClick = enter` (Settings > General) makes right-click act as Enter instead.
- **Autosave** writes every open drawing with unsaved changes (`recovery-<n>.tbcad` + `.json` in the
  autosave folder). Saving or closing a drawing removes its backup. After a crash the recovery prompt reopens all
  of them (`AppCore::recoverAll`); the first replaces the empty drawing, the others get tabs.
