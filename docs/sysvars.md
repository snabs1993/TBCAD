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

**VIEW** (alias V, menu View > Named Views...) opens the View Manager: the named views with their
category, size and layer snapshot, and Set Current, New (current display or a window), Rename, Delete,
Update Layers and Category. Every button runs **-VIEW** (aliases -V, NAMEDVIEW), the typed form:
**Restore**, **Save**, **Window** (save the area between two corners), **Delete**, **Rename**, **LAyers**
(update the layer snapshot), **Category**, **sEttings** (save a layer snapshot with new views, setting
`view.layerSnapshot`, on by default) and **List**. A view is stored as `Header::custom["tbcad.view.<name>"]`
and follows undo; its layer snapshot is the hidden layer state `*view|<name>`, which the Layer States list
does not show. Restore fits the saved area to the window and puts the layer visibility back. On a sheet
it goes into the selected (or only) viewport, which must not be locked. Restore goes in the ZOOM
Previous history.

**VPORTS** (menu View > Viewports...) splits model space into tiled viewports: Single, two (Vertical or
Horizontal), three (Right, Left, Above, Below, Vertical, Horizontal) or four. Each tile has its own view; a
click or the wheel over a tile makes it the active one, and commands work in the active tile. Named
configurations are saved in the drawing (`Header::custom["tbcad.vports.<name>"]`, one undo step). The
typed form is **-VPORTS** with Save, Restore, Delete, SIngle, List, 2, 3 and 4. The tiles are for model
space; on a sheet use MVIEW.

**ZOOM** (alias Z) takes a window corner, a scale (nX relative to the view, nXP relative to paper), All,
Center, Dynamic (place and size a box over the whole drawing), Extents, Previous, Scale, Window and Object;
Enter starts real-time zoom (drag up to zoom in; Esc, Enter or a right click ends it). **PAN** (alias P)
starts real-time pan; **-PAN** takes two points. **ZOOM Previous** steps back through the last ten views:
wheel zooms and drag pans (one step per burst), PAN, ZOOM, -VIEW Restore, PLAN and UCSFOLLOW all add one.
**NAVBAR** ON/OFF (menu View > Navigation Bar, setting `ui.navbar`) shows a bar at the right edge of the
drawing with Pan, Zoom Extents, Window, Previous, In, Out and Real time.
**TIME** shows the current date and time.

# UNITS

**UNITS** (alias UN, menu Format > Units...) opens the Drawing Units dialog: length type and precision,
angle type and precision, Clockwise, the base angle, the insertion unit and a sample of how a length and an
angle will read. Every change runs **-UNITS**, so it is one undo step. **-UNITS** is the typed form:
Millimeters/Centimeters/Meters/Inches/Feet set INSUNITS, then **Precision**, **Type**, **Angles**,
**Direction** and **Base** set the rest. DIST, ID, AREA, LIST and MEASUREGEOM show lengths and angles in
these types (Engineering and Architectural read the drawing as inches: 15 shows as 1'-3").

# DIST, ID and MEASUREGEOM

**DIST** shows the distance, the angle in the XY plane, the angle from the XY plane and Delta X / Y / Z
(TBCAD drawings are flat, so the Z values are 0). **Multiple points** at the second point keeps a
running total along the points picked (**Undo** steps back, Enter or **Total** ends). **ID** shows X, Y
and Z. **MEASUREGEOM** (MEA) stays open after each measurement and asks for the next one (Distance,
Radius, Angle, Area, Volume, Quick) until **eXit** or Enter.

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
