#include "tbcad/table/table.h"
#include "tbcad/text/text.h"

namespace tbcad::table {

namespace {

Entity textEntity(const Entity& owner, const TableData& t, const TextItem& item, double f) {
    Entity out;
    out.common = owner.common;
    out.common.id = {};
    out.common.annotative = false;  // the height is already scaled
    MTextData m;
    m.position = item.position;
    m.height = t.textHeight * f;
    m.width = item.width;
    m.rotation = item.rotation;
    m.attach = item.attach;
    m.style = t.style;
    m.utf8 = item.utf8;
    out.data = Indirect<MTextData>(std::move(m));
    return out;
}

}  // namespace

void buildTableDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                       DisplayList& out) {
    const auto* t = entityAs<TableData>(e);
    if (!t)
        return;
    const Layout L = layout(*t);
    const DisplayStyle style = geom::resolveStyle(doc, e, ctx);
    for (const auto& [a, b] : L.lines) {
        DisplayPolyline pl;
        pl.pts = {a, b};
        pl.style = style;
        out.items.emplace_back(std::move(pl));
    }
    const double f = annotationFactor(doc.header(), e.common, ctx.annoScale);
    for (const TextItem& item : L.texts)
        text::buildTextDisplay(doc, textEntity(e, *t, item, f), ctx, out);
}

void registerDisplay() {
    geom::DisplayRegistry::set(EntityKind::Table, buildTableDisplay,
                               [](const Document&, const Entity& e) {
                                   const auto* t = entityAs<TableData>(e);
                                   return t ? bounds(*t) : BBox2{};
                               });
}

Result<std::vector<Entity>> explodeTable(const Document& doc, const Entity& e) {
    const auto* t = entityAs<TableData>(e);
    if (!t)
        return makeError(ErrorCode::InvalidArgument, "not a table");
    const Layout L = layout(*t);
    std::vector<Entity> out;
    for (const auto& [a, b] : L.lines) {
        Entity n;
        n.common = e.common;
        n.common.id = {};
        n.common.annotative = false;
        n.data = LineData{a, b};
        out.push_back(std::move(n));
    }
    const double f = annotationFactor(doc.header(), e.common);
    for (const TextItem& item : L.texts)
        out.push_back(textEntity(e, *t, item, f));
    return out;
}

}  // namespace tbcad::table
