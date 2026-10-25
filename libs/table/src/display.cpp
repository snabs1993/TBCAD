#include <algorithm>

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
    for (const Fill& f : L.fills) {
        DisplayFill fill;
        fill.rings.push_back({f.corners[0], f.corners[1], f.corners[2], f.corners[3]});
        fill.style = style;
        fill.style.color = paletteColor(static_cast<std::uint8_t>(f.color));
        out.items.emplace_back(std::move(fill));
    }
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

bool fitRowHeights(const Document& doc, TableData& t) {
    bool grown = false;
    for (std::size_t r = 0; r < rowCount(t); ++r) {
        for (std::size_t c = 0; c < colCount(t); ++c) {
            const TableCell& cell = cellAt(t, r, c);
            if (cell.covered || cell.rowSpan != 1 || cellBody(cell.utf8).empty())
                continue;
            double w = 0;
            for (std::size_t k = c; k < c + cell.colSpan && k < colCount(t); ++k)
                w += t.colWidths[k];
            const TableCellStyle& cs = cellStyleOfRow(t, r);
            MTextData m;
            m.height = cs.textHeight > 0 ? cs.textHeight : t.textHeight;
            m.width = std::max(0.0, w - 2 * t.margin);
            m.style = t.style;
            m.utf8 = displayText(t, r, c);
            const text::TextLayout lay = text::layoutMText(doc, m);
            if (lay.empty())
                continue;
            const double need = (lay.frame.max.y - lay.frame.min.y) + 2 * t.margin;
            if (need > t.rowHeights[r] + 1e-6) {
                t.rowHeights[r] = need;
                grown = true;
            }
        }
    }
    return grown;
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
