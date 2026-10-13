# Image export (PNG and JPEG)

`EXPORTPNG` and `EXPORTJPG` (alias `EXPORTJPEG`) plot the drawing to a picture, with the same options
as `PLOT` (paper, orientation, area, scale, colour, plot style table, layout) plus **Resolution**.
File > Export PNG / Export JPEG and the buttons of the Plot panel do the same.

- **Resolution:** 10 to 2400 dpi (setting `plot.dpi`, default 300). The pixel size is the paper size
  at that resolution, so A4 at 300 dpi is 3508 x 2480 pixels. Output over 200 million pixels is
  refused with a hint to lower the resolution.
- **PNG:** 8-bit truecolour, white paper, the resolution is stored in the file (pHYs chunk).
- **JPEG:** baseline, 4:4:4 (no colour subsampling, so thin coloured lines stay crisp), quality from
  setting `plot.jpegQuality` (1 to 100, default 90), JFIF density set. Use PNG for line drawings:
  JPEG blurs thin lines.
- Lines, arcs and fills are drawn anti-aliased by a software rasteriser in `libs/export`
  (`rasterize`, `pngBytes`, `jpegBytes`, `writePng`, `writeJpeg`); no extra dependency beyond the
  miniz that was already linked for plot style tables. The image shows exactly what the PDF shows:
  lineweights are real paper widths, dashes, hatches and layout viewports included.
