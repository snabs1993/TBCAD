# Tables (Q71)

Library `libs/table` (`tbcad::table`, include `tbcad/table/table.h`), command module `libs/commands/table`
(`tbcad::tablecmd`). The entity is `TableData` in core, entity kind `Table`.

## Model

A table is a grid in its own frame: x to the right, y downwards from the top-left corner `origin`, the whole
frame turned by `rotation`. `colWidths` and `rowHeights` (millimetres) size the grid; `cells` holds
`rows * columns` `TableCell`s in row-major order. A merged range lives on its top-left cell (`colSpan`,
`rowSpan`); the other cells of the range are `covered` and ignored. `titleRow` marks the first row as a
centred title. Text in a cell is TBCAD markup (plain text works) and wraps at the cell width.

## Commands

- **TABLE** (TB): columns, rows, column width, row height, optional title (a title adds a merged first row),
  then the top-left corner with a live preview.
- **TABLEEDIT** (TE): pick a table, then click a cell to type its text (Enter keeps the text, `-` clears it).
- **TABLEEXPORT**: writes a table's cells as plain text to a CSV file (UTF-8 with a byte order mark, so Excel
  reads it). **TABLEIMPORT**: makes a table from a CSV file at a picked corner; each column is as wide as
  its longest text and the text height follows the drawing size. The comma or the semicolon (Excel in some
  locales) separates the fields.
- **ATTTABLE**: makes a schedule table from the attributes of the block inserts (block name and one column
  per tag), for example a door schedule; run it again for a fresh table. **ATTEXT** writes the same data to CSV.
  Keywords: RowBelow, RowAbove, ColRight, ColLeft, DeleteRow, DeleteCol, Merge (then click the opposite corner
  cell), Unmerge, Width, Height. Inserting or deleting first splits any merged range that crosses the change.
  The whole session is one undo step.
- **Formulas**: a cell whose text starts with `=` is worked out when drawn, exported to CSV or exploded.
  Operators `+ - * / ^` and brackets; numbers; cell names (`A1`: column letters, row from 1); ranges `A1:C3`;
  functions SUM, AVERAGE (AVG), MIN, MAX, COUNT over cells, ranges and numbers, and ABS, SQRT, ROUND(x, digits).
  Text that reads as a number counts as that number, an empty cell as 0. Mistakes show `#ERR`, `#REF` (outside the
  table), `#NAME`, `#VALUE` (text where a number is needed), `#DIV/0` or `#LOOP` (a formula that needs itself).
  The formula itself is kept in the cell and in the DXF extended data; TABLEEDIT shows it when editing.
- **Alignment**: TABLEEDIT `Align` (this cell) and `AlignColumn` (the whole column) set Left, Center, Right or
  Default. It is stored as the MTEXT paragraph code `\pxql;`, `\pxqc;` or `\pxqr;` at the start of the cell text,
  so it needs no file format change; formulas, CSV export and explode look through it, and editing the cell text
  keeps it.
- **TABLESTYLE** (alias TS) opens the **Table Style** dialog: the styles with a preview, Set current, New (starting
  with the selected style), Delete, Apply to tables, and the general sizes and the Title / Header / Data cell styles
  of the selected one. Each change runs `-TABLESTYLE Modify` with the whole style, one undo step.
- **-TABLESTYLE**: the typed form. Named sets of column width, row height, text height, margin and text style, kept in the
  drawing. Keywords: Set (make current), New, Modify, Delete, Apply (give a style's sizes to tables already drawn),
  List. TABLE starts from the current style; `Standard` (30 x 8, text 2.8, margin 1) always exists.
- Grips: the top-left corner moves the table, the bottom-right corner rescales the grid keeping proportions.
- MOVE, COPY, ROTATE, SCALE (uniform) and EXPLODE (grid lines plus one MTEXT per filled cell) work. A table
  cannot be mirrored or scaled non-uniformly.

## Files

- `.tbcad` stores the table in the entity codec (new kind number after Proxy; older files are unaffected).
- DXF export writes an INSERT of an anonymous `*T<n>` block holding the drawn grid and texts, so other programs
  show the table, plus TBCAD extended data with the table itself. DXF import rebuilds the table from that data
  and skips the `*T` block. An AutoCAD table (ACAD_TABLE) is read as an editable table: row heights,
  column widths, merged cells, the text of every cell (group 1, or the CELL_VALUE text of newer files),
  the text height and style, the data-row alignment, and a title row when the first row is one merged
  cell. Borders, colours and fills then follow the TBCAD table style. A table whose cell data does not
  add up is shown from its `*T` block instead.
