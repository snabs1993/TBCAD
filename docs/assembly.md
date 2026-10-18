# Parametric assemblies (`tbcad::blocks`, `tbcad::expr`, `block_cmds`)

An **assembly** is a block with named parameters. Every Insert of the block holds its own value for each
parameter, and formulas turn those values into the things that change inside the block: dynamic block
parameters (see `docs/blocks.md`), attribute texts, parameters of blocks nested inside it, and the rows,
columns and spacing of nested arrays. Assemblies nest: an outer assembly's formulas drive the parameters of
an assembly placed inside it.

## Parameters

| Type | Value | Limits |
| --- | --- | --- |
| `Number` | any real number | optional minimum and maximum (used when maximum > minimum) |
| `Integer` | a number rounded to a whole number | the same |
| `Text` | any text | none |
| `Choice` | one of a list of texts, the first is the default | the list |

A value outside the limits is clamped when an assembly is displayed, and refused when set with `ASMSET`,
`-ASMSET` or the MCP tool. An Insert that holds no value for a parameter uses the default.

## Formulas (`libs/expr`)

Numbers, text in double quotes, parameter names (Thai names work) and the operators `^`, unary `-`, `*`, `/`,
`+`, `-`, `&` (join as text), `< <= > >= == !=`, `&&`, `||`, `!`. Functions: `min`, `max`, `abs`,
`round` (one or two arguments), `floor`, `ceil`, `sqrt`, `pow`, `sin`, `cos`, `tan`, `len`, `if(test, a, b)`;
the constant `pi`. Angles are in radians. Names are case-insensitive.

## Bindings

A binding names a target and a formula:

| Kind | Target | Effect |
| --- | --- | --- |
| Dynamic | a dynamic parameter of this block | its value for the insert |
| Attribute | an attribute tag of this block | the attribute text |
| Nested | a parameter of an insert nested in this block | that insert's value |
| Array | `rows`, `columns`, `count`, `rowSpacing`, `columnSpacing` or `pathSpacing` of a nested insert's array | the array field |

A formula that cannot be evaluated is skipped and listed by `ASMINFO`, the MCP tool and `asmValidate`.
`ASMDEF Check` reports parse errors and names that do not exist.

## Commands

| Command | Flow |
| --- | --- |
| `ASMDEF` | block name, then `Parameter` (name, type, default, limits or choices), `Bind` (what to drive, the target, the formula; nested inserts are numbered by their position in the block, 1 = first object), `Preset` (name, then a value per parameter), `Join` (see below) or `Check`. |
| `ASMSET` | pick an assembly, name a parameter (or `Preset`) and give the value. |
| `ASMBOM` (`BOM`) | lists every assembly in model space, grouped by block and parameter values, with the quantity; output as a list, a CSV file or a table in the drawing. |
| `ASMINFO` | pick an assembly; lists its parameters, values and formula problems. |
| `-ASMSET` | for panels and scripts: sets a parameter (or `@preset`) on every selected assembly, repeating until an empty parameter name. One undo step. |

The Object Properties panel shows an **Assembly** section for a selection of inserts of one assembly block:
a field per parameter (a drop-down for choices), a preset list and the join check box.

## Mating assemblies

Block inserts (assemblies included) can be mated with `JOINT` and limited with `JOINTLIMIT`; see
`docs/constraints.md`.

## Joint assemblies

Give the assembly a **join class** (`ASMDEF Join wall`). Inserts of blocks with the same class that overlap
read as **one body**: every polyline of one member that lies inside the **body outline** of another member is
not drawn, so the shared edges and the hatch lines under the other member disappear. T, L and X joins and
walls that overlap along an edge all work, and the result follows the members live: move, rotate or change a
parameter of one member and the joint is recomputed.

- The body outline of a member is the largest **closed polyline** in its drawing. Draw the outline of a wall,
  slab or frame as a closed polyline (for example with `RECTANG`) for the join to find it.
- When two members share an edge exactly, the member with the lower entity id draws it.
- Solid fills are not clipped (two fills of one colour look the same where they overlap).
- `UNJOINASM` keeps the selected inserts separate; `JOINASM` lets them join again; `ASMDEF Join` with an
  empty class turns joining off for the block. Nothing is deleted: the joint is only a way of drawing.
- Joining applies to inserts in model space. Explode, DXF export and object snap use the members' own
  geometry.

## Files and DXF

In `.tbcad` the definitions are the `ASMB` chunk and the values and the un-join flag are trailing sections
of the Insert record (format 1.5, see `docs/file-format.md`). DXF export writes the current geometry of each
assembly as plain objects, as for dynamic blocks.

## MCP

`list_assembly_parameters` (`id`) returns the parameters with type, value, limits and choices, the preset
names and formula problems. `set_assembly_parameters` (`id`, `values`: name to number or text, optional
`preset`) changes them in one undo step; a value the parameter does not allow is an error.
