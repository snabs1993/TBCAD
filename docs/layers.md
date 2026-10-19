
## LAYERP

`LAYERP` puts the layer settings (on, frozen, locked, plot, colour, linetype, lineweight and transparency) back to what they were before the last command that changed them, such as an isolate or a freeze. It does not bring back deleted or renamed layers. The earlier state is kept in the drawing, so it survives save and load.

`LAYCHANGE` moves the selected objects to a layer you name (like CHPROP Layer, but asks for the layer after the selection).

## Properties panel and -PROP

The Properties panel (and `-PROP Set <field> <value>` on the command line) shows the General rows of the selection and a Geometry section for the kind in play. As in AutoCAD:

- **Polyline**: *Current vertex* steps through the vertices (it wraps around); *Vertex X*, *Vertex Y*, *Start segment width* and *End segment width* edit that vertex. On the command line the vertex follows an `@`, counted from 1: `-PROP Set vertexy@3 25`. Setting a segment width on a polyline with a global width turns the global width into per-segment widths.
- **Block reference**: one row per attribute, named by its tag (`-PROP Set att:ROOM 102`).
- **Dimension**: *Measurement* (read-only), *Text override*, *Dim style*, text position and text rotation. **Leader** and **Multileader**: contents and style; a multileader also has its landing point, text height or block scale and rotation.
- **Table**: position, rotation, rows, columns, table width and height (read-only), text height, cell margin and text style.
- **Hatch**: the number of boundaries and the area (read-only); pattern, scale, angle and origin are in the Hatch section below.

With several objects of one kind selected, a row whose value differs between them shows *VARIES*, as in AutoCAD.
