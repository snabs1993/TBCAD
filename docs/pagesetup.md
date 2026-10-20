# Page setups

A page setup is a named bundle of plot options saved **in the drawing**: paper, orientation, plot
area, scale, offset, margin, lineweights, colour mode and the plot style table. Setups are stored in
`Header::custom["tbcad.pagesetups"]` (code: `libs/layout/pagesetup.*`), so they travel with the
`.tbcad` file and can be undone.

## PAGESETUP

File > Page Setup... or `PAGESETUP`:

| Option | What it does |
| --- | --- |
| List | Shows the setups and which layouts (or model space) use them |
| Save | Saves the current plot options under a name; the same name replaces |
| Apply | Makes a setup the current plot options (PLOT, EXPORT*, the Plot panel) |
| Assign | Gives a setup to a layout (or `Model` for model space); an empty setup name removes it. The layout takes the setup's paper and orientation |
| Rename / Delete | Layouts that used it follow the rename or lose it on delete |

## Where setups apply

- Choosing a layout in the Plot panel or the `Layout` option of `PLOT` applies the layout's setup to
  the plot options (the command line says so).
- `PLOTSET` / `BATCHPLOT` plots every listed layout, and model space, with its own setup when it has
  one; sheets without one use the current plot options.
- Layout sheets are always plotted at 1:1 on their own paper, so for a layout the setup's area,
  scale and offset do not matter; lineweights, colour mode and the style table do.

The Plot panel has a **Page setup** drop-down for the plotted target and a field to save the current
options as a new setup.

## Plot stamp

`Stamp` in `PLOT` / `EXPORT*` / `PRINTER` (and the Plot panel checkbox) adds one line of text in the
lower-left corner of the printable area: the settings `plot.stamp` and `plot.stampText`. Tokens:
`%file%` `%layout%` `%date%` `%time%` `%scale%` `%paper%`. The stamp uses the same fonts as drawing
text, so Thai works, and it is not counted among the plotted objects. `PLOTSTAMP` switches it
(`On` / `Off`) and sets its text (`Text`). In a sheet list every sheet is stamped with its own file
and layout name.

## Layer information in PDFs

A PDF lists the plotted layers (optional content groups) so a PDF viewer can switch them, as
AutoCAD's "include layer information" does. The setting `plot.pdfLayers` (default on) turns it off
for a flat PDF: `PdfLayers` in `PLOT` / `EXPORTPDF`, the Plot panel checkbox **Include layer
information (PDF)** and the Publish dialog's **Layer information**.
