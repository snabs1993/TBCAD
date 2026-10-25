# Drawing templates

A template is a normal `.tbcad` file with the extension `.tbt`. New drawings start from a copy of one,
so title blocks, layers, linetypes, text styles, dimension styles, units and even entities carry over.

## Where they live
`%APPDATA%\TBCAD\templates\` on Windows, `~/.config/tbcad/templates/` elsewhere (`TBCAD_CONFIG_DIR`
moves it). Copy a `.tbt` file in there to share a template with a colleague.

## Built-in templates
Without a user template NEW uses the built-in drawing, in millimetres. `Built-in (inch)` is the same
drawing in inches, with acad.lin's inch linetype patterns and AutoCAD's imperial dimension sizes
(0.18" text and arrows, four decimals). Both have:

- Layers: `0`, Defpoints (not plotted), Walls, Doors-Windows, Furniture, Dimensions, Text, Hatch,
  Centerlines, Hidden, Grid, Construction (not plotted), Title-Block, Viewports (not plotted).
- Linetypes: Continuous, Dashed, Hidden, Center, Phantom, Dot, DashDot.
- Text styles: Standard, Notes. Dimension styles: Standard (filled arrows), Architectural (ticks).
- Layouts: Layout1 and Layout2, empty landscape sheets (A3, or ANSI B in inches).

## Commands
| Command | Does |
| --- | --- |
| `SAVEASTEMPLATE` | Saves the current drawing as `<name>.tbt` in the templates folder. |
| `NEWFROMTEMPLATE` (`QNEWT`) | Opens a new drawing from a template, or `Built-in`. Also in File > New from Template. |
| `TEMPLATEDEFAULT` | Chooses the template NEW, the tab "+" button and the first start use. Also in Settings > Files. |

Opening a `.tbt` file (File > Open) gives an untitled copy; the template file is never changed by it.
If the chosen default template is missing or unreadable, TBCAD says so in the history and uses the
built-in drawing.

## DXF and DWT templates, Save As template

- A `.dxf` or `.dwt` file placed in the template folder is listed by NEWFROMTEMPLATE and TEMPLATEDEFAULT under its file name (`Sheet.dxf`). A full path to a `.dxf`, `.dwt` or `.tbt` also works. The new drawing is untitled; the template file is never overwritten.
- Save As with the "Drawing Template" filter (`.tbt`) writes a template copy; the open drawing keeps its own name.
