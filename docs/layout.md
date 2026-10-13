# Layouts, viewports and sheet lists

## Layouts (`libs/layout`, commands in `libs/commands/layout`)

A drawing can hold named layouts. A layout has a paper size and orientation, a sheet content block
(title block text, borders) and viewports. Layouts are stored in `Header::custom["tbcad.layouts"]`,
so they are saved in the `.tbcad` file and changes are undoable.

| Command | What it does |
| --- | --- |
| `LAYOUT` (`LO`) | New, Delete, Rename, Paper, Set (current), List |
| `MVIEW` (`MV`) | Adds a viewport: two sheet corners in mm, scale `1:N`, model centre |
| `VPSCALE` | Changes a viewport's scale |
| `VPLAYER` | Freezes or thaws a layer in one viewport (sub-layers follow their parent) |
| `TITLEBLOCK` | Inserts the `TITLEBLOCK` block (frame, labels and PROJECT, DRAWN BY, DATE, SCALE, SHEET attributes) on the sheet; edit the values with the attribute commands |

A viewport plots the model through its own scale, clipped to its rectangle.

## Plotting a layout

`PLOT`, `EXPORTPDF` and `EXPORTSVG` have a **Layout** option (and the Plot panel a "Plot" drop-down with live preview). Give a layout name to plot that sheet
at 1:1 on its paper; leave it empty to plot model space (the setting is `plot.layout`).

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
