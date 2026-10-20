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
| INSUNITS | length unit (Millimeters, Centimeters, Meters, Inches, Feet); objects are not scaled | -UNITS |
| LUNITS | how lengths are shown: 1 Scientific, 2 Decimal, 3 Engineering, 4 Architectural, 5 Fractional | -UNITS Type |
| AUNITS | how angles are shown: 0 Degrees, 1 Deg/Min/Sec, 2 Grads, 3 Radians, 4 Surveyor | -UNITS Angles |
| ANGDIR | angles measured clockwise (1) or counterclockwise (0) | -UNITS Direction |
| ANGBASE | direction of angle 0 (east by default) | -UNITS Base |
| AUDITCTL | 1 = AUDIT also writes its report to a .adt file next to the drawing | AUDITCTL |

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

# UNITS

**UNITS** (alias UN, menu Format > Units...) opens the Drawing Units dialog: length type and precision,
angle type and precision, Clockwise, the base angle, the insertion unit and a sample of how a length and an
angle will read. Every change runs **-UNITS**, so it is one undo step. **-UNITS** is the typed form:
Millimeters/Centimeters/Meters/Inches/Feet set INSUNITS, then **Precision**, **Type**, **Angles**,
**Direction** and **Base** set the rest. DIST, ID, AREA, LIST and MEASUREGEOM show lengths and angles in
these types (Engineering and Architectural read the drawing as inches: 15 shows as 1'-3").

# AUDIT and RECOVER

**AUDIT** (menu File > Drawing Utilities) checks every object and reports what it finds: objects on a layer
that no longer exists (asks, then moves them to layer 0), a linetype that is gone (set to ByLayer), text with
a text style that is gone (set to Standard), and broken objects (zero or non-numeric sizes, empty hatches,
blocks that are missing; asks, then removes them). It ends with "Total errors found N, fixed M." With
**AUDITCTL** on, the report is also written to a .adt file next to the drawing. **RECOVER** does the same
without asking; RECOVER with a file name opens that drawing first.

**DRAWINGRECOVERY** (alias DRM) opens the Drawing Recovery Manager: autosaves left by a session that did not
close normally, and the .bak copies of open and recent drawings, each with an Open button. A recovered file
opens as a new unsaved drawing; save it under a name to keep it. "Discard all autosaves" removes the
leftover autosaves.

# DWGPROPS

**DWGPROPS** (menu File > Drawing Properties...) opens the Drawing Properties dialog: General (name,
folder, size, last change), Summary (title, subject, author, keywords, comments), Statistics (object, layer,
block, style, linetype and layout counts) and Custom properties (name and value, added and deleted here).
**-DWGPROPS** is the typed form; its **Custom** option takes a name and a value (an empty value removes it).

`DWGUNITS` is another name for `-UNITS`. `FLATTEN` puts the selected objects on the default level (TBCAD drawings keep no Z, so a level is the only height an object can have) and says so when nothing needed it. `SYNCHRONIZE` is another name for `ATTSYNC` (it brings the inserts of a block in line with the block's attributes).

## HYPERLINK, SPACETRANS and DELAY

`HYPERLINK` keeps a web address or file name and a description on an object: Set, Remove, Open (asks the application to open it) or List. The links are saved in the drawing, tied to the object. `SPACETRANS` converts a length from paper to model units or back, using the scale you give (the annotation scale by default). `DELAY` waits the given number of milliseconds (0 to 10000), for scripts.
