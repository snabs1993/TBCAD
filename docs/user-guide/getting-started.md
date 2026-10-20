# Getting started

TBCAD is a free 2D CAD program released under the MIT licence.

## Install

Run `TBCAD-Setup-<version>.exe`. It installs for the current user only, so no administrator
rights are needed. Optional tasks: a desktop icon, the `.tbcad` file association, and an
"Open with TBCAD" entry for `.dxf` files. A portable ZIP is also published; unpack it anywhere and
run `tbcad.exe`.

## Draw

Type a command on the command line at the bottom, or use the menus. Press Enter or Space to
accept, Esc to cancel, and Enter on an empty line to repeat the last command.

- `LINE` (alias `L`): pick points, press Enter to finish.
- `CIRCLE` (alias `C`), `ARC`, `PLINE`, `RECTANG`: see [Commands](commands.md).
- Type coordinates directly: `10,20` is absolute, `@5,0` is relative, `@10<45` is polar.
- At any point prompt, type `FROM` (base point then offset), `M2P` (midpoint between two picks),
  `TT` (temporary tracking point), or a snap name such as `END`, `MID`, `PAR` for a one-time snap.
- Mouse wheel zooms, the middle button pans.

## Drafting aids

| Key | Toggle |
|-----|--------|
| F3  | Object snap |
| F7  | Grid display |
| F8  | Ortho |
| F9  | Grid snap |
| F10 | Polar tracking |

`POLAR` sets the increment angles, `Additional` angles that are tracked on their own (not their
multiples), and whether angles count from the X axis or from the last segment (`Relative`).
The Extension object snap works like AutoCAD's: rest the cursor on the end of a line, an open
polyline or an arc, then move along its extension (or the arc's circle). Two acquired line ends
also snap to where their extensions cross.

With dynamic input (F12), a whole coordinate typed into a field places the point: after the
first point `10,5` and `20<45` are relative to it and `#10,5` is absolute (the Dynamic Input
settings tab can make plain `x,y` absolute).

`SNAP` sets the spacing (`Aspect` for different X and Y), the `Base` point the lattice passes
through, and the `Type`: grid snap, or polar snap (steps of a distance along tracking lines only).
`GRID Spacing 0` makes the grid follow the snap spacing.
Ortho and polar tracking exclude each other, as in AutoCAD; Shift flips ortho while held.

Object snap modes are chosen with the `OSNAP` command. Every key can be changed, see
[Configuration](configuration.md).

## Save

`Ctrl+S` saves a `.tbcad` file. Use `File > Open` or `OPEN` to load one. DXF files are converted
first, see [Opening DXF files](dxf.md).

## Several drawings

Each open drawing has a tab at the top. Right-click a tab for New, Open, Save, Save As, Save All,
Close, Close All, Close Other Drawings, Copy full path, Open file location and Quick view.
`SAVEALL` saves every changed drawing that has a file and asks for a name for the first untitled
one. `CLOSEALL` closes them all and asks about each one with unsaved changes. `QVDRAWING` (`QVD`)
shows a card per open drawing with its model and layout buttons; `QVLAYOUT` (`QVL`) shows the
layouts of the current drawing.

## Export

`EXPORT` (`EXP`) asks for a file name and exports by its extension: `.dxf` (EXPORTDXF), `.pdf`,
`.svg`, `.png` or `.jpg` (the plot exports with their saved settings). With `FILEDIA` 1 a file
dialog asks for the name.
