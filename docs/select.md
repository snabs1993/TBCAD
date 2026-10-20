# Selection tools

| Command | Alias | What it does |
|---|---|---|
| `QSELECT` | `QSEL` | Quick Select dialog: apply to the whole drawing or the current selection, an object type (or Multiple), a property, an operator (=, <>, >, <, wildcard, Select All) and a value; include or exclude the matches, optionally appended to the selection |
| `-QSELECT` | | Typed form: select objects whose type, layer, level, colour, linetype, lineweight, transparency, hatch pattern, text height, radius or length equals (or differs from) a value; the numeric ones also take Greater and Less. The result Replaces the selection, is Appended to it, or keeps only the matches Within it |
| `SELECTSIMILAR` | `SELSIM` | Select everything that matches a picked object (type and layer unless Settings says otherwise; Settings opens the Select Similar Settings dialog and also takes the property names typed: colour, linetype, lineweight, level, transparency, style and size) |
| `FILTER` | `FI` | Object Selection Filters dialog: a list of conditions (AND within a group, OR starts a group), named filters to load, save and delete (one undo step each), and Apply to the whole drawing or only within the selection |
| `-FILTER` | | Typed form: build a list of conditions (all must hold; `Or` starts another group, and an object matching any group is selected), select the matches in the whole drawing or, with `SElect`, only inside a selection; `Save` / `Use` keep named filters in the file |
| `MATCHPROP` | `MA` | Copy properties between objects (in the Modify module) |
| `CHPROP` | `CH` | Change one property of the selection (also what Quick Properties runs) |
| `QUICKPROPERTIES` | `QP` | Show or hide the Quick Properties box (also View > Quick Properties) |
| `ISOLATE` / `HIDE` / `UNISOLATE` | | Hide or show objects without touching their layers |
| `PURGE` | `PU` | Purge dialog: the unused layers, blocks (with a picture), text styles, linetypes and dimension styles in a tree with check boxes, or the items still in use; "Purge nested items" repeats until nothing more goes; unnamed objects: zero-length geometry, empty text and orphaned data. Purge Checked Items or Purge All is one undo step |
| `-PURGE` | | Typed purge: a kind of name or All, then Zerolength, Emptytext and Orphaned each with a Yes/No; names what it removed |
| `LAYTRANS` | | Bring layers to an office standard: `Export` writes all layers to a CSV (`Old,New,Color,Linetype,Lineweight`), `Load` applies a CSV, renaming each layer or merging it into the existing layer of the new name and restyling it; one undo step |

## Mouse

- **Alt+click** cycles through the objects under the cursor, one per click.
- **Alt+drag** draws a freeform lasso. Dragging left to right selects what lies completely inside it, right to left everything it touches.
- Plain drags still make a window (left to right) or a crossing box (right to left).

## Quick Properties

When on (`QPMODE` 1, or `QUICKPROPERTIES`), a small box next to the pointer where the selection was made shows the common layer, colour, linetype and lineweight of the selection (`*VARIES*` when they differ). It stays put while the pointer moves on, and moves out of the way near the edges of the drawing. Settings > Selection can keep it in the top right corner of the drawing instead (AutoCAD's QPLOCATION). Changing a field runs `CHPROP`, so the edit is one undo step.

## Rollover tooltips

When the pointer rests on an object for a moment, a tooltip lists its type, colour, layer, linetype and length or radius. `ROLLOVERTIPS` 0 (or Settings) turns this off.

## CHANGE

**CHANGE** takes the selection and a point: the nearest end of a line moves to it, a circle's radius reaches it, text and
inserts move there (arcs and the rest are left alone). The keyword **Properties** works like CHPROP (layer, colour, linetype,
lineweight, linetype scale). One undo step.

`SELECTALL` selects every object that is shown and sits on a layer that is on, thawed and unlocked.

## Select objects options

At any "Select objects" prompt you can pick objects or drag a window (left to right) or crossing
(right to left), Alt+drag for a freeform lasso, or type an option: **W**indow, **C**rossing,
**BOX** (two corners, a window or crossing by direction), **WP**olygon, **CP**olygon, **F**ence,
**AL**l, **L**ast, **P**revious, **G**roup (by name), **A**dd, **R**emove, **SI**ngle (the first
pick or window ends the prompt), **AU**to and **M**ultiple (how picking always works here), **U**ndo,
and **?** to list them.

## Selection cycling

**SELCYC** (status bar) switches selection cycling on. **SELECTIONCYCLING** sets how it works:
2 (default) shows a list of the overlapping objects under the cursor after a click (the row under
the pointer is highlighted in the drawing), 1 picks the next object under the cursor on each click
at the same spot, 0 turns cycling off. Alt+click cycles whatever the setting.
