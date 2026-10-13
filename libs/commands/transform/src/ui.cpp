#include "tbcad/transform/grips.h"
#include "tbcad/transform/transform.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::transform {

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Modify/Move", "MOVE", "modify.move", "");
    ui.addMenuItem("Modify/Copy", "COPY", "modify.copy", "");
    ui.addMenuItem("Modify/Rotate", "ROTATE", "modify.rotate", "");
    ui.addMenuItem("Modify/Scale", "SCALE", "modify.scale", "");
    ui.addMenuItem("Modify/Mirror", "MIRROR", "modify.mirror", "");

    // Grip squares for the selection (8 px, accent blue). Hover / hot colours need the cursor and
    // the hot grip; the application draws those once it forwards them (see the PR notes).
    ui.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty())
            return;
        auto grips = collectGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
        for (const Grip& g : grips)
            overlay.marker(render::Space::World, g.pos, render::MarkerKind::Square, 8.0,
                           Rgba{38, 120, 255, 255}, true);
    });
}

}  // namespace tbcad::transform
