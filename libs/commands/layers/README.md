# Layers, sub-layers, linetypes and properties (W4-3)

Command module `layers`. Registers through `module.cmake`; nothing outside this folder (and
`assets/linetypes/tbcad.lintypes`) is edited.

## Commands

| Command | Alias | What it does |
|---|---|---|
| `LAYER` | `LA` | Asks the application to open the Layers panel (sets host setting `ui.showPanel` to `layers`). |
| `-LAYER` | `-LA` | Command-line layer editing. Options: `New`, `NewSub`, `Parent`, `Set`, `On`, `Off`, `Freeze`, `Thaw`, `Lock`, `Unlock`, `Color`, `Linetype`, `Lineweight`, `Plot`, `NoPlot`, `Rename`, `Delete`, `Isolate`, `ShowAll`, `List`. |
| `LAYERCURRENT` | `LAYCUR` | Makes the layer of a picked object current. |
| `-PROP` | `-PR` | Changes properties of the selection: `Only <type>`, `Layer`, `Color`, `Linetype`, `LTScale`, `Lineweight`, `Set <field> <value>`. The whole command is one undo step. |
| `-LINETYPE` | `-LT` | `Load <names or *>`, `File <path>`, `List`. |
| `-LAYERSTATE` | `-LAS` | `Save`, `Restore` (all, or only visibility / lock / plot / look), `Delete`, `Rename`, `List` named layer states. |
| `-LAYERFILTER` | `-LAF` | `Apply` an expression, `Save` / `Use` / `Delete` named filters, `Clear`, `List`. |
| `-CURRENT` | `-CUR` | Sets the colour, linetype and lineweight new objects get (stored in the document header; the draw commands apply them). |

Layers are named by path (`S-FLOOR~edge`), by bare name when only one sub-layer has it, or by
`#<id>` (what the panels send). Lists use commas, `*` means every layer. A sub-layer starts
with colour, linetype and lineweight `ByParent`.

Panels are driven by these commands (they run `-LAYER ...` / `-PROP ...` lines through
`PanelContext::runCommand`), so every panel edit is one undo step and appears in the command
history.

## Sub-layer rules

Implemented in `layer_ops` on top of core's `Layer::parent` / `Document::effective`:

- on/off, freeze, lock and no-plot are inherited along the branch; each child keeps its own switch;
- colour, linetype, lineweight follow `ByParent` up the chain;
- depth at most 8, no cycles, names unique among siblings, layer 0 takes no children;
- delete: a layer with sub-layers is deleted as a branch (every layer in it empty) or with its
  sub-layers moved up one level (they keep how they looked, `ByParent` values become fixed);
- a layer that holds objects, is current, or is layer 0 cannot be deleted; the message says why;
- moving a layer to the top level fixes its `ByParent` values to what it showed.

## Linetypes

`assets/linetypes/tbcad.lintypes` is our own file, written from scratch (format
`name ; description ; pattern in mm`, + dash, - gap, 0 dot). It is compiled into the library, and
`-LINETYPE File` reads any file of the same format.

## Layer states and filters (Q32)

Both live in the document header (`Header::custom`), so they are saved in `.tbcad` and each change
is one undo step:

- `tbcad.layerstate.<name>`: one line per layer (path, on/frozen/locked/plot, colour, linetype,
  lineweight). Restoring matches layers by path, leaves layers added later alone, and says how
  many saved layers no longer exist.
- `tbcad.layerfilter.<name>`: the filter expression.

A filter expression is words separated by spaces, all of which must match: `on off frozen thawed
locked unlocked plot noplot used empty current`, `name:S-*`, `path:S-FLOOR~e*`, `branch:Walls`,
`color:red`, `linetype:Hidden`, `lineweight:0.25`, and `not:` in front of any word. `*` and `?` are
wildcards. A bare word is a name pattern. `-LAYERFILTER Apply` sets the host setting
`layers.filter`; the Layers panel follows it and also accepts expressions in its filter box (any
text with a `:`). The panel has a "Layer states..." list to restore a state and save the current one.
