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
| `MVSETUP` | One-step sheet setup: layout name, paper, orientation, scale (or Fit) and optional title block; makes the layout with one viewport inside the margins |
| `EXPORTLAYOUT` | Writes the active layout as a new model-space drawing in millimetres (.tbcad, or .dxf by file name): every viewport's visible model objects scaled and placed as on the sheet (objects partly in a viewport are kept whole, not cut), then the sheet's own objects |

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

## Sheet lists and PUBLISH (`libs/sheets`)

`PUBLISH` (alias `SHEETLIST`, menu File > Publish...) opens the Publish dialog: a list of sheets
from several drawings that is plotted into one multi-page PDF.

- **Add current drawing's sheets** adds model space and every layout of the open (saved) drawing;
  **Add drawing** does the same for a `.tbcad` or `.dxf` file typed in the box.
- Each row has a **Page setup** drop-down: the layout's own setup (default) or one of the page
  setups of the open drawing, which then overrides it for that sheet only.
- **Remove**, **Move up**, **Move down** and **Clear** edit the list; **Save list** / **Load list**
  write and read it as a `.tbdsd` text file (below), like AutoCAD's `.dsd`.
- **Layer information** and **Plot stamp** are the same switches as in the Plot panel.
- **Publish to PDF** writes the list and runs `-PUBLISH` with it, so the result shows on the command line.

`-PUBLISH` (aliases `PLOTSET`, `BATCHPLOT`) is the typed form: it asks for the list file and the
output PDF (or `Printer` / `Dialog`, see [printing](print.md)). The list file:

```
# comment
plans/ground.tbcad | A1 Plan
plans/roof.tbcad
plans/roof.tbcad | Model | A3 mono
```

A line is `file`, `file | layout` or `file | layout | page setup`. Relative paths are relative to the
list file. `Model` (or an empty layout) is model space, plotted with the current plot settings unless
it has a page setup. A page setup named on the line is looked up in that sheet's drawing, then in
the open drawing. Sheets of the open drawing are plotted as it is now, unsaved changes included;
other drawings are read from their files (`.tbcad` or `.dxf`). Each sheet gets the plot stamp with
its own file and layout name. If any sheet fails (missing file, unknown layout or page setup)
nothing is written and the message names the entry.

## Clipped viewports

The outline of a non-rectangular viewport is stored with the viewport in the layout table (field 13,
`x:y` pairs). A DXF from another program is read through the `VIEWPORT` clip boundary (group 340
pointing at a closed `LWPOLYLINE` on the sheet, flag 16384); the polyline is kept as an ordinary
sheet object (it is on the layer the other program gave it, usually `Defpoints`). TBCAD writes the
outline into its own XDATA on the `VIEWPORT` and a warning says other programs show the rectangle.
