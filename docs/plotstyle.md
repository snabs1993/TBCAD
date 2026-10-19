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

## Named plot styles (.stb)

A `.stb` table lists styles by name instead of by colour. PLOTSTYLE Import reads one (like a `.ctb`), and Select
chooses it. With a named table, each object plots with the style its own name gives, else the style its layer gives,
else `Normal` (which keeps the object's colour and lineweight when the table's Normal does). A name the table does not
have plots as drawn.

- **ASSIGNPSTYLE** (APS): give a layer or picked objects a style name. `ByLayer` makes an object follow its layer again.
- **PSTYLEMODE**: records whether the drawing was made for colour-dependent or named styles (DXF `$PSTYLEMODE`). The
  plot follows the kind of table that is chosen, so the mode is a label, not a switch.
- The names are kept in the drawing header (`tbcad.pstyle.layer:<id>`, `tbcad.pstyle.ent:<id>`), so they are saved in
  `.tbcad`, undo with the command, and copy with the drawing. They are not copied with a pasted object.
- DXF: a layer's or object's name is group 390 (with 380 = 3 on objects) pointing at an ACDBPLACEHOLDER in the
  ACAD_PLOTSTYLENAME dictionary, as AutoCAD writes it; `Normal` is the default and has no pointer. DXF files with
  `$PSTYLEMODE 0` read back the same way.
- CONVERTCTB and CONVERTPSTYLES only explain the above; no conversion is done.

## Page setups from a DXF file

PAGESETUP Import also reads `.dxf` files: every named PLOTSETTINGS object becomes a page setup (paper, orientation,
plot area, scale, centring, lineweights, plot style table). The settings a layout carries become a setup named after the layout (a named setup of the same name wins);
`*None*` setups are skipped.
