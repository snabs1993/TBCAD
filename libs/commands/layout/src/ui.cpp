#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "tbcad/layout/layout.h"
#include "tbcad/layoutcmd/layoutcmd.h"
#include "tbcad/render/overlay.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::layoutcmd {

namespace {

// Clicks this close to a viewport's border (in pixels' worth of drawing units) select it.
constexpr double kBorderReach = 2.0;

std::string describe(const layout::Viewport& v) {
    char scale[48];
    std::snprintf(scale, sizeof scale, "1:%g", v.scaleN);
    return "Viewport " + std::to_string(v.id) + " selected (" + scale +
           (v.locked ? ", locked" : "") + (v.on ? "" : ", off") +
           "). VPSCALE, VPLOCK, VPMOVE, VPRESIZE, VPCENTER, VPLAYER and VPDELETE use it.";
}

// The viewport whose border passes within `reach` of `p`, the topmost one first.
const layout::Viewport* borderAt(const layout::Layout& lay, Vec2 p, double reach) {
    const layout::Viewport* hit = nullptr;
    for (const layout::Viewport& v : lay.viewports) {
        if (!v.on)
            continue;
        const BBox2 outer = v.paper.inflated(reach);
        const BBox2 inner = v.paper.inflated(-reach);
        if (outer.contains(p) && !(inner.width() > 0 && inner.height() > 0 && inner.contains(p)))
            hit = &v;  // later viewports are drawn on top
    }
    return hit;
}

}  // namespace

int selectedViewport(const cmd::IHost& host, const Document& doc) {
    // In model space: the viewport being edited, if any.
    const bool model = host.drawingOwner() == BlockId{};
    const std::string name = currentLayout(host, doc);
    auto sel = host.setting(model ? "tmp.layout.editing" : "tmp.layout.viewport");
    if (name.empty() || !sel || sel->empty())
        return 0;
    const auto colon = sel->find(':');
    if (colon == std::string::npos || sel->substr(0, colon) != name)
        return 0;
    const int id = std::atoi(sel->c_str() + colon + 1);
    auto lay = layout::findLayout(doc, name);
    return lay && lay->findViewport(id) ? id : 0;
}

void registerUi(ui::UiRegistry& ui) {
    // A click on a viewport's border selects it. Clicks elsewhere only clear that selection, and
    // are then handled as usual.
    ui.addIdleClickHook([](ui::PanelContext& ctx, Vec2 world, double aperture, bool, bool) {
        if (!ctx.doc || !ctx.host || ctx.host->drawingOwner() == BlockId{})
            return false;
        const std::string name = currentLayout(*ctx.host, *ctx.doc);
        auto lay = name.empty() ? std::nullopt : layout::findLayout(*ctx.doc, name);
        if (!lay)
            return false;
        const layout::Viewport* v = borderAt(*lay, world, aperture * kBorderReach);
        if (!v) {
            if (selectedViewport(*ctx.host, *ctx.doc))
                ctx.host->setSetting("tmp.layout.viewport", "");
            return false;
        }
        ctx.host->setSetting("tmp.layout.viewport", name + ":" + std::to_string(v->id));
        ctx.host->print(describe(*v));
        return true;
    });

    // The selected viewport: an accent outline with a square at each corner.
    ui.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (!ctx.doc || !ctx.host)
            return;
        const int id = selectedViewport(*ctx.host, *ctx.doc);
        if (!id)
            return;
        auto lay = layout::findLayout(*ctx.doc, currentLayout(*ctx.host, *ctx.doc));
        const layout::Viewport* v = lay ? lay->findViewport(id) : nullptr;
        if (!v)
            return;
        const Rgba accent{38, 120, 255, 255};
        render::OverlayStyle st;
        st.color = accent;
        st.widthPx = 2.0;
        const Vec2 a = v->paper.min, b = v->paper.max;
        const std::vector<Vec2> ring{a, {b.x, a.y}, b, {a.x, b.y}};
        overlay.polyline(render::Space::World, ring, true, st);
        for (const Vec2& c : ring)
            overlay.marker(render::Space::World, c, render::MarkerKind::Square, 8.0, accent, true);
    });
}

}  // namespace tbcad::layoutcmd
