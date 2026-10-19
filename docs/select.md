# Selection tools

| Command | Alias | What it does |
|---|---|---|
| `QSELECT` | `QSEL` | Select objects whose type, layer, level, colour, linetype, lineweight, transparency, hatch pattern, text height, radius or length equals (or differs from) a value; the numeric ones also take Greater and Less. The result Replaces the selection, is Appended to it, or keeps only the matches Within it |
| `SELECTSIMILAR` | `SELSIM` | Select everything that matches a picked object (type and layer unless Settings says otherwise; Settings can also match colour, linetype, lineweight, level, transparency, style and size) |
| `FILTER` | `FI` | Build a list of conditions (all must hold; `Or` starts another group, and an object matching any group is selected), select the matches in the whole drawing or, with `SElect`, only inside a selection; `Save` / `Use` keep named filters in the file |
| `MATCHPROP` | `MA` | Copy properties between objects (in the Modify module) |
| `CHPROP` | `CH` | Change one property of the selection (also what Quick Properties runs) |
| `QUICKPROPERTIES` | `QP` | Show or hide the Quick Properties box (also View > Quick Properties) |
| `ISOLATE` / `HIDE` / `UNISOLATE` | | Hide or show objects without touching their layers |
| `PURGE` | `PU` | Remove unused layers, blocks, styles and linetypes; names the layers and blocks it removed |
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
