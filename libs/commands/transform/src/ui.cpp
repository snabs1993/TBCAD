#include <cstdio>

#include "tbcad/transform/grip_menu.h"
#include "tbcad/transform/grips.h"
#include "tbcad/transform/transform.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::transform {

namespace {
// Grips are 8 px squares; the pick aperture is smaller, so clicks reach a bit further.
constexpr double kGripReach = 1.5;
}  // namespace

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Modify/Move", "MOVE", "modify.move", "");
    ui.addMenuItem("Modify/Copy", "COPY", "modify.copy", "");
    ui.addMenuItem("Modify/Rotate", "ROTATE", "modify.rotate", "");
    ui.addMenuItem("Modify/Scale", "SCALE", "modify.scale", "");
    ui.addMenuItem("Modify/Mirror", "MIRROR", "modify.mirror", "");
    ui.addMenuItem("Modify/Stretch", "STRETCH", "modify.stretch", "");

    // A click on a grip of the selection starts GRIPEDIT; Ctrl makes it a copy.
    ui.addIdleClickHook([](ui::PanelContext& ctx, Vec2 world, double aperture, bool /*shift*/,
                           bool ctrlKey) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty() || !ctx.runCommand)
            return false;
        std::string line = gripClickCommand(*ctx.doc, *ctx.selection, world, aperture * kGripReach,
                                            gripLimit(*ctx.host), ctrlKey);
        if (line.empty())
            return false;
        ctx.runCommand(line);
        return true;
    });

    // Resting the cursor on a grip offers its other actions (add vertex, convert to arc ...).
    ui.addHoverMenu([](ui::PanelContext& ctx, ui::HoverMenu& menu) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty() ||
            ctx.commandActive || ctx.apertureMm <= 0)
            return false;
        auto grips = collectGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
        const Grip* g = hitGrip(grips, ctx.cursorWorld, ctx.apertureMm * kGripReach);
        if (!g)
            return false;
        const auto actions = gripActions(*ctx.doc, *g);
        if (actions.size() < 2)
            return false;
        for (GripAction a : actions) {
            std::string line = gripActionCommand(*ctx.doc, *g, a);
            if (!line.empty())
                menu.items.push_back({gripActionLabel(a), std::move(line)});
        }
        if (menu.items.size() < 2)
            return false;
        char key[96];
        std::snprintf(key, sizeof key, "grip %llu %.9g,%.9g",
                      static_cast<unsigned long long>(g->entity.value), g->pos.x, g->pos.y);
        menu.key = key;
        menu.anchorWorld = g->pos;
        return true;
    });

    // Grip squares for the selection (8 px): blue, pink under the cursor, red while hot.
    ui.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty())
            return;
        auto grips = collectGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
        const auto hot = hotGrip();
        const Grip* hover = ctx.apertureMm > 0
                                ? hitGrip(grips, ctx.cursorWorld, ctx.apertureMm * kGripReach)
                                : nullptr;
        for (const Grip& g : grips) {
            Rgba colour{38, 120, 255, 255};
            if (hot && distance(g.pos, *hot) < 1e-9)
                colour = Rgba{255, 50, 50, 255};
            else if (hover && hover == &g)
                colour = Rgba{255, 105, 180, 255};
            overlay.marker(render::Space::World, g.pos, render::MarkerKind::Square, 8.0, colour,
                           true);
        }
    });
}

}  // namespace tbcad::transform
