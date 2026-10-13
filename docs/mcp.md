# MCP server (W5-7): `libs/mcp`, `libs/mcpapp`, `apps/tbcad-mcp`

TBCAD is an [MCP](https://modelcontextprotocol.io) **server** (protocol revision `2025-06-18`; `2025-03-26`
is accepted too). An AI assistant such as Claude Desktop or Claude Code can read the open drawing, draw,
change layers and sub-layers, run any TBCAD command, undo what it did, save the file, export and look at
the result. Nothing new is linked: JSON comes from `libs/platform`, PNG from miniz, the sockets are about
400 lines of our own code.

Out of scope here: an MCP client inside TBCAD, prompts, sampling, elicitation, resource subscriptions,
remote access and OAuth.

## Using it

1. Start TBCAD and choose **Tools > MCP Server**. The server is **off by default** (`mcp.enabled`).
2. Connect your AI client. `tbcad-mcp --print-config` prints ready-to-paste text:
   - Claude Desktop (`claude_desktop_config.json`):
     `{"mcpServers":{"tbcad":{"command":"C:\\...\\tbcad-mcp.exe","args":[]}}}`
   - Claude Code: `claude mcp add tbcad -- "C:\...\tbcad-mcp.exe"`
3. The first time the AI wants to change the drawing in a session, TBCAD asks:
   "**Claude Desktop** wants to change this drawing." with *Allow for this session*, *Read only* or *Deny*.
   `mcp.editPermission` can be set to `ask` (default), `allow` or `readOnly`.
4. Every AI action that changes the drawing is **one undo step** labelled `AI: <tool>`. Ctrl+Z undoes it.

`tbcad-mcp --launch` starts TBCAD (`tbcad.exe --mcp`) when it is not running. `tbcad.exe --mcp` turns the
server on for that run.

### Headless

`tbcad-mcp --headless [file.tbcad] [--read-only]` serves a drawing over stdio with no window (CI, scripts).
It has every tool except the viewport ones (`capture_view`, `set_view`). Without a file it starts an empty
drawing and `save_drawing` needs a path.

## Security

- The HTTP listener binds `127.0.0.1` only. There is no setting to change that.
- Every HTTP request needs `Authorization: Bearer <token>`. The token is 32 random bytes (hex), new every
  time the server starts, and is written to `mcp.json` (see below), readable by the current user only.
- A request with an `Origin` that is not `http://localhost[:port]` / `http://127.0.0.1[:port]`, or a `Host`
  that is not local, gets `403` (DNS-rebinding guard).
- Request bodies are limited to 16 MB, at most 8 sessions and 16 connections, idle sessions expire after
  30 minutes. Chunked request bodies are refused.
- Editing needs the user's permission per session (see above). The AI cannot undo the user's own steps
  unless it passes `force: true`. It never cancels a command the user is running: while one runs, editing
  tools answer "The user is running LINE in TBCAD; try again when it finishes."

## Transports

| Transport | Where |
|---|---|
| Streamable HTTP | `http://127.0.0.1:<port>/mcp` inside `tbcad.exe` (default port 41730, `mcp.port`; if taken, a free port is used) |
| stdio, bridge | `tbcad-mcp.exe`: forwards each line to the running TBCAD; re-reads the discovery file on every message, so starting TBCAD later works without restarting the AI client. While TBCAD is not running, `tools/list` still answers and `tools/call` returns "TBCAD is not running or its MCP server is off. Open TBCAD and turn on Tools > MCP Server." |
| stdio, headless | `tbcad-mcp.exe --headless` |

HTTP details: `POST /mcp` carries one JSON-RPC message and is answered with JSON (`200`), or `202` for
notifications. `initialize` answers with `Mcp-Session-Id`; later requests must send it (missing `400`,
unknown or expired `404`, so the client initializes again). `GET /mcp` with `Accept: text/event-stream` is
the server-to-client stream (`notifications/tools/list_changed`; a `: keepalive` comment every 25 s).
`DELETE /mcp` ends the session. JSON-RPC batches are not supported (`-32600`).

Discovery file: `<data dir>/mcp.json` (`%LOCALAPPDATA%\TBCAD\mcp.json`, Linux
`$XDG_DATA_HOME/tbcad/mcp.json`), written atomically when the server starts and removed when it stops:
`{"url":"http://127.0.0.1:41730/mcp","token":"...","pid":1234,"version":"0.0.1"}`.

## Tools

Units are millimetres and radians (`{"deg": 90}` and `{"value": 3, "unit": "m"}` are accepted anywhere an
angle or length is). Ids are decimal strings (64-bit safe). Tool errors are results with `isError: true`.
Every tool has an input and an output schema; results come as text content and `structuredContent`.

Read only: `get_drawing_info`, `list_layers`, `query_entities` (filters: ids, kinds, layers with
sub-layers, bbox inside/crossing; `limit` and `cursor` pagination, uses the spatial index for a bbox),
`get_entity`, `measure` (distance, area, length), `get_selection`, `list_commands`, `list_blocks`,
`list_text_styles`, `list_linetypes`, `capture_view` (PNG image content; only when the session has a
viewport; does not change the user's view).

Changing the drawing (one undo step each): `add_entities` (up to 10,000 per call), `modify_entities`
(`set` properties or geometry, or `transform` with move/rotate/scale/mirror), `delete_entities`,
`create_layer`, `modify_layer`, `delete_layer`, `set_current_layer`, `create_level`, `modify_level`,
`delete_level`, `set_current_level`, `set_level_view`, `run_command`. `list_levels` is read only, and
`query_entities` also filters by `levels`. Entities carry a `level` name once the drawing has more than
one level; edits to objects on another level are refused in the default dim view (see `docs/levels.md`).

`run_command` runs a command exactly as typed. `inputs` are strings (`"0,0"`, `"@100,0"`, `"C"`, `""` for
Enter, `"<Esc>"`), `{click:[x,y]}`, `{pick:id}` or `{select:[ids]}`. If the command still waits for input
after the last one, it is cancelled, everything it did is rolled back, and the error names the prompt it
stopped at and the prompts seen. The result lists messages, prompts and the ids created, changed and erased.
Developer commands and file commands (`DEVGEN`, `OPEN`, `SAVE`, ...) are refused; files have their own tools.

Other: `undo` / `redo` (`steps`, default 1; `undo` only undoes `AI: ...` steps unless `force`), `set_selection`,
`save_drawing` (absolute path ending in `.tbcad`), `open_drawing`, `new_drawing` (refused with unsaved changes
unless `discard_changes`), `set_view` (viewport only), `export_pdf` (listed when the `EXPORTPDF` command exists:
it sets `Paper`, `Orientation`, `Scale`, `Area` through that command's menu and then `Export`) and
`export_dxf` (listed when `EXPORTDXF` exists).

Editing tools (one undo step each, `AI: <tool>`):

- `offset_entities` (`ids`, `distance`, `side` / `side_point` / `through`, `round_corners`, `delete_source`),
  `trim_entities` (`targets: [{id, pick}]`, `edges`), `extend_entities` (`targets`, `boundaries`),
  `fillet` / `chamfer` (two entities or a polyline vertex; radius, distances or angle; `trim`),
  `break_entity`, `join_entities`. These call the same `libs/modify` functions as the commands.
- Layouts and plot: `list_layouts`, `create_layout`, `modify_layout`, `delete_layout`, `add_viewport`,
  `modify_viewport` (scale `"1:50"`, `freeze` layers, `remove`), `add_title_block`; `export_pdf` takes `layout`
  to plot one sheet and `color` for the pen colours.
- Blocks: `create_block` (BLOCK command: `ids`, `base`, `originals` convert/retain/delete, `redefine`);
  text with `attdef` becomes an attribute. `list_blocks` lists attributes, dynamic parameters/actions and xref
  state. Insert with `add_entities` kind `insert` (`attributes: {TAG: value}`).
- Dynamic blocks: `add_block_parameter` (linear, rotation, flip, visibility), `add_block_action` (stretch, move,
  scale), `set_dynamic_value` on an insert.
- Layers: `set_active_only`, `layer_state` (list, save, restore with `parts`, rename, delete).
- Constraints: `list_constraints`, `add_constraint` (Coincident, Horizontal, Vertical, Parallel,
  Perpendicular, Equal, Tangent, Fix, Distance, Radius, Angle in degrees; solves and moves the geometry; a
  conflicting set is refused and nothing changes), `remove_constraint`, `solve_constraints`.
- Dimensions and leaders are writable with `add_entities` (`dimKind` linear, aligned, angular, radius,
  diameter, ordinate, ...). Chains and baselines (`DIMCONTINUE`, `DIMBASELINE`) are reachable with
  `run_command`. Hatch accepts `gradient: {type, color1, color2, angle}`; inserts accept `array`.

Resources: `tbcad://drawing/info`, `tbcad://drawing/layers`, `tbcad://commands`, `tbcad://docs/entity-json`.

A failed call leaves nothing behind: the undo group is rolled back.

## Entity JSON

Units: lengths are millimetres, angles are radians. Anywhere an angle is expected you may write
`{"deg": 90}`; anywhere a length is expected you may write `{"value": 3, "unit": "m"}` (units: mm,
cm, m, in, ft). Points are `[x, y]` (or `{"x": 1, "y": 2}`). Output is always mm and radians.

Every entity has:

| field | meaning |
|---|---|
| `id` | decimal string (64-bit safe); ignored when writing |
| `kind` | line, circle, arc, polyline, point, ellipse, spline, text, mtext, hatch, insert, dimension, leader, proxy |
| `layer` | full layer path, sub-layers joined with `~` (`S-FLOOR~edge`) |
| `color` | `"ByLayer"`, `"ByBlock"`, an ACI number 1..255, or `"#RRGGBB"` |
| `linetype` | linetype name, `"ByLayer"` or `"ByBlock"` |
| `lineweight` | mm number, `"ByLayer"`, `"ByBlock"` or `"Default"` |
| `linetypeScale`, `visible` | only present when not 1 / not true |

Kind fields:

- **line**: `start`, `end`
- **circle**: `center`, `radius`
- **arc**: `center`, `radius`, `startAngle`, `endAngle` (counter-clockwise from start to end)
- **polyline**: `vertices` (`[[x,y],...]` or `[{x,y,bulge,startWidth,endWidth},...]`), `closed`.
  `bulge = tan(sweep/4)`, positive = counter-clockwise, belongs to the segment starting at that vertex.
- **point**: `position`
- **ellipse**: `center`, `majorAxis` (vector from the centre to the end of the major axis), `ratio` (minor/major), optional `startParam`, `endParam`
- **spline**: `degree`, `controlPoints`, optional `fitPoints`, `weights`, `closed`
- **text**: `position`, `height`, `value`, optional `rotation`, `style`, `hAlign` (left, center, right, aligned, middle, fit), `vAlign` (baseline, bottom, middle, top), `alignPoint`, `widthFactor`, `oblique`
- **mtext**: `position`, `height`, `width`, `value`, `attachment` (topLeft ... bottomRight), optional `rotation`, `style`
- **hatch**: `loops`, `solid`, and for patterns `pattern`, `scale`, `angle`. When writing, a loop is `[[x,y],...]` or `{"vertices": [...]}` or `{"edges": [{"type":"line"|"arc"|"polyline",...}]}`.
- **insert**: `block` (name), `position`, `scale` (number or `[sx, sy]`), `rotation`
- **dimension**, **leader**: read and write (`dimKind`, definition points, `textOverride`, `style`; leader `vertices`, `text`). Moving one with `set` is refused; use `transform`.
- **proxy**: an imported entity of an unknown type; read only.

Writing: `add_entities` accepts line, circle, arc, polyline, point, ellipse, spline, text, mtext,
hatch and insert. `modify_entities` `set` takes any of the fields above for the entity's kind.

## For developers

```
libs/mcp        protocol (JSON-RPC, lifecycle, tools/resources dispatch), the tools, entity JSON,
                HTTP server and client, stdio loop, HeadlessSession, OwnerQueue. No window, no GL.
libs/mcpapp     AppSession (DrawingSession over ui::AppCore), McpController, panel, dialog, status item
apps/tbcad-mcp  tbcad-mcp.exe: bridge, --headless, --print-config
```

Tools are written once against `mcp::DrawingSession`. `HeadlessSession` and `AppSession` implement it.
Which tools are listed depends on the session (`capture_view` and `set_view` need a viewport, `export_pdf`
needs the `EXPORTPDF` command ...).

Calling a tool (`McpServerCore::callTool`): validate the arguments against the schema, ask for edit
permission on the serving thread (never on the owner thread, because the dialog may wait), then run the
handler on the owner thread (`DrawingSession::runOnOwnerThread`) inside `UndoStack::beginGroup("AI: <tool>")`.
A failed call undoes what it recorded.

`OwnerQueue` hands work from server threads to the UI thread: the server thread blocks on a future, the app
calls `pump()` once per frame with a 50 ms budget and wakes itself with `glfwPostEmptyEvent`.

Adding a tool: add a function to one of `libs/mcp/src/tools_*.cpp`, register it with a schema, document it
here, add a test in `libs/mcp/tests`.

Tests: `tbcad_mcp_tests` (protocol, tools, HTTP, owner queue), `tbcad_mcpcli_tests` (headless end to end,
real `LINE`, bridge, the executable over redirected stdio), `tbcad_mcpapp_tests`.
