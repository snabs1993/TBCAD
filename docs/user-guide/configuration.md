# Configuration

TBCAD keeps user files in `%APPDATA%\TBCAD`. Set the environment variable `TBCAD_CONFIG_DIR` to
use another folder (settings, aliases and key bindings) and `TBCAD_DATA_DIR` for data.

## aliases.json

A flat JSON object. Each key is the new alias, each value is the command it runs:

```json
{
  "L": "LINE",
  "CC": "CIRCLE",
  "XL": "LINE"
}
```

Aliases are case-insensitive. An alias that is not a valid name, or points to an unknown command,
is skipped and reported on the command line. After editing the file run `ALIASES` to
reload it.

### Moving aliases from another CAD program

Settings > Aliases has Import and Export buttons, and the commands `ALIASIMPORT` and
`ALIASEXPORT` do the same from the command line. The file is in `acad.pgp` style, one alias per
line: `L, *LINE`. Lines starting with `;` are comments and the `[External Commands]` section is
ignored. Aliases for commands TBCAD does not have, and aliases that already exist, are skipped and
listed. `ALIASEXPORT` writes every alias in effect so you can share the list.

## keybindings.json

A flat JSON object mapping an action id to a key chord. An empty string removes the binding:

```json
{
  "view.grid": "F7",
  "view.zoomExtents": "",
  "edit.undo": "Ctrl+Z"
}
```

Chords are written `Ctrl+Shift+Key`. A bad chord, an unknown action or two actions on one chord produce a warning, never a failed
start. Delete the file to return to the defaults.

To share a set of shortcuts, use Settings > Keybindings > Export and Import, or the commands
`KEYSEXPORT` and `KEYSIMPORT`. The exported file is this same `keybindings.json`; an import that
cannot be read changes nothing.

## settings

Drafting options (`drafting.osnap.modes`, `drafting.osnap.aperture`, `drafting.snap.spacing`,
`drafting.grid.spacing`, `drafting.polar.increment` and so on) are saved automatically in
`settings.json` in the same folder.

## Workspaces

A workspace is a saved window layout: which panels are shown and where they are docked.
TBCAD ships three: **Drafting** (layers and properties), **Annotation** (adds the text, text style
and dimension style panels) and **Minimal** (only the drawing and the command line). Pick one from
the box at the right of the status bar or from View > Workspaces. Arrange the window the way you like,
then View > Workspaces > Save Current Layout As to keep it under your own name. The `WORKSPACE`
command (alias `WS`) does the same from the keyboard: Load, Save, Rename, Delete or List.

Your workspaces are `.json` files in the `workspaces` folder next to `settings.json`. The
workspace you used last is restored on start.
