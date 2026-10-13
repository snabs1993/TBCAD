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
