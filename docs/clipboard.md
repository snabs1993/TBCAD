# Clipboard, groups and draw order

## Clipboard

| Command | Keys | What it does |
| --- | --- | --- |
| `COPYCLIP` | Ctrl+C | Copies the selected objects. The base point is the lower-left corner of their bounding box. |
| `COPYBASE` | Ctrl+Shift+C | Same, but you pick the base point. |
| `CUTCLIP` | Ctrl+X | Copies, then erases the objects (objects on locked layers are left alone). |
| `PASTECLIP` | Ctrl+V | Pastes with the base point on the point you pick; the new objects are selected. |
| `PASTEBLOCK` | Ctrl+Shift+V | Pastes everything as one insert of a new block named `A$C1`, `A$C2` ... |
| `PASTEORIG` | | Pastes at the original coordinates, without asking for a point. |

The clip carries the layers, levels, linetypes, text styles, dimension styles and the block
definitions its objects use, found again by name in the target drawing and created when missing,
so you can copy between two open drawings. An existing block with the same name is kept as it is.
Hatch associativity is dropped.

The clip lives inside the application (copy in one drawing tab, paste in another). It is not on the
system clipboard yet, so copying to another program or another copy of TBCAD does not work.

Code: `libs/commands/clipboard` (`tbcad::clipboard::capture`, `paste`).

## Groups

`GROUP` [G] names a set of objects; picking one member (or a box that touches one) selects the
whole group in the viewport and in every command that asks for a selection. `UNGROUP` [UG]
dissolves the group of a picked object. An object belongs to one group at a time. Groups are stored
in the drawing header (`tbcad.groups`), so they are saved with the file and follow undo. Members
that are erased disappear from their group. Code: `libs/core/groups.h`.

## Draw order

`DRAWORDER` [DR]: Front, Back, or Above / Below a reference object. The relative order of the
moved objects is kept. Moving above or below a reference re-stacks every object from that position
up to the front.
