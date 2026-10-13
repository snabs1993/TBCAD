# Annotative scaling

Text, dimensions, leaders, tables and symbols (block inserts) can be drawn at paper size (1:1)
and sized for a plot scale afterwards.

## Per object

- Select an object: the **Annotation** part of the Properties panel has an *Annotative* box and
  the scales the object is made for (*Change...* offers the scale list, or a custom N).
- `ANNOTATIVE` (`ANNO`): select objects, then `On`, `Off` or `Scales` (for example `50,100`;
  empty means "follow the drawing's current scale").
- An annotative object is drawn at its **first** scale in model space (text 2.5 mm high becomes
  125 mm at 1:50). In a layout viewport it is drawn at the viewport's scale, so it prints at the
  same paper size, and it is shown only in viewports whose scale is in its list.
- A symbol (block insert) has no live scaling: turning it on or changing its scale rescales the
  insert, so a north arrow drawn at 1:1 becomes 50 times larger at 1:50 and back again when
  turned off.

## Per drawing

- `CANNOSCALE` (`AS`), or the *Scale* box in the status bar: the current annotation scale, the
  default for new annotative objects and for annotative objects that list no scale of their own.
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
