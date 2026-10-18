# Drawing variables (SETVAR)

`libs/commands/sysvars` holds the commands that set what new objects get and the drawing settings:
COLOR, LWEIGHT, CETRANSPARENCY, LTSCALE, UNITS, STATUS, RENAME and **SETVAR**.

**SETVAR** (alias SET) takes a variable name and then its new value, or `?` to list them with their
current values. Every change is one undo step, and the command line shows the value afterwards.

| Variable | Meaning | Same as |
| --- | --- | --- |
| LTSCALE | linetype scale factor | LTSCALE |
| CECOLOR | colour of new objects (name, 1-255, r,g,b, ByLayer, ByBlock) | COLOR |
| CELWEIGHT | lineweight of new objects | LWEIGHT |
| CELTYPE | linetype of new objects | |
| CETRANSPARENCY | transparency of new objects | CETRANSPARENCY |
| CLAYER | current layer (by name) | |
| INSUNITS | length unit (Millimeters, Centimeters, Meters, Inches, Feet); objects are not scaled | UNITS |

# BREAKLINE

**BREAKLINE** (alias BRL, in the draw2 module) draws a line with a break symbol: pick the two ends, then
where the symbol goes (Enter = the middle). The symbol is a Z shaped zigzag as high as it is wide; the
keyword **Size** at the first prompt sets its width (remembered, default 10). The result is one polyline.

# ADDSELECTED

**ADDSELECTED** picks an object and makes its layer, level, colour, linetype, lineweight and transparency
the ones new objects get. Start the drawing command afterwards (commands cannot start each other in this
version). One undo step.

# VIEW and TIME

**VIEW** (alias NAMEDVIEW) saves the centre and scale of the display under a name (**Save**), puts it back
(**Restore**), **Delete**s it or **List**s the views. The views are stored in the drawing and follow undo.
**TIME** shows the current date and time.

# RECOVER

**RECOVER** repairs the current drawing without asking (open the damaged file first): objects on a layer that
no longer exists go to layer 0, and broken objects (zero or non-numeric sizes, empty hatches, blocks that are
missing) are removed. **AUDIT** does the same checks and asks first.

`DWGUNITS` is another name for `UNITS`. `FLATTEN` only reports that nothing needs flattening, because TBCAD drawings are 2D. `SYNCHRONIZE` is another name for `ATTSYNC` (it brings the inserts of a block in line with the block's attributes).
