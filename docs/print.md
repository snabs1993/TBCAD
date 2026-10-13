# Printing

`PRINTER` (alias `PRN`) sends the sheet described by the plot settings (paper, scale, area, layout, colours) to a Windows printer.

- **Dialog** (default) opens the system print dialog for the printer and copies.
- **Direct** prints straight to the printer chosen with **Printer** (empty = default printer).
- **List** shows the installed printers. **Copies** sets the number of copies.

`PLOT`/`PRINT` still make PDF and SVG files. Linux builds have no printing backend and say so.

Design: `libs/print` draws a `PlotSheet` onto a device `Canvas` (tested on every system); the Windows backend implements the canvas with GDI, sets each page's paper size and orientation from the sheet, and opens `PrintDlg`. Needs a visual check on a real printer (Q17 style).

## Sheet lists on a printer (PUBLISH)

`PLOTSET` / `BATCHPLOT` / `PUBLISH` write a multi-page PDF from a sheet list. Answer the output file
prompt with `Printer` to send every sheet to the printer set with `PRINTER` (direct), or `Dialog` to
choose in the system print dialog. Each layout (and model space) with a [page setup](pagesetup.md)
is plotted with its own setup, so one job can mix colour and monochrome sheets and different style
tables.

The Plot panel has the same printer choice, copies and **Print** / **Print dialog...** buttons.
