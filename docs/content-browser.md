# Content browser

Tools > Content Browser (command `CONTENT`, aliases `ADC`, `ADCENTER`, `DC`, `DCENTER`, the
DesignCenter names; `ADCCLOSE` closes it) opens a panel that looks inside other drawings
and copies single items into the one you are working on: blocks, layers (with their sub-layers),
linetypes, text styles, dimension styles, multileader styles, table styles and multiline styles.

1. Pick a folder with the path box (type a path and press Enter; double-click a folder to enter it).
   Drawings (`.tbcad`, `.dxf`, `.tbt`) are listed below. A drawing's full path typed in the box
   opens it at once, and **History** lists the drawings browsed lately (here or in the Other
   Drawing tab of the Blocks palette).
2. Click a drawing. Its contents appear in tabs on the right; blocks show a small preview.
3. Select items (Ctrl+click for several) and press **Copy to drawing**. **Copy and insert** copies
   a block and starts `INSERT` with it.

What comes along: a block brings the layers, linetypes and text styles its objects use, and the
blocks it contains (named `content$<block>`). A dimension style brings its text style, a
multileader style the dimension style that holds its arrow and text settings, and a table style
its text style. Layers and
styles are matched by name; one the drawing already has is left alone and reported as skipped, so
nothing is overwritten. A block whose name is taken is skipped. Each copy is one undo step.

## From the command line
`CONTENTIMPORT` (alias `CIMP`) asks for the file, then what to copy:

    CONTENTIMPORT
    D:\library\doors.tbcad
    Block
    door-single|door-double      (names separated by |, or * for all)

The kinds are Block, Layer, Linetype, Textstyle, Dimstyle, Mleaderstyle, TAblestyle and MLStyle.
Choose `List` instead of a kind to see what the file offers.

## Drag and drop

Drag a block, layer, linetype or style from the list onto the drawing area to copy it into the drawing; a dragged block is also inserted where it is dropped. Drag blocks onto the "Drop blocks here" bar of a palette tab to add them as tools. If several rows are selected, dragging one of them takes all of them.
