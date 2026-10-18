# Annotative scaling

Text, dimensions, leaders, tables and symbols (block inserts) can be drawn at paper size (1:1)
and sized for a plot scale afterwards.

## Per object

- Select an object: the **Annotation** part of the Properties panel has an *Annotative* box and
  the scales the object is made for (*Change...* offers the scale list, or a custom N).
- `ANNOTATIVE` (`ANNO`): select objects, then `On`, `Off` or `Scales` (for example `50,100`;
  empty means "follow the drawing's current scale").
- In model space an annotative object is drawn at the **current annotation scale** when its list
  has that scale (or is empty), so text 2.5 mm high is 125 mm at 1:50 and 250 mm at 1:100. If the
  list does not have the current scale it is drawn at its **first** scale (see `ANNOALLVISIBLE`).
  In a layout viewport it is drawn at the viewport's scale, so it prints at the same paper size,
  and it is shown only in viewports whose scale is in its list.
- A symbol (block insert) has no live scaling: turning it on or changing its scale rescales the
  insert, so a north arrow drawn at 1:1 becomes 50 times larger at 1:50 and back again when
  turned off.

## Per drawing

- `CANNOSCALE` (`AS`), or the *Scale* box in the status bar: the current annotation scale, the
  default for new annotative objects and for annotative objects that list no scale of their own.
- `ANNOAUTOSCALE` (default 1): changing the annotation scale, with `CANNOSCALE`, the status bar or
  by making a layout viewport active, adds the new scale to every annotative object that lists
  scales of its own (one undo step). Objects with an empty list follow the current scale anyway;
  symbols keep their list and their first-scale size. With 0 nothing is added.
- `ANNOALLVISIBLE` (default 1): 1 shows annotative objects that do not list the current scale (at
  their first scale); 0 hides them and they cannot be picked or snapped to. Plots are not affected:
  a viewport shows an object when the viewport's scale is in its list.
- `SELECTIONANNODISPLAY` (default 1): a selected annotative object with several scales also shows
  its other scales, dimmed. Engine: `geom::annotationGhosts` gives the display of each other scale
  for the renderer; 0 turns it off. All three are set with `SETVAR` or by name and are saved with
  the drawing.
- `ANNONEW`, or the *ANNO* button in the status bar: new text, dimensions, leaders, tables and
  symbols start annotative at the current scale.
- `SCALELISTEDIT`: Add, Delete, Reset, List the scales on offer.

## Files

- `.tbcad` keeps the flag and each object's scales, the current scale and the list.
- DXF keeps the same in TBCAD's extended data (per object) and in an `XRECORD` in the root
  dictionary (current scale and list). R2007 files also get real `ACAD_SCALELIST` / `SCALE`
  objects. Other programs show the objects at the size last drawn.

## Wipeout

`WIPEOUT` (`WO`) makes a closed shape that hides what was drawn before it: pick points (Enter
closes), a `Rectangle`, or turn a closed `Polyline` (a revision cloud too) into one. `Remove`
turns it back into an ordinary polyline. It is filled with the background colour of the screen
(or the white sheet when plotted) and shows no outline; what is drawn after it stays on top.
It is saved in `.tbcad`; DXF files carry an ordinary polyline plus a TBCAD note, so other
programs show the outline only.

## Symbols

Drawn at paper size and put in as annotative blocks at the current annotation scale (Draw >
Symbols). Each asks for a point, a rotation and its attribute values; the block (`TB_...`) is
made in the drawing the first time and can be edited like any block.

| Command (alias) | Block | Attributes |
| --- | --- | --- |
| `NORTHARROW` (`NA`) | `TB_NORTH` | none |
| `GRIDBUBBLE` (`GB`) | `TB_GRID` | `ID` (grid letter or number) |
| `LEVELMARK` (`LM`) | `TB_LEVEL` | `LEVEL` (e.g. `+3.20`) |
| `SECTIONMARK` (`SM`) | `TB_SECTION` | `SECTION`, `SHEET` |
| `ELEVATIONMARK` (`EM`) | `TB_ELEVATION` | `VIEW`, `SHEET` |
| `DETAILMARK` (`DM`) | `TB_DETAIL` | `DETAIL`, `SHEET` |

Sizes follow common Thai / ISO 128 practice on the sheet: grid bubbles 8 mm across with 3.5 mm
figures, markers 10 mm across, notes 2.5 mm.

## TOLERANCE (feature control frames)

`TOLERANCE` (`TOL`) draws a geometric tolerance frame as GD&T drawings show it: a symbol cell, the
tolerance (optionally with a diameter sign and a maximum or least material modifier) and up to
three datum cells. The prompts are the characteristic (Straightness, Flatness, Circularity,
Cylindricity, ProfileLine, ProfileSurface, Angularity, Perpendicularity, Parallelism, Position,
Concentricity, Symmetry, CircularRunout, TotalRunout), the value, the diameter zone, the material
condition, the datum letters and the position of the left edge of the frame.

The frame is one annotative block insert (`TB_TOL_1`, `TB_TOL_2` ...) drawn at paper size, 5 mm
high with 2.5 mm text, scaled like the symbols above (the annotation scale, or the size a
far-away view or a large plan needs). The symbols are drawn as lines, arcs and circles, so they do
not depend on a font. EXPLODE turns the frame into plain lines and text. `gdt::frameEntities` (libs/gdt)
returns the geometry for tests and tools. The header table `tbcad.tolerances` remembers what each
frame block says, so DXF can carry it (see `docs/dxf-import.md`).
