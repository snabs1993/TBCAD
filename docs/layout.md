# Layouts, viewports and sheet lists

## Layouts (`libs/layout`, commands in `libs/commands/layout`)

A drawing can hold named layouts. A layout has a paper size and orientation, a sheet content block
(title block text, borders) and viewports. Layouts are stored in `Header::custom["tbcad.layouts"]`,
so they are saved in the `.tbcad` file and changes are undoable.

| Command | What it does |
| --- | --- |
| `LAYOUT` (`LO`) | New, Copy, Delete, Rename, Paper, Set (current), List, Template (bring a layout with its sheet objects and viewports from a .tbcad or .dxf file) |
| `MVIEW` (`MV`) | Adds a viewport: two sheet corners in mm, scale `1:N`, model centre (Enter = the middle of the drawing) |
| `VPSCALE` | Changes a viewport's scale |
| `VPMOVE`, `VPRESIZE`, `VPCENTER`, `VPDELETE` | Move a viewport on the sheet, give it new corners, change the model point it shows, or delete it |
| `VPCLIP` (`VC`) | Gives a viewport an outline of any shape: **Object** (a closed polyline on the sheet), **Polygonal** (pick points, Enter or Close to finish) or **Delete** (back to a rectangle). The rectangle becomes the bounding box, so scale and view centre keep their meaning; moving or resizing the viewport carries the outline. Plots (PDF, SVG, PNG/JPEG, printer) and a double click follow the outline |
| `VPLAYER` | Freezes or thaws a layer in one viewport (sub-layers follow their parent) |
| `VPLEVEL` | Shows one level only in a viewport (empty name = every visible level again); plots follow it |
| `TITLEBLOCK` | Inserts the `TITLEBLOCK` block (frame, labels and PROJECT, DRAWN BY, DATE, SCALE, SHEET attributes) on the sheet; edit the values with the attribute commands |

Every viewport command asks "Click inside a viewport, or type its number".

A viewport plots the model through its own scale, clipped to its rectangle.

## Drawing on the sheet

With a layout tab open you draw on the sheet itself: LINE, CIRCLE, RECTANG, TEXT, dimensions and the other
drawing commands put their objects on the sheet, in millimetres of paper (text defaults to 2.5 mm). Click or
window-select sheet objects to change them with MOVE, COPY, ERASE and the other editing commands; snaps work
on sheet geometry. Double-click an empty part of a viewport to edit the model through it.

## Plotting a layout

`PLOT`, `EXPORTPDF`, `EXPORTSVG`, `EXPORTPNG` and `EXPORTJPG` have a **Layout** option (and the Plot panel a "Plot" drop-down with live preview). Give a layout name to plot that sheet
at 1:1 on its paper; leave it empty to plot model space (the setting is `plot.layout`).

A layout can have a named page setup (paper plus plot options), see [page setups](pagesetup.md).

## Sheet lists (`libs/sheets`)

`PLOTSET` (aliases `BATCHPLOT`, `PUBLISH`) reads a text file and writes one multi-page PDF, one page per line:

```
# comment
plans/ground.tbcad | A1 Plan
plans/roof.tbcad
```

A line is `file` or `file | layout`. Relative paths are relative to the list file. A line without a
layout plots model space with the current plot settings. If any sheet fails (missing file, unknown
layout) nothing is written and the message names the entry.

## Clipped viewports

The outline of a non-rectangular viewport is stored with the viewport in the layout table (field 13,
`x:y` pairs). A DXF from another program is read through the `VIEWPORT` clip boundary (group 340
pointing at a closed `LWPOLYLINE` on the sheet, flag 16384); the polyline is kept as an ordinary
sheet object (it is on the layer the other program gave it, usually `Defpoints`). TBCAD writes the
outline into its own XDATA on the `VIEWPORT` and a warning says other programs show the rectangle.
