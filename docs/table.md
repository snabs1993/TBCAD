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
  Keywords: RowBelow, RowAbove, ColRight, ColLeft, DeleteRow, DeleteCol, Merge (then click the opposite corner
  cell), Unmerge, Width, Height. Inserting or deleting first splits any merged range that crosses the change.
  The whole session is one undo step.
- Grips: the top-left corner moves the table, the bottom-right corner rescales the grid keeping proportions.
- MOVE, COPY, ROTATE, SCALE (uniform) and EXPLODE (grid lines plus one MTEXT per filled cell) work. A table
  cannot be mirrored or scaled non-uniformly.

## Files

- `.tbcad` stores the table in the entity codec (new kind number after Proxy; older files are unaffected).
- DXF export writes an INSERT of an anonymous `*T<n>` block holding the drawn grid and texts, so other programs
  show the table, plus TBCAD extended data with the table itself. DXF import rebuilds the table from that data
  and skips the `*T` block. A table from another program (ACAD_TABLE) is not read as a table.
