# Plot style tables

A plot style table decides how each palette colour is plotted: its plotted colour, lineweight and
screening (lighter tint). Objects drawn in true colours are not affected. Library: `libs/plotstyle`.

- **Default table:** `monochrome.ctb` (everything black). Other built-in tables: `grayscale.ctb`,
  `screening-25.ctb`, `screening-50.ctb`, `screening-75.ctb`, `acad.ctb` (as drawn). `none` plots
  as drawn.
- **Where:** setting `plot.style`, the `Style` option of `PLOT` / `EXPORTPDF` / `EXPORTSVG`, the Style
  drop-down of the Plot panel, or `PLOTSTYLE` (Select, List, Import, Edit, Save).
- **User tables** live in the `plot styles` folder next to the settings and use our text format
  (`.tbstyle`, see `plotstyle.h`). `PLOTSTYLE Import` converts a `.ctb` file into one.
- **Editor:** `PLOTSTYLE Edit` (or File / Plot Style Tables...) opens a panel with all 255 colours;
  built-in tables are read-only, so edit and Save under a new name.
- **Order:** the table is applied first; the plot's colour mode (Monochrome or Greyscale) still wins
  over it.

## .ctb import

Only colour-dependent tables (`.ctb`) are read; named-style tables (`.stb`) are refused with a clear
message. The file is a short header, three words and a zlib stream of brace-structured text; entry k
of `plot_style` is palette colour k+1, `color=-1` means "use object colour" and other values carry
R, G, B in the low 24 bits. Line end, join and fill styles are not applied when plotting.
TBCAD's palette is its own, so a CTB colour index lines up with ours only for the shared 1 to 9.
