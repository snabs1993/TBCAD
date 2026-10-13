/* A sample TBCAD plugin in plain C: two commands.
 *
 *   HELLOLINE  asks for two points and draws a line between them.
 *   HELLOGRID  asks for a corner, a count and a size and draws a grid of circles.
 *
 * Build it as a shared library (see docs/plugin-api.md) and copy it into the plugins folder.
 */
#include <stdio.h>

#include "tbcad/plugin/tbcad_plugin.h"

static const TbcadHost* g_host;

static int helloline(TbcadCommandCtx* ctx, void* user) {
    double x1, y1, x2, y2;
    (void)user;
    if (g_host->get_point(ctx, "Specify first point", NULL, &x1, &y1) != TBCAD_OK)
        return 0;
    {
        double base[2];
        base[0] = x1;
        base[1] = y1;
        if (g_host->get_point(ctx, "Specify second point", base, &x2, &y2) != TBCAD_OK)
            return 0;
    }
    g_host->add_line(ctx, x1, y1, x2, y2, NULL);
    g_host->message(ctx, "Line drawn by the hello plugin.");
    return 0;
}

static int hellogrid(TbcadCommandCtx* ctx, void* user) {
    double x, y, size;
    int64_t n;
    int64_t i, j;
    (void)user;
    if (g_host->get_point(ctx, "Specify the lower left corner", NULL, &x, &y) != TBCAD_OK)
        return 0;
    if (g_host->get_integer(ctx, "Number of circles per side", &n) != TBCAD_OK)
        return 0;
    if (g_host->get_real(ctx, "Distance between circles", &size) != TBCAD_OK)
        return 0;
    for (i = 0; i < n; ++i)
        for (j = 0; j < n; ++j)
            g_host->add_circle(ctx, x + (double)i * size, y + (double)j * size, size * 0.3, NULL);
    return 0;
}

TBCAD_PLUGIN_EXPORT const TbcadPluginInfo* tbcad_plugin_info(void) {
    static const TbcadPluginInfo info = {TBCAD_PLUGIN_ABI_VERSION, "Hello", "1.0",
                                         "TBCAD contributors"};
    return &info;
}

TBCAD_PLUGIN_EXPORT int tbcad_plugin_init(const TbcadHost* host) {
    g_host = host;
    if (host->register_command("HELLOLINE", "Draw a line between two points", helloline, NULL))
        return 1;
    if (host->register_command("HELLOGRID", "Draw a grid of circles", hellogrid, NULL))
        return 1;
    host->add_menu_item("Plugins/Hello line", "HELLOLINE");
    host->log("hello plugin ready");
    return 0;
}
