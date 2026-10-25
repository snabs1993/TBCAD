# TBCAD sample drawings

`thai-house.tbcad` is a two-storey Thai house plan to try the program on. The same drawing is in
`thai-house.dxf` (R2000, opens in other CAD programs) and the A3 sheet is in `thai-house-A3.pdf`.

What it shows:

- **Levels:** `ชั้น 1`, `ชั้น 2` and `หลังคา` hold the three plans at the same position. Switch with the
  Levels panel (Ctrl+Shift+E) or Ctrl+Alt+PageUp / Ctrl+Alt+PageDown.
- **Sub-layers:** `A-WALL` has `exterior` and `interior`, `A-FURN` has `bed` and `living`.
- **Annotative text and dimensions:** the annotation scale is 1:100; the dimensions and room names
  keep the same size on paper at any scale (`CANNOSCALE`).
- **Hatches:** wall section fill, wood and tile floors, roof.
- **Thai text** in the Sarabun font, and **blocks** (doors, windows, bed, sofa, table, WC).
- **A layout** (`A3 1-100`) with a title block and one viewport per floor, each showing its own
  level (`VPLEVEL`). Open it with the `LAYOUT` command and plot with `PLOT`.

The files are made by `tbcad-samples`, so they can be rebuilt: `tbcad-samples samples`
(source: `apps/tbcad-samples`).
