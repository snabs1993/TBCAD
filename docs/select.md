# Selection tools

| Command | Alias | What it does |
|---|---|---|
| `QSELECT` | `QSEL` | Select objects whose type, layer, colour, linetype or lineweight equals (or differs from) a value |
| `SELECTSIMILAR` | `SELSIM` | Select everything that matches a picked object (type and layer unless Settings says otherwise) |
| `FILTER` | `FI` | Build a list of conditions (all must hold), select the matches; `Save` / `Use` keep named filters in the file |
| `MATCHPROP` | `MA` | Copy properties between objects (in the Modify module) |
| `CHPROP` | `CH` | Change one property of the selection (also what Quick Properties runs) |
| `QUICKPROPERTIES` | `QP` | Show or hide the Quick Properties box (also View > Quick Properties) |
| `ISOLATE` / `HIDE` / `UNISOLATE` | | Hide or show objects without touching their layers |
| `PURGE` | `PU` | Remove unused layers, blocks, styles and linetypes |

## Mouse

- **Alt+click** cycles through the objects under the cursor, one per click.
- **Alt+drag** draws a freeform lasso. Dragging left to right selects what lies completely inside it, right to left everything it touches.
- Plain drags still make a window (left to right) or a crossing box (right to left).

## Quick Properties

When on, a small box in the corner of the viewport shows the common layer, colour, linetype and lineweight of the selection (`*VARIES*` when they differ). Changing a field runs `CHPROP`, so the edit is one undo step.
