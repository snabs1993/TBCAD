#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "tbcad/core/color.h"
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
    ui.addIdleClickHook([](ui::PanelContext& ctx, Vec2 world, double aperture, bool shift,
                           bool ctrlKey) {
        if (!ctx.selection || ctx.selection->empty())
            clearWarmGrips();  // nothing selected: nothing stays picked
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty() || !ctx.runCommand)
            return false;
        // Shift picks grips without dragging: clicking one of the picked grips then drags them
        // all by the same amount.
        if (shift) {
            const auto& grips = cachedGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
            if (const Grip* g = hitGrip(grips, world, aperture * kGripReach)) {
                toggleWarmGrip(g->pos);
                return true;
            }
            return false;
        }
        std::string line = gripClickCommand(*ctx.doc, *ctx.selection, world, aperture * kGripReach,
                                            gripLimit(*ctx.host), ctrlKey);
        if (line.empty()) {
            clearWarmGrips();
            return false;
        }
        ctx.runCommand(line);
        return true;
    });

    // Resting the cursor on a grip offers its other actions (add vertex, convert to arc ...).
    ui.addHoverMenu([](ui::PanelContext& ctx, ui::HoverMenu& menu) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty() ||
            ctx.commandActive || ctx.apertureMm <= 0)
            return false;
        const auto& grips = cachedGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
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

    // Grip squares for the selection, 2 x GRIPSIZE px: blue, pink under the cursor, red while
    // hot, or the colours GRIPCOLOR, GRIPHOVER and GRIPHOT set (colour numbers 1-255).
    ui.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (!ctx.doc || !ctx.host || !ctx.selection || ctx.selection->empty())
            return;
        auto aciSetting = [&](const char* key, Rgba fallback) {
            const auto v = ctx.host->setting(key);
            const long n = v ? std::strtol(v->c_str(), nullptr, 10) : 0;
            return n >= 1 && n <= 255 ? paletteColor(static_cast<std::uint8_t>(n)) : fallback;
        };
        const Rgba cold = aciSetting("grips.color", Rgba{38, 120, 255, 255});
        const Rgba hotColour = aciSetting("grips.hot", Rgba{255, 50, 50, 255});
        const Rgba hoverColour = aciSetting("grips.hover", Rgba{255, 105, 180, 255});
        double sizePx = 8.0;
        if (const auto v = ctx.host->setting("grips.size"))
            sizePx = 2.0 * std::clamp(std::strtod(v->c_str(), nullptr), 1.0, 20.0);
        const auto& grips = cachedGrips(*ctx.doc, *ctx.selection, gripLimit(*ctx.host));
        const auto hot = hotGrip();
        // Only the grips in view are drawn, and no more than a screen can show.
        constexpr std::size_t kMaxDrawn = 20000;
        BBox2 view;
        if (ctx.camera)
            view = ctx.camera->visibleWorld().inflated(4 * ctx.apertureMm);
        std::size_t drawn = 0;
        const Grip* hover = ctx.apertureMm > 0
                                ? hitGrip(grips, ctx.cursorWorld, ctx.apertureMm * kGripReach)
                                : nullptr;
        for (const Grip& g : grips) {
            if (ctx.camera && !view.contains(g.pos))
                continue;
            if (++drawn > kMaxDrawn)
                break;
            Rgba colour = cold;
            const auto& warm = warmGrips();
            if (hot && distance(g.pos, *hot) < 1e-9)
                colour = hotColour;
            else if (!warm.empty() && std::any_of(warm.begin(), warm.end(), [&](Vec2 w) {
                         return distance(g.pos, w) < 1e-9;
                     }))
                colour = hotColour;  // picked with Shift
            else if (hover && hover == &g)
                colour = hoverColour;
            overlay.marker(render::Space::World, g.pos, render::MarkerKind::Square, sizePx, colour,
                           true);
        }
    });
}

}  // namespace tbcad::transform
