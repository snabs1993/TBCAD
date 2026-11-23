# TBCAD handover notes (for a new AI assistant)

Written 2026-11-23. Read this first, then `AGENTS.md` and `CONTRIBUTING.md` in this folder.
Keep this file up to date when rules or priorities change.

## 1. What this is

- **TBCAD**: a free, open-source 2D CAD program meant to replace AutoCAD for everyday drafting
  (Thai architects/engineers first). Licence **MIT**, copyright "TBCAD contributors".
  Anyone may use the code in their own projects without opening their own code.
- Public repo: `https://github.com/snabs1993/TBCAD` (old name TCAD redirects). Default branch `main`.
- Paid features live in a **separate private repository**. Never copy, mention or push paid code
  into this public repo. Everything in this repo is free.

## 2. The owner and how to work with them

- The owner (GitHub `snabs1993`) is Thai and **not a developer**. Reply to them **in Thai, short,
  plain words**. Never put Windows paths in code blocks in messages to them.
- **You decide all technical questions yourself.** Only bring them money, accounts, legal matters
  and product direction. They will not review pull requests.
- "พัก" = pause, "ทำต่อ" = continue.
- If you need a permission or access, ask immediately while the owner is there.

## 3. Owner rules (must follow)

1. **Quality over "done"** ("ทำต้องออกมาดี ไม่ใช่ทำออกมาให้เสร็จๆ"). Try every change for real the way
   a user would (UI changes on Windows in the real app, not only unit tests). Think about edge cases:
   unsaved files, two monitors, Thai text, real house drawings. Give clear feedback in the UI: no
   silent refusals, no tiny 3-second toast as the only signal.
2. **Release gate.** Do not rush test builds. Before any `0.1.0-test.N` release: real mouse and keyboard
   walkthrough of the installed build with real house drawings (draw, edit, select, TRIM, XREF, XCLIP,
   text, layers, plot/print, save and reopen), fix what is found, add an automated regression test for
   every bug found. Only then release.
3. **DXF is the only save format.** TBCAD extras are stored inside the DXF. Old `.tbcad` files open
   only (read-only import), never saved.
4. **Our own UI, never a copy of AutoCAD.** Familiar short aliases (L, C, PL...) and F-keys are fine;
   layout, look, panels and wording are our own. Thin menus, big canvas, no floating prompt box that
   follows the mouse. The dark "glow" theme was rejected: do not propose it again. When the owner
   asks to "see an example", make a mockup image, not a real build.
5. **Clean room.** Never copy Autodesk files, text, data or names. No "AutoCAD", "Autodesk", "DWG",
   "TrustedDWG" in product names, file names, UI text or metadata. DWG reading is written from the
   public ODA specification only.
6. **No GPL/LGPL/AGPL/MPL code**, no new dependencies without a strong reason.
7. Never commit `Plotstyles/`, `Plotstyles.rar`, `Datafef/` or any owner drawing file. Owner sample
   drawings stay on their PC; never copy them into the repo.
8. Parked, do not propose again soon: DWG writing; dark glow theme; Chinese AI models.
   Backlog, do not start: Revit-style layer "View Templates".

## 4. Priorities (owner, in order)

1. Everyday draw/edit commands correct, no crash.
2. Undo / save / open never lose data.
3. Real DWG files open correctly (object counts must never drop).
4. Familiar, easy UI.
5. Ready for friends to test (installer, first run, help).

Big-file speed is the **lowest** priority.

## 5. Tech decisions

- C++20, Windows first (MSVC 2022), Linux for CI/dev. GPU renderer (OpenGL), UI with **Dear ImGui**
  (multi-viewport, panels can float). Namespace `tbcad::<module>`.
- Geometry in `double`, millimetres, angles in radians. Default unit mm. UI text English (Thai text in
  drawings fully supported, including tone marks mixed with English).
- Layout: `libs/<module>/{include/tbcad/<module>,src,tests}`, app in `apps/` (`apps/tbcad`, GUI in
  `gui.cpp` / `AppCore`), command line converter `tbcad-convert` (DXF/DWG import, PDF).
- Tests: doctest, one test per behaviour change. GUI tests in `tbcad_gui_tests` (golden images,
  scripted clicks). Warnings are errors, also on MSVC `/W4 /WX`.
- UI buttons must start commands through `AppCore::startCommand` (not by calling commands directly).
- Settings are key/value (e.g. `ui.flatTables`, `ui.toolbox.labels`, `layout.newViewport`, `trim.quick`).
- A built-in MCP server (MIT) lets AI tools draw in TBCAD.
- LAN collaboration on one file is free and in this repo.

## 6. Build and test

**On the owner's Windows PC** (source `D:\TCAD`, build folder `D:\tb`):

```
git pull --ff-only origin main        # this clone may be old: pull before anything else
tools\pc-check.cmd                    # vcvars64 + configure D:\tb (Release, Ninja) + build + ctest -j4
tools\pc-check.cmd nopull fast        # tests only, no pull, no build
```

Logs: `D:\tb\pc-build.log`, `D:\tb\pc-ctest.log`, verdict in `D:\tb\pc-summary.txt`.
Presets also exist: `cmake --preset windows-msvc-debug` / `windows-msvc-release`.

**On Linux** (see AGENTS.md):

```
tools/cloud-setup.sh                         # once: X11/GL headers, configures with -DTBCAD_BUILD_APP=ON
cmake --preset linux-clang-debug
cmake --build build/linux-clang-debug -j3 --target <only what you need>
ctest --test-dir build/linux-clang-debug --output-on-failure -R <related tests>
TBCAD_REQUIRE_GL=1 TBCAD_REQUIRE_GUI=1 xvfb-run -a -s "-screen 0 1600x1000x24" ctest ...   # GUI tests
```

Without `TBCAD_BUILD_APP=ON` the GUI tests are silently skipped: any panel/dialog change must build and
run `tbcad_gui_tests`. Golden images update with `TBCAD_UPDATE_GOLDEN=1`.

**Windows check before merging:** `tools/wincheck.sh` (local: clang with MSVC-style warnings on changed
files) and `tools/wincheck.sh push` (real MSVC build on GitHub's Windows runner, branch
`wincheck/<branch>`; delete that branch afterwards). Gotchas: MSVC `windows.h` defines `small`; a menu
item that opens a native window (PrintDlgW) hangs tests unless guarded by
`platform::nativeDialogsAvailable()`; GCC Release can crash compiling `xclip_command.cpp` (use clang).

**Workflow the owner agreed:** build only the targets you need, run only related tests (plus wincheck),
then merge your own PR; do not wait for CI, fix `main` forward if it goes red. Every bug fixed gets a
regression test.

**Releases:** test builds are public GitHub pre-releases titled "TBCAD 0.1.0-test.N", made by the
`release.yml` workflow (inputs version=0.1.0-test.N, publish=true, public=true). Latest: **test.21**
(2026-11-16). Check that Windows CI is green before dispatching; the release runs a Release-mode
ctest that includes performance budgets.

## 7. State of work (as of 2026-11-23)

Recently finished (in `main`, not yet in a release, owner should retest in test.22):
- test.21 feedback fixes (#1288): ARC PDF stray line, BREAK on MLINE, -BLOCK Last on layouts,
  VPLAYER comma list, LAYOUT New viewport layer, typo "Freezen".
- Drag a drawing tab out to its own window or a 2nd monitor, also for unsaved drawings (#1279, #1287,
  handover via a temporary `.tbcadmove` file).
- Friendlier command line (#1281-#1283): click-click window select, full command names and *Cancel*
  in history, typo-tolerant autocomplete (lnie -> LINE), taller command area, TRIM cuts on first click.
- XCLIP/XREF (#1280, #1284, #1285): clip frame shown, one Attach dialog, xref fade 30% on screen only,
  REFEDIT saves DXF xrefs.
- DXF save fuzz (#1276): every command, save, reopen, compare.

Open / next:
1. **Release gate for test.22**: full real-mouse walkthrough on the installed build (section 3.2), then
   release. Items still needing a check on Windows: Publish Ctrl+A, Object Properties layer row, Make
   Block focus, tab tear-off across monitors, Thai cursor movement in text boxes, real print fit-to-paper.
2. **DWG regression**: one real DWG lost objects (5382 -> 5332 objects, 110 -> 81 blocks). Find and fix;
   DWG changes merge only if no object counts drop.
3. UX findings not yet fixed: dimensions unreadable at 1:1 in building-size drawings with no hint;
   default Layout1/Layout2 empty so PLOT prints a blank page silently; Layers panel Linetype column cut
   off; layer names cut from the left in narrow panels; single click on text opens the text editor bar
   over the canvas; history shows internal names (GRIPEDIT x,y).
4. Layout plot vs original PDF: a block (road reserve line) plots although the original hides it;
   suspect viewport-frozen layers or annotation scales not read from DWG. Low priority.
5. Annotation scales: CANNOSCALE and DWG annotative scales are never imported.
6. Installer check in a clean Windows (Windows Sandbox: install, first drawing, save, uninstall).
7. Remaining AutoCAD-parity items are listed in the project's task queue; most daily commands already
   exist, so remaining work is mostly finding wrong behaviour in real use.

## 8. Testing on the owner's PC with real mouse

- Use the **installed** TBCAD (setup.exe, Start menu, `C:\Program Files\TBCAD\tbcad.exe`), not a
  portable copy.
- Owner has 2 monitors. Leave their screen exactly as you found it: never minimize, hide or close their
  windows.
- Use the owner's real house drawings for testing, but never copy them into the repo.
