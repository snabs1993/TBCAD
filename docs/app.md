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

- **Files**: `newDocument`, `openFile`, `save`, `saveAs` (adds the extension of `files.defaultFormat`, `.tbcad` unless set to `dxf`; atomic with a `.bak`; a `.dxf` path is read and written with `libs/dxf`, so a drawing opened from DXF is saved back as DXF),
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
- **Autosave**: `tickAutosave` writes `recovery.tbcad` in the background (the drawing is encoded on
  the UI thread, the rest runs on other threads; `autosaveNow()` and `waitForAutosave()` wait for it) every `autosave.minutes` (default 10) while
  there are unsaved changes; a normal save, new, open or exit removes it. At the next start
  `pendingRecovery()` finds a leftover and the window offers to recover it.
- **Working area**: a commit that puts geometry outside the working area raises
  `outOfAreaNotice()`; the window shows it with "Zoom to" and "Undo" (`outOfAreaZoomTo`,
  `outOfAreaUndo`). Undo and redo by hand do not raise it again.
- **`DEVGEN LINES 1000000`**: hidden command (no description, not completed) that creates the
  reproducible random test drawing of the M1 gate in one undo step.

## The window (`apps/tbcad`)

Layout: menu bar, tool strip, docked panels (Command at the bottom, Object Properties at the right, plus
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
tbcad [file.tbcad|file.dxf] [--script "CMD;CMD"] [--frames N] [--screenshot out.ppm]
      [--gate [--report out.txt]] [--self-test] [--no-vsync]
      [--capture-script script.txt [--size 1600x900]]
```

`--capture-script` is for pictures of the real program without anyone at the screen: the window is
hidden and never takes focus, the script runs, and each `@shot` line writes a PNG of the whole
window. A line is a command line (an empty line is Enter, `#` starts a comment) or one of
`@size W H`, `@frames N`, `@mouse X Y`, `@click X Y`, `@drag X0 Y0 X1 Y1` (window pixels),
`@shot name.png` (a relative name is placed beside the script), and the view helpers
`@zoom X0 Y0 X1 Y1` (world box), `@zoom-all` (extents without the outliers: the middle 96 % of the
objects), `@zoom-dense [N]` (the busiest cell of an N x N grid, default 40), `@zoom-kind KIND
[index [factor]]` (the index-th hatch, dimension, text, mtext, insert, leader, mleader, table, ...
in draw order, shown `factor` times its size) and `@stats` (prints how many objects of each kind
are in view and in the drawing, and how many are selected; it is also printed after every shot),
`@deselect` (clear the selection) and the shot line itself: an older file of the same name is
deleted first, and a warning is printed when a step before the shot failed. A byte order mark at
the start of the script is ignored. Open a drawing with `tbcad file.dxf --capture-script ...`;
zoom and selection are ordinary commands (`ZOOM`, `W`, `SELECT`) or a `@drag` across the view. A shot waits until the
drawing has finished building. The program exits when the script ends (status 1 if a line failed).

Scenario scripts drive whole workflows like a user and check the result: `@mousew X Y`,
`@clickw X Y`, `@dblclickw X Y` and `@dragw X0 Y0 X1 Y1` act at drawing coordinates (sheet
millimetres on a layout; a click lands on the nearest pixel, so exact points are typed or picked
with an object snap), `@clickitem LABEL [| WINDOW [| N]]`, `@dblclickitem` and `@rclickitem` click
a widget by its label like the GUI tests do (`@items [WINDOW]` lists the labels), `@key
[ctrl+|shift+]NAME` (Enter, Escape, Delete, Tab, Space, arrows, F1-F12, A-Z), `@type TEXT`
(characters into the focused field), `@wait`, `@dump` (prints the first objects with their points)
and `@expect`: `objects N`, `selected N`, `KIND N` (line, circle, hatch, ...), `endpoint X Y N`
(line ends and polyline vertices at a point), `layer NAME on|off|frozen|thawed|locked|unlocked`,
`on-layer KIND LAYER N` and `color KIND ACI N` (KIND `any` counts all), `text N SUBSTRING` (TEXT
and MTEXT holding it), `layout NAME` (`Model` in model space), `viewport LAYOUT ID scale N` and
`viewport LAYOUT ID center X Y`, `file NAME` (exists and is not empty), `contains NAME TEXT` (the
file holds the text) and `prompt TEXT` (the current prompt contains it). The command line's history
is printed with a `| ` prefix. A failed check prints `FAILED line N` and the exit status is 1.
Lines like `@10,5` or `@20<45` are relative points, not capture lines. `{out}` in a line is
replaced by the output folder (`--capture-out`, default the script's folder) and `{dir}` by the
script's folder. The scenarios in `apps/tbcad/tests/scenarios` run as ctest `tbcad_scenario_*`
(skipped without a display); each gets fresh settings through `TBCAD_CONFIG_DIR`. Their test
plot style table `data/ep-test.ctb` is made by `tools/make_test_ctb.py`.

Scripts can also run on real drawings. `{env:NAME}` (or `{env:NAME|fallback}`) is replaced by an
environment variable and `{stem:NAME}` by the file name, without extension, of the path in it;
`@require-env NAME` skips the script (exit 77) when the variable is not set. `@step NAME` starts a
timed step: when the next one starts (or the script ends) the program waits for the drawing to
settle and prints `step NAME: S s (N frames)`; a command still running at a step boundary is ended
with Esc and counted as a failure. `@count NAME` remembers the number of objects and layouts, and
`@expect same NAME` / `@expect added NAME K` compare with it. Generic steps find something to work
on in any drawing: `@enter-viewport` shows the first layout with a viewport and double-clicks into
it (sets `{layout}` and `{vpid}`; `@expect viewport-zoomed F` checks the viewport now shows F times
closer), `@hatch-pick` zooms to a closed polyline that nothing crosses (sets `{pick}`, a point in
its middle) and `@select-kind KIND[|KIND] [i]` selects such an object (sets `{picked}`).
`@skip-step-unless NAME` leaves out the rest of the step when `{NAME}` came out empty, and
`{NAME|fallback}` gives a value for that case. `10_real_file.txt` runs open, viewport zoom and
pan, select all, move and undo, hatch pick, text edit, plot and save-and-reopen (as .tbcad and as
.dxf) on the drawing named by `TBCAD_SCENARIO_DWG`, plotting with `TBCAD_SCENARIO_CTB` when set:

```
set TBCAD_SCENARIO_DWG=C:\drawings\S2312.dwg
set TBCAD_SCENARIO_CTB=C:\plotstyles\office.ctb
tbcad --capture-script apps\tbcad\tests\scenarios\10_real_file.txt --capture-out C:\out\S2312
```

`--self-test` opens a hidden window, runs the script, draws the frames and checks that something
was drawn; it exits with 77 (skipped by ctest) when there is no display or OpenGL 3.3, unless
`TBCAD_REQUIRE_GUI` is set. `--gate` runs the M1 measurements (see the pull request for the limits).

## Automated GUI tests

`tbcad_gui_tests` (apps/tbcad/tests) drives the real window code (menus, panels, dialogs, command
window) without a display or a GPU: GLFW runs on its null platform, Dear ImGui receives queued mouse
and keyboard events, and a small software rasteriser paints the draw lists into a picture. It runs
in ctest on every platform, so a panel that compiles but does nothing fails a pull request.

- `guitest::Harness` (`gui_harness.h`): `click("Label", "Window")`, `rightClick`, `enterText`,
  `key`, `holdCtrl`, `command("LINE")`, `windowShot("Layers")`. Widgets are found by the label they
  were created with (recorded by `cmake/deps/imgui_extra/tbcad_imgui_items.*`); combo boxes and
  other unlabelled widgets by their `"##id"`. Only the front tab of a docked group draws, so click
  its tab first. Open popups and combos are windows named `##Popup...`, `##Combo...`, `##Menu...`.
- Panel state is shared between tests in one run (the panels keep it in statics), so each test
  filters or selects what it needs instead of relying on a clean panel.
- `matchesGolden(image, "name")` compares with `apps/tbcad/tests/golden/name.png`, allowing a small
  per-channel difference and a small share of differing pixels. On a mismatch the actual and
  difference pictures are written to `<build>/apps/tbcad/gui-test-failures`. Run with
  `TBCAD_UPDATE_GOLDEN=1` to write the golden files after a change on purpose, and look at them
  before committing. Take golden pictures of one window, never of text that holds a path.

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

## DXF as the main file

File > Open and drag-and-drop accept `.dxf` as well as `.tbcad`. Save on a drawing that came from a `.dxf` writes DXF (R2000) back to the same file, keeping the old one as `.bak`; notices from the reader and writer (skipped or simplified objects) go to the command history. Tools > Settings > Files > "Default save format" chooses the extension for new drawings; `.tbcad` stays the default.

## Tool strip icons

The tool strip is one row of 26 px icons in groups (File, Edit, Draw, Modify, View) with tooltips. The icons
are our own line drawings (`apps/tbcad/src/icons.cpp`, a 20 x 20 grid drawn with line primitives), so they take
the theme colour and stay sharp at every DPI. Commands that are not registered are left out. Right-click the
strip to show or hide a group (settings `ui.toolgroup.<Group>`).

## Speed, memory and crash reports

Measured on a Release build (GCC) with a synthetic 100 MB DXF of 721,000 lines, circles, polylines and
texts (`tools/gen_big_dxf.py`): import to `.tbcad` takes about 3.7 s, export back to DXF about 3.1 s, and
the importer peaks at about 430 MB of memory. Inside the window, IMPORTDXF of the same file takes about
8.6 s (index, undo and display lists included). The window opens in about 0.2 s (Debug build, software
OpenGL). The CI `perf` job repeats the large-file run and fails if either direction takes over a minute.

If TBCAD crashes it writes `crash-<time>.txt` (signal or exception and a stack trace; on Windows also a
`crash-<time>.dmp` minidump) into the `crashes` folder of the data directory. Nothing is uploaded. At the
next start a dialog names the report and, when an automatic backup exists, offers to recover the drawings.

Opening a file over 8 MB from the menu, the file browser, a recent-files entry or a drop shows an
"Opening <name> ..." message for one frame before the (blocking) load starts, so the window does not
look frozen. A profile of opening a 20 MB drawing (Release, callgrind) put about a quarter of the load
time in text shaping and layout, which runs twice per text entity (once for the index bounds, once for
the display list); shaped runs are now cached by (font chain, direction, text), so repeated strings are shaped once.
