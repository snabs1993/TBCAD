# Tool palettes

Tools > Tool Palettes (command `PALETTES`, aliases `TP`, `TOOLPALETTES`) shows tabs of one-click tools.
A click starts the tool's command and answers its first prompts for you; you only pick the point.
The tooltip shows exactly what a tool does.

There are three kinds of tool:

- **Block**: runs `INSERT` for a block, with a saved scale and rotation. If the tool names a
  drawing file and the open drawing does not have the block yet, the block is copied from that file
  first (the same as `CONTENTIMPORT`).
- **Hatch**: runs `HATCH` with a saved pattern, scale and angle, then you pick the area.
- **Command**: runs any command line; several lines are separated by `|`, for example
  `ZOOM|Extents`.

Every tool can also name a **layer**; it becomes the current layer before the tool starts.

## Editing
- `+ Add tool` at the bottom of a tab makes a new tool.
- Right-click a tool to move it left or right, or to remove it.
- Right-click a tab to rename or delete the palette; the `+` tab makes a new palette.
- In the content browser, select blocks and press **Add to palette** to turn them into tools.

Palettes are saved in `palettes.json` in the settings folder. The first start offers a Draw and a
Hatches palette. `-PALETTE` edits them from the command line (New, Rename, Delete, AddBlock,
AddHatch, AddCommand, Remove, List).
