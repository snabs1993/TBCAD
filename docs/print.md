# Printing

`PRINTER` (alias `PRN`) sends the sheet described by the plot settings (paper, scale, area, layout, colours) to a Windows printer.

- **Dialog** (default) opens the system print dialog for the printer and copies.
- **Direct** prints straight to the printer chosen with **Printer** (empty = default printer).
- **List** shows the installed printers. **Copies** sets the number of copies.

`PLOT`/`PRINT` still make PDF and SVG files. Linux builds have no printing backend and say so.

Design: `libs/print` draws a `PlotSheet` onto a device `Canvas` (tested on every system); the Windows backend implements the canvas with GDI, sets each page's paper size and orientation from the sheet, and opens `PrintDlg`. Needs a visual check on a real printer (Q17 style).
