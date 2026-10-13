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

## settings

Drafting options (`drafting.osnap.modes`, `drafting.osnap.aperture`, `drafting.snap.spacing`,
`drafting.grid.spacing`, `drafting.polar.increment` and so on) are saved automatically in
`settings.json` in the same folder.
