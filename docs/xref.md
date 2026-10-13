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

## Commands

| Command | Alias | What it does |
|---|---|---|
| `PDFATTACH` | `PA` | Attaches a PDF page as a faded vector underlay (`docs/underlay.md`). |
| `XATTACH` | `XA` | Asks for a `.tbcad`, `.dxf` or `.pdf` file and a name (default: the file name), attaches it and inserts it (point, `Scale`, `Rotate`; Enter = origin). Esc undoes the attach. |
| `XRELOAD` | `XR` | Reloads one xref by name or `All` (default). Inserts stay where they are. One undo step. |
| `XDETACH` | | Removes the xref: its inserts, block, nested blocks and the layers nothing uses. |
| `XBIND` | `XB` | Makes the xref an ordinary block (the path is dropped). Its layers and blocks stay as they are. |
| `XLIST` | | Lists xrefs with path, state (up to date / changed on disk / file not found) and use count. |

Relative paths are taken from the folder of the drawing (the host setting `document.file`); an
unsaved drawing uses the current folder. The stored path is the text the user typed.

## Reload when the file changes

`AppCore::tickXrefs` (called every frame from `updateFrame`) checks all xrefs every two seconds.
When a file's stamp differs from the snapshot and no command is running, it runs `XRELOAD`. A
file that fails to load (half written, damaged) is not retried until its stamp changes again.

## File format

The xref fields travel in the `XRFS` chunk added in format 1.2 (`docs/file-format.md`); older
readers keep it as an unknown chunk and still show the snapshot.

## Not done

* Overlay mode, clipping boundaries, nested xrefs staying live (an xref inside the source is
  copied as plain content), and path types other than "as typed".
* DXF export writes an xref as an ordinary block (no `XREF` flag in group 70).
