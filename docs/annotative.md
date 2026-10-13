# Annotative scaling

Text, dimensions, leaders and tables can follow the drawing's annotation scale, so notes stay
readable at any plot scale.

- `ANNOTATIVE` (`ANNO`): select objects, then On or Off.
- `CANNOSCALE` (`AS`): set the current scale as N of 1:N (`100` for 1:100). Annotative objects
  are drawn N times larger in model space, so they print at the same paper size.
- `SCALELISTEDIT`: Add, Delete, Reset or List the scales on offer.
- A layout viewport uses its own scale, so one drawing can show the same note at 1:50 and 1:100
  side by side with the same paper height.

Both the current scale and the list are stored in the drawing (`.tbcad`). DXF files carry only the
per-object annotative note (in TBCAD's XDATA), so other programs see ordinary objects at the
size last drawn; the scale and the list are not written to DXF.
