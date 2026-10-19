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
    const auto st = findStyle(doc, d->style);
    // A style line can have its own colour and linetype; the rest comes from the multiline.
    auto styleOfElement = [&](int k) {
        if (!st || k < 0)
            return style;
        const int c = st->colorOf(static_cast<std::size_t>(k));
        const std::string& lt = st->linetypeOf(static_cast<std::size_t>(k));
        const Linetype* ltype = lt.empty() ? nullptr : doc.linetypeByName(lt);
        if (c == 256 && !ltype)
            return style;
        Entity tmp;
        tmp.common = e.common;
        if (c == 0)
            tmp.common.color = Color::byBlock();
        else if (c < 256)
            tmp.common.color = Color::index(static_cast<std::uint8_t>(c));
        if (ltype)
            tmp.common.linetype = ltype->id;
        return geom::resolveStyle(doc, tmp, ctx);
    };
    if (st && st->fill >= 0 && !s.fill.empty()) {
        Entity tmp;
        tmp.common = e.common;
        if (st->fill == 0)
            tmp.common.color = Color::byBlock();
        else if (st->fill < 256)
            tmp.common.color = Color::index(static_cast<std::uint8_t>(st->fill));
        DisplayFill f;
        f.rings = s.fill;
        f.style = geom::resolveStyle(doc, tmp, ctx);
        out.items.emplace_back(std::move(f));
    }
    for (std::size_t i = 0; i < s.lines.size(); ++i) {
        DisplayPolyline pl;
        pl.pts = s.lines[i];
        pl.closed = s.closed[i];
        pl.style = styleOfElement(s.element[i]);
        out.items.emplace_back(std::move(pl));
    }
}

}  // namespace

void registerDisplay() {
    geom::DisplayRegistry::set(EntityKind::MLine, build);
}

}  // namespace tbcad::mline
