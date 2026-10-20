# Customizing the interface

What AutoCAD keeps in its CUI file, TBCAD keeps in a few places, all reachable from Settings
(`SETTINGS`, or `CUI` to open the Customize page directly):

| What | Where | Commands |
| --- | --- | --- |
| Command aliases | Settings > Aliases, saved to `aliases.json` | `ALIASEDIT`, `ALIASIMPORT` / `ALIASEXPORT` (acad.pgp files), `REINIT` |
| Keyboard shortcuts | Settings > Keybindings, saved to `keybindings.json` | |
| Quick Access toolbar | Settings > Customize | right-click the toolbar > Customize... |
| Right-click menu | Settings > Customize, and Settings > General for what a right click does | |
| Double-click actions | Settings > Customize | `DBLCLKEDIT` |

## Quick Access toolbar
Command lines (`CIRCLE`, `ZOOM E`, `-LAYER M Walls`) shown as buttons at the end of the toolbar,
in the order of the list. Each must start with a command or an alias.

## Right-click menu
Two lists of command lines: one added to the drawing's menu when nothing is selected, one when
objects are selected. They come after the recent commands.

## Double-click actions
What a double click on each type of object runs, with the object selected. The defaults follow
AutoCAD's:

| Type | Default |
| --- | --- |
| Text, multiline text | `TEXTEDIT` |
| Polyline | `PEDIT` |
| Spline | `SPLINEDIT` |
| Multiline | `MLEDIT` |
| Hatch | `HATCHEDIT` |
| Block reference | `BEDIT` (the block editor on that block) |
| Block with attributes | `EATTEDIT` |
| Everything else | `PROPERTIES` (the Properties panel) |

An empty field uses the default; a command that does not exist opens the Properties panel.
`DBLCLKEDIT 0` turns double-click editing off: a double click then only selects.
