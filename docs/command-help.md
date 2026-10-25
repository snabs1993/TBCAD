# TBCAD command help

Offline help pages for the everyday commands. F1 while a command runs opens its page; type `HELP` or `?` on the command line to list or search them (`HELP OFFSET`, `? hatch`). Shipped as assets/help/commands.md. Format: libs/cmd/include/tbcad/cmd/help.h.

## LINE
Draws straight segments (alias L).
Usage: Pick the first point, then each next point. Enter or Esc ends; right-click also ends.
Options: Close (join back to the first point), Undo (remove the last segment).
Example: LINE 0,0 100,0 @0,50 C draws an L-shaped triangle closed to the start.

## CIRCLE
Draws a circle (alias C).
Usage: Pick the centre, then a radius by clicking or typing it.
Options: 3P (three points), 2P (two diameter end points), Ttr (tangent, tangent, radius), Diameter.
Example: CIRCLE 50,50 25 draws a circle of radius 25 at 50,50.

## ARC
Draws an arc (alias A).
Usage: Pick the start point, a second point on the arc and the end point (three-point arc).
Options: Center, End, Angle, Direction, Radius, Length of chord.
Example: ARC 0,0 5,5 10,0 draws an arc through the three points.

## POLYLINE
Draws a connected chain of line and arc segments as one object (aliases PL, PLINE).
Usage: Pick points one after another; Enter ends.
Options: Arc (switch to arc segments), Line (back to lines), Close, Halfwidth, Width, Length, Undo.
Example: PLINE 0,0 W 2 2 100,0 Enter draws a 2 mm wide line.

## RECTANGLE
Draws a rectangle as a closed polyline (aliases REC, RECTANG).
Usage: Pick one corner, then the opposite corner.
Options: Chamfer, Fillet, Width, Dimensions (type length and width), Area, Rotation.
Example: RECTANGLE 0,0 @200,100 draws a 200 by 100 rectangle.

## POLYGON
Draws a regular polygon with 3 to 1024 sides (alias POL).
Usage: Enter the number of sides, pick the centre, choose Inscribed or Circumscribed, then give the radius.
Options: Edge (pick one side instead of the centre).
Example: POLYGON 6 0,0 I 50 draws a hexagon inside a circle of radius 50.

## ELLIPSE
Draws an ellipse or an elliptical arc (alias EL).
Usage: Pick the two end points of one axis, then the distance to the other axis.
Options: Center, Arc, Rotation.
Example: ELLIPSE 0,0 100,0 30 draws an ellipse 100 long and 60 high.

## SPLINE
Draws a smooth curve through fit points (alias SPL).
Usage: Pick points; Enter ends.
Options: Close, Fit tolerance, start and end Tangents.
Example: SPLINE 0,0 20,30 50,10 80,40 Enter.

## POINT
Places a point marker.
Usage: Pick the position. The marker style is set with DDPTYPE or PTYPE.
Options: none.
Example: POINT 10,10.

## HATCH
Fills an enclosed area with a pattern, a solid colour or a gradient (alias H).
Usage: Pick a point inside a closed boundary, or choose Select objects; adjust pattern, scale and angle in the Hatch panel; Enter accepts.
Options: Pick points, Select objects, Settings, Pattern, Scale, Angle.
Example: HATCH, then click inside a rectangle.

## ERASE
Deletes objects (alias E).
Usage: Select objects, then Enter. Without a selection a click or window before the command also works. OOPS brings the last erase back.
Options: none; undo with U or Ctrl+Z.
Example: ERASE ALL Enter removes everything.

## MOVE
Moves objects (alias M).
Usage: Select objects, Enter, pick a base point, then a second point or type a displacement.
Options: Displacement.
Example: MOVE, select, Enter, 0,0, @100,0 moves 100 to the right.

## COPY
Copies objects (alias CO, CP).
Usage: Select objects, Enter, pick a base point, then one or more second points; Enter ends.
Options: Displacement, mOde (Single or Multiple), Array.
Example: COPY, select, Enter, 0,0, @50,0, @100,0 Enter makes two copies.

## ROTATE
Rotates objects around a point (alias RO).
Usage: Select objects, Enter, pick the base point, then give the angle.
Options: Copy (keep the original), Reference (rotate from one angle to another).
Example: ROTATE, select, Enter, 0,0, 90 turns the objects 90 degrees counter-clockwise.

## SCALE
Resizes objects around a base point (alias SC).
Usage: Select objects, Enter, pick the base point, then give the scale factor.
Options: Copy, Reference (new length over old length).
Example: SCALE, select, Enter, 0,0, 2 doubles the size.

## MIRROR
Makes a mirror image of objects (alias MI).
Usage: Select objects, Enter, pick the two ends of the mirror line, then answer whether to delete the originals.
Options: Yes or No for deleting the source objects. The setting MIRRTEXT decides whether text is reversed.
Example: MIRROR, select, Enter, 0,0, 0,10, N.

## OFFSET
Draws a parallel copy of a line, arc, circle, polyline or spline at a distance (alias O).
Usage: Enter the distance, select the object, click the side to place the copy; repeat; Enter ends.
Options: Through (copy passes through a picked point), Erase (remove the source), Layer (Current or Source), Multiple, Undo, Exit.
Example: OFFSET 10, click a line, click to one side.

## TRIM
Cuts objects at the edge of other objects (alias TR).
Usage: Select the cutting edges and press Enter (Enter alone uses everything), then click the parts to remove. Shift-click extends instead.
Options: Fence, Crossing, Edge (Extend or No extend), Mode (Quick or Standard), Remove, Undo.
Example: TRIM Enter, click each overhanging end.

## EXTEND
Lengthens objects up to the edge of other objects (alias EX).
Usage: Select the boundary edges and press Enter (Enter alone uses everything), then click the objects to extend. Shift-click trims instead.
Options: Fence, Crossing, Edge, Mode, Undo.
Example: EXTEND Enter, click the end of a line near the boundary.

## FILLET
Rounds a corner between two objects with an arc (alias F).
Usage: Set the radius with R, then pick the two objects. Radius 0 makes a sharp corner.
Options: Radius, Polyline (all corners of one polyline), Trim or No trim, Multiple, Undo.
Example: FILLET R 5 Enter, then click two lines.

## CHAMFER
Bevels a corner between two lines (alias CHA).
Usage: Set the distances with D, then pick the two lines.
Options: Distance, Angle, Polyline, Trim, Method, Multiple, Undo.
Example: CHAMFER D 5 5 Enter, then click two lines.

## STRETCH
Moves the part of the drawing inside a crossing window and stretches what crosses it (alias S).
Usage: Select with a crossing window (drag right to left), Enter, pick a base point and a second point.
Options: Displacement.
Example: STRETCH, drag a crossing window over one end of a room, Enter, 0,0, @500,0.

## BREAK
Cuts an object in two or removes a piece (alias BR).
Usage: Click the object, then the second break point. The first point is the click unless you choose First point.
Options: First point. BREAKATPOINT (BAP) cuts at one point without a gap.
Example: BREAK, click a line, click a second point.

## JOIN
Joins lines, arcs and polylines that touch end to end into one object (alias J).
Usage: Select the objects, Enter.
Options: none.
Example: JOIN, select two touching lines, Enter.

## ARRAY
Copies objects in a rectangular, polar or path pattern.
Usage: Select objects, Enter, choose Rectangular, Polar or Path, then give counts and spacing. ARRAYEDIT changes an array afterwards.
Options: Rectangular, Polar, PAth.
Example: ARRAY, select, Enter, Rectangular, 3 rows, 4 columns.

## EXPLODE
Breaks a block, polyline, dimension or hatch into simpler objects (alias X).
Usage: Select objects, Enter.
Options: none.
Example: EXPLODE, click a block, Enter.

## LENGTHEN
Changes the length of a line or arc (alias LEN).
Usage: Choose a mode, give the value, then click the end to change.
Options: DElta, Percent, Total, DYnamic.
Example: LENGTHEN DE 20, click near the end of a line.

## ALIGN
Moves, rotates and scales objects to match points on another object (alias AL).
Usage: Select objects, Enter, then pick source and destination point pairs.
Options: scale objects after answering Yes.
Example: ALIGN, select, Enter, source 1, destination 1, source 2, destination 2, Enter.

## PEDIT
Edits a polyline (alias PE).
Usage: Select a polyline (or a line or arc, to turn it into one), then choose an option.
Options: Close or Open, Join, Width, Edit vertex, Fit, Spline, Decurve, Ltype gen, Reverse, Undo.
Example: PEDIT, click a line, Yes, Join, select the touching lines, Enter.

## MATCHPROP
Copies layer, colour, linetype and other properties from one object to others (aliases MA, MATCH, PAINTER).
Usage: Select the source object, then click each object to change; Enter ends.
Options: Settings.
Example: MATCHPROP, click the source, click the targets.

## TEXT
Writes one line of text.
Usage: Pick the start point, give the height and rotation, type the text, press Enter twice to finish.
Options: Justify, Style.
Example: TEXT 0,0 5 0 Hello Enter Enter.

## MTEXT
Writes a paragraph of text in a box with a formatting toolbar (aliases MT, T).
Usage: Pick two corners of the text box, then type; click outside the box or press Ctrl+Enter to finish.
Options: Height, Justify, Line spacing, Rotation, Style, Width.
Example: MTEXT, click two corners, type the note.

## DIMLINEAR
Dimensions a horizontal or vertical distance (alias DLI).
Usage: Pick two points (or press Enter and click an object), then the position of the dimension line.
Options: Mtext, Text, Angle, Horizontal, Vertical, Rotated.
Example: DIMLINEAR, click two corners, click above them.

## DIMSTYLE
Opens the dimension style manager (alias D).
Usage: Choose a style or create a new one; set arrows, text, lines and units.
Options: Save, Restore, Status, Variables, Apply, ?.
Example: DIMSTYLE, select ISO-25, Modify.

## LAYER
Opens the Layer Properties panel (alias LA).
Usage: Create, rename, colour, freeze, lock and set the current layer. -LAYER does the same from the command line.
Options: for -LAYER: Make, Set, New, ON, OFF, Color, Ltype, LWeight, Freeze, Thaw, LOck, Unlock.
Example: -LAYER M WALLS Enter makes WALLS and sets it current.

## BLOCK
Turns objects into a named block (alias B).
Usage: Enter a name, pick a base point, select objects, Enter.
Options: -BLOCK works from the command line.
Example: BLOCK DOOR 0,0, select, Enter.

## INSERT
Places a block or a drawing file in the drawing (alias I).
Usage: Choose the block, pick the insertion point; the scale and rotation can be typed first.
Options: Scale, X, Y, Rotate, Multiple, Basepoint.
Example: INSERT DOOR 100,100 1 1 0.

## WBLOCK
Writes objects or a block to a separate drawing file (alias W).
Usage: Choose the source (block, whole drawing or selected objects), the base point and the file name.
Options: -WBLOCK works from the command line.
Example: WBLOCK, choose Objects, select, name the file.

## ZOOM
Changes the view (alias Z). Works while another command runs ('ZOOM).
Usage: Type an option or drag a window.
Options: All, Extents, Window, Previous, Scale, Center, Object, Dynamic, In, Out.
Example: ZOOM E shows everything. The mouse wheel zooms too.

## PAN
Moves the view (alias P). Works while another command runs ('PAN).
Usage: Pick two points, or hold the middle mouse button and drag.
Options: none.
Example: PAN, click, click.

## DIST
Measures the distance and angle between two points (alias DI).
Usage: Pick two points. The result appears in the command line.
Options: Multiple points.
Example: DIST 0,0 30,40 reports 50.

## AREA
Measures the area and perimeter (alias AA).
Usage: Pick points around the shape, or choose Object and click a closed object.
Options: Object, Add area, Subtract area.
Example: AREA O, click a closed polyline.

## LIST
Lists the data of selected objects (alias LI).
Usage: Select objects, Enter. The text window (F2) shows the result.
Options: none.
Example: LIST, click a circle.

## OSNAP
Chooses the object snap modes (alias OS).
Usage: Tick the modes in the dialog or type them. F3 turns object snap on and off; Shift+right-click opens a one-time menu.
Options: Endpoint, Midpoint, Center, Node, Quadrant, Intersection, Extension, Insertion, Perpendicular, Tangent, Nearest.
Example: OSNAP END,MID,CEN.

## UNDO
Reverses the last action (alias U, Ctrl+Z). REDO (Ctrl+Y) repeats what was undone.
Usage: Type U or press Ctrl+Z. UNDO with a number goes back several steps.
Options: Number, Auto, Control, BEgin, End, Mark, Back.
Example: UNDO 3 Enter.

## QSAVE
Saves the drawing (Ctrl+S). SAVEAS asks for a new name; the file format can be .tbcad or .dxf.
Usage: Type QSAVE or press Ctrl+S.
Options: none.
Example: SAVEAS, choose a file name.

## PLOT
Prints or exports a layout to a printer or PDF (Ctrl+P).
Usage: Choose the layout, paper size, scale, plot style table and output; Preview, then OK.
Options: Preview, Apply to Layout, plot area (Layout, Extents, Display, Window).
Example: PLOT, choose PDF, OK.

## PURGE
Removes unused layers, blocks, linetypes, text styles and dimension styles (alias PU).
Usage: Run the command and confirm what to remove.
Options: Blocks, Layers, Linetypes, Styles, Nested items, All.
Example: PURGE All Enter.
