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
Ortho and polar tracking exclude each other, as in AutoCAD; Shift flips ortho while held.

Object snap modes are chosen with the `OSNAP` command. Every key can be changed, see
[Configuration](configuration.md).

## Save

`Ctrl+S` saves a `.tbcad` file. Use `File > Open` or `OPEN` to load one. DXF files are converted
first, see [Opening DXF files](dxf.md).
