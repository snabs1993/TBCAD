# Plugin API (version 1)

A plugin is a shared library that adds commands to TBCAD. The interface is plain C
(`libs/plugin/include/tbcad/plugin/tbcad_plugin.h`, MIT licensed), so plugins can be written in C, C++,
Rust, Zig and so on, and a plugin does not have to publish its source.

## Installing

Put the library (`.dll` on Windows, `.so` on Linux, `.dylib` on macOS) in the `plugins` folder inside the
settings folder (Windows: `%APPDATA%\TBCAD\plugins`). TBCAD loads every library there at start-up. The `PLUGINS`
command lists what was loaded, with the reason for any plugin that was skipped. Setting `plugins.enabled = false`
turns loading off. A plugin that cannot be loaded never stops the program from starting.

A plugin runs with the same rights as TBCAD, so install only plugins you trust.

## Writing a plugin

Export `tbcad_plugin_init`, and optionally `tbcad_plugin_info`:

```c
#include "tbcad/plugin/tbcad_plugin.h"

static const TbcadHost* g;

static int hello(TbcadCommandCtx* ctx, void* user) {
    double x1, y1, x2, y2, base[2];
    if (g->get_point(ctx, "First point", NULL, &x1, &y1) != TBCAD_OK) return 0;
    base[0] = x1; base[1] = y1;
    if (g->get_point(ctx, "Second point", base, &x2, &y2) != TBCAD_OK) return 0;
    g->add_line(ctx, x1, y1, x2, y2, NULL);
    return 0;
}

TBCAD_PLUGIN_EXPORT int tbcad_plugin_init(const TbcadHost* host) {
    g = host;
    return host->register_command("HELLO", "Draw a line", hello, NULL);
}
```

A command is an ordinary blocking function. The prompt calls (`get_point`, `get_real`, `get_integer`,
`get_string`, `get_keyword`, `get_selection`) wait for the user while the program stays responsive; the
drawing calls (`add_line`, `add_circle`, `add_polyline`, `erase`, `move`) change the drawing. The whole command is
one undo step. If a prompt returns `TBCAD_CANCEL` (the user pressed Esc) the command must return; whatever it
drew is discarded. Later prompts keep answering `TBCAD_CANCEL`, so a command that ignores it still ends.

Other calls: `message` (write to the command line), `get_entity` (kind, bounding box, layer), `list_entities`,
`add_menu_item` and `log`. A command name is upper-case ASCII and must not clash with an existing command.

Everything that takes `ctx` must be called from inside the command function. The host table only ever grows at the
end (check `host->size`), and `TBCAD_PLUGIN_ABI_VERSION` changes only when something is removed or changed, in which
case TBCAD refuses plugins built for the old one if they report it in `tbcad_plugin_info`.

## Sample

`libs/plugin/examples/hello_plugin.c` has two commands (HELLOLINE, HELLOGRID). Build it with any C compiler:

```
cc -shared -fPIC -I libs/plugin/include libs/plugin/examples/hello_plugin.c -o hello_plugin.so
```

## How it works

Each plugin command runs the plugin's function on its own thread. Only one of the two threads runs at a time: the
function runs while TBCAD waits, and while a prompt is open the function waits and TBCAD takes the input. So a plugin
never touches the drawing at the same time as the program, and no locking is needed on either side.

## Not in version 1

Panels, entity-change hooks and display generators for new entity kinds are planned for a later version of the
interface. Commands that take a long time block the window while they compute, like any other command.
