# External references (xref)

An xref shows another drawing inside the current one without copying it by hand, and follows that
file when it changes. Library: `libs/xref`; commands in `libs/commands/blocks`.

## What it is

An xref is a **block** whose definition has `BlockDef::xrefPath` set (the path as typed) and
`xrefStamp` (the source file's last-write time when the block was filled). The block's entities
are a **snapshot** of the source's model space, stored in the drawing file like any block, so the
drawing opens, plots and exports DXF without the source. Inserts of the block are ordinary
`Insert` entities (move, copy, rotate, scale, array, explode all work).

Everything the source brings is renamed so it cannot clash with the host:

* **Layers**: a top-level layer named like the xref, with the source's layer tree below it. The
  source layer `walls` appears as the sub-layer `site~walls` of an xref called `site`; its own
  sub-layers keep their structure. The source's layer `0` becomes `site~0`. Turn the `site` layer
  off, freeze or lock it, and the whole xref follows (sub-layer inheritance).
* **Blocks**: blocks used by the source are copied as `site$<name>` (a numbered suffix is added
  when that name is taken).
* **Linetypes, text styles, dimension styles**: matched by name; missing ones are added.

## Opening a DXF or DWG

A DXF or DWG file only names its xrefs (the block is empty in the file). When such a drawing is
opened, and in `tbcad-convert` unless `--no-xrefs` is given, every xref whose file is found is
loaded into its block (`xref::loadOnOpen`), nested xrefs included. The file is looked for where
the stored path points and then, as AutoCAD does, by its name in the drawing's own folder, so a
project copied from another disk still finds its xrefs; the stored path is kept as it was. An
xref whose file is not found stays an empty, unloaded block and is listed in the missing-xref
notice; XREF > Reload loads it once the file is there.
The host file's own rows for the xref's layers (`site|walls`, where AutoCAD keeps the host's
colour, on/off, frozen, locked, plot, line weight and linetype for them) are applied to the loaded
sub-layers (`site~walls`), and those rows are removed when nothing is drawn on them.

## Commands

| Command | Alias | What it does |
|---|---|---|
| `PDFATTACH` | `PA` | Attaches a PDF page as a faded vector underlay (`docs/underlay.md`). |
| `IMAGEATTACH` | `IAT` | Attaches a PNG, JPEG or BMP picture as a faded underlay (`docs/underlay.md`). |
| `XATTACH` | `XA` | Asks for a `.tbcad`, `.dxf` (or, when the reader is available, `.dwg`) file, then **Attach/Overlay** and the **path type** (Relative / Absolute / None), then a name (default: the file name); attaches it and inserts it (point, `Scale`, `Rotate`; Enter = origin). Esc undoes the attach. |
| `XRELOAD` | `XR` | Reloads one xref by name or `All` (default). Inserts stay where they are. One undo step. |
| `XDETACH` | | Removes the xref: its inserts, block, nested blocks and the layers nothing uses. |
| `XUNLOAD` | | Drops the content of the xref but keeps its block, path and inserts and the layer states; `XRELOAD` brings it back. Shown as "unloaded" in `XLIST`. |
| `XBIND` | `XB` | Makes the xref an ordinary block (the path is dropped). **Bind** keeps its layers (`site~walls`) and nested blocks (`site$door`); **Insert** merges the layers into the drawing's own by name (`site~walls` becomes `walls`, sub-layers move below it, `site~0` becomes layer 0, the look is kept) and removes the `site$` prefix from nested blocks when the plain name is free. One undo step. |
| `XPATH` | | Re-stores the path of an xref as **Relative**, **Absolute** or **None** (file name only) without moving the file. |
| `XRELINK` | | Points an xref (typically one whose file was not found) at another file, checks it can be read, and reloads it. A file that cannot be read changes nothing. |
| `XLIST` | | Lists xrefs with path, state (up to date / changed on disk / file not found) and use count. |

Relative paths are taken from the folder of the drawing (the host setting `document.file`); an
unsaved drawing uses the current folder. The stored path is the text the user typed.

## Attach or overlay

An **attached** xref travels with its drawing: if drawing B attaches A and drawing C attaches B, C
shows A too. An **overlay** xref is shown in the drawing that holds it but is left out when that
drawing is used as an xref (so two drawings can overlay each other without a loop). `XLIST` marks
overlays. The flag is stored in the `XRFO` chunk (format 1.5).

## Paths

The path stored with the xref is chosen when attaching: **Relative** (from the folder of the
drawing, forward slashes; the full path when the drawing is not saved yet or the file is on another
drive), **Absolute**, or **None** (only the file name, looked up next to the drawing).

## Nested xrefs

Loading a file (attach or reload) first refreshes the xrefs inside it from their own files, with
paths taken from that file's folder, so a change deep in a chain reaches the top. A nested xref
that cannot be read keeps the snapshot saved in its parent. A loop (A attaches B attaches A) is cut
where it closes. Overlays inside a loaded file are left out.

## Reload when the file changes

`AppCore::tickXrefs` (called every frame from `updateFrame`) checks all xrefs every two seconds.
When a file's stamp differs from the snapshot and no command is running, it runs `XRELOAD`. A
file that fails to load (half written, damaged) is not retried until its stamp changes again.

## Layer states (VISRETAIN)

Layers of an xref are ordinary layers under the xref's top-level layer. Reload and unload never
touch layers that already exist, so what the user changed (on/off, frozen, locked, colour,
linetype, plot) is kept. Layers that appear in a newer version of the source are created with
the source's settings. This always behaves like AutoCAD's `VISRETAIN = 1`.

## Missing files

If the file cannot be found, the xref keeps its snapshot, `XLIST` says "file not found", the
automatic reload is silent, and `XRELOAD` reports the error and leaves the drawing unchanged.

## File format

The xref fields travel in the `XRFS` chunk added in format 1.2 (`docs/file-format.md`); older
readers keep it as an unknown chunk and still show the snapshot.

## Clipping (XCLIP)

`XCLIP [XC]` picks an inserted block or xref and sets **New** (rectangular or polygonal), **ON**,
**OFF** or **Delete**. The boundary is stored on the insert in the block's own coordinates
(`InsertData::clip`, file format 1.6), so it follows the insert when that is moved, turned or
scaled. Display, bounds and plots show only the inside; lines are cut exactly at the boundary,
filled areas are cut exactly for a convex boundary (a concave one may leave zero-width slivers).
Snapping and picking still see the whole block. `EXPLODE` drops the boundary and says so.

`XCLIP Invert` flips a boundary so that the **outside** shows and the inside is hidden (`InsertData::clipInvert`, kept in the same file section as bit 1 of the flags byte, and in DXF XDATA as `clipinvert=1`). Lines are cut exactly; filled areas are cut exactly for a convex boundary; the bounds of an inverted clip are the whole block. `New` and `Delete` reset the flag. For readers of other formats (the DWG SPATIAL_FILTER object): fill `InsertData::clip` (block coordinates), `clipOn` and `clipInvert`; nothing else needs to change.

## DXF

DXF has no external reference with a snapshot, so the export writes the xref as an ordinary block
(content included) with TBCAD XDATA on the `BLOCK` record (`xref=<path>`, `stamp=`, overlay flag);
TBCAD restores the xref from it. A clipping boundary is written as XDATA on the `INSERT` (`clip=1|0`
and one `1010` point per corner); other programs show the whole block and the export report says so.
An `xref` block from another program (flag 4) is still imported as an empty block with a warning.
