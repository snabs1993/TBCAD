#include "tbcad/geom/geom.h"
#include "tbcad/mline/mline.h"

namespace tbcad::mline {

namespace {

void build(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
           DisplayList& out) {
    const auto* d = entityAs<MlineData>(e);
    if (!d)
        return;
    const Strokes s = strokes(doc, *d);
    const DisplayStyle style = geom::resolveStyle(doc, e, ctx);
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        DisplayPolyline pl;
        pl.pts = s.lines[i];
        pl.closed = s.closed[i];
        pl.style = style;
        out.items.emplace_back(std::move(pl));
    }
}

}  // namespace

void registerDisplay() {
    geom::DisplayRegistry::set(EntityKind::MLine, build);
}

}  // namespace tbcad::mline
