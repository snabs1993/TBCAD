# Levels

A **level** is one floor or storey of a 2D drawing. You work on one level at a time, and objects
on different levels may sit at exactly the same position without ever interfering: a wall line on
level 1 and a wall line on level 2 can have the same coordinates, and picking, snapping, hatching
and editing only see the level you are working on.

Levels are independent of layers. Every object has both a layer (what it is: walls, doors) and a
level (where it is: ground floor, first floor). Layers keep their colour, linetype and plot
settings across levels.

## Using levels

* The first level (`Level 1`) always exists. It can be renamed but not deleted.
* **Levels panel** (Format > Levels, `LEVEL`, `Ctrl+Shift+E`): New Level, Delete, Move Selection
  Here, a radio button for the level you work on, show / hide, lock, elevation (mm) and the object
  count per level. Double-click a name to rename it.
* **Status bar**: the *Level* drop-down switches the level you work on. `Ctrl+Alt+PageUp` and
  `Ctrl+Alt+PageDown` (`LEVELUP`, `LEVELDOWN`) step to the next level up or down by elevation.
* New objects always go on the current level.
* **Other levels** (panel drop-down, `-LEVEL View`):
  * *Dimmed* (default): faded reference, cannot be picked, snapped to or edited, so a ground-floor
    plan can be traced from the first floor without touching it.
  * *Hidden*: not drawn.
  * *Shown (editable)*: drawn normally and fully editable.
* A hidden level is never drawn, a locked level is drawn but read-only. The current level cannot be
  hidden. Making a hidden level current shows it.
* Deleting a level moves its objects to the first level. It is one undo step.
* Move objects to another level with `CHLEVEL` (or `-LEVEL Move`, or the panel button).
* Copy objects to another level, keeping the position, with `COPYTOLEVEL` (for example to repeat a wall layout on the next floor).

## Commands

| Command | Alias | What it does |
|---|---|---|
| `LEVEL` | `LV` | Opens the Levels panel. |
| `-LEVEL` | `-LV` | Command line: `New` (name, elevation; becomes current), `Set`, `Rename`, `Elevation`, `Show`, `Hide`, `Lock`, `Unlock`, `View` (Dim, Hide, All), `Move`, `Delete`, `List`. Levels are named by name or `#<id>`. One undo step per command. |
| `CHLEVEL` | `CHLV` | Moves the selected objects to another level. |
| `COPYTOLEVEL` | `COPYLV` | Copies the selected objects to another level at the same position. |
| `LEVELUP` / `LEVELDOWN` | `LVUP` / `LVDN` | Work on the next level up or down. |

## Files

* `.tbcad` stores levels in the `LVLS` chunk and each object's level in `ELVL`
  (`file-format.md`, format 1.7). Older programs keep both chunks and put everything on one level.
* DXF: TBCAD writes the level table and each object's level as XDATA and reads it back
  (`dxf-export.md`, `dxf-import.md`). Other programs show all objects of all levels. A DXF from
  another program opens with everything on the first level.
* Plots and PDF/SVG export draw every level that is visible, in the normal colours.
* In a layout, `VPLEVEL` makes one viewport show a single level (for example one floor per viewport on a sheet); an empty name shows every visible level again. The choice is saved with the layout and used when plotting.
* `QSELECT` and `FILTER` have a `Level` property. It also finds objects on dimmed levels, so you can select a floor by name.

## For modules

`Document::levels()`, `Document::level(id)`, `Entity::common.level`, `Header::currentLevel` and
`Header::levelView`. Use `Document::effective(entity.common)` (not `effective(layer)`) whenever
you decide whether an object is visible, selectable or editable: it applies the level on top of the
layer. `EffectiveLayer::live()` additionally excludes objects on other levels, which is what snapping
and boundary detection use. Headless operations are in `libs/commands/levels` (`level_ops.h`); the MCP
tools are `list_levels`, `create_level`, `modify_level`, `delete_level`, `set_current_level` and
`set_level_view`.
