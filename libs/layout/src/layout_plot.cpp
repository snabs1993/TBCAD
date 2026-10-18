#include <algorithm>

#include "tbcad/geom/geom.h"
#include "tbcad/layout/layout.h"

namespace tbcad::layout {

Result<exportlib::PlotSheet> buildLayoutSheet(const Document& doc, const std::string& layoutName,
                                              const exportlib::PlotSettings& base,
                                              const LayoutPlotOptions& options) {
    auto found = findLayout(doc, layoutName);
    if (!found)
        return Error{ErrorCode::NotFound, "No layout named \"" + layoutName + "\""};
    const Layout& l = *found;

    // The sheet itself is plotted 1:1 with no margins: sheet millimetres are paper millimetres.
    exportlib::PlotSettings s = base;
    s.paperName = "Custom";
    s.customWidthMm = l.widthMm;
    s.customHeightMm = l.heightMm;
    s.landscape = l.widthMm > l.heightMm;
    s.marginMm = 0;
    s.area = exportlib::PlotArea::Window;
    s.window = {};
    s.window.expand({0, 0});
    s.window.expand({l.widthMm, l.heightMm});
    s.fitToPaper = false;
    s.scaleN = 1;
    s.centerOnPaper = true;
    auto lay = exportlib::computeLayout(doc, s);
    if (!lay)
        return lay.error();

    exportlib::PlotSheet sheet;
    sheet.layout = lay.value();
    if (const BlockDef* block = doc.block(l.content))
        exportlib::appendEntities(sheet, doc, block->entities, sheet.layout, s);

    const std::vector<EntityId> model = doc.modelSpace();
    // Every viewport only needs the objects that reach its window. The bounds are worked out once
    // here (for the kinds whose size does not depend on the viewport scale) instead of once per
    // viewport inside appendEntities, which matters with a few hundred thousand objects.
    struct Boxed {
        EntityId id;
        BBox2 box;
        bool always;
    };
    std::vector<Boxed> boxed;
    if (!l.viewports.empty()) {
        boxed.reserve(model.size());
        for (EntityId id : model) {
            const Entity* e = doc.find(id);
            if (!e)
                continue;
            bool fixed = false;
            switch (kind(*e)) {
                case EntityKind::Line:
                case EntityKind::Circle:
                case EntityKind::Arc:
                case EntityKind::Polyline:
                case EntityKind::Ellipse:
                case EntityKind::Spline:
                case EntityKind::Point:
                case EntityKind::Text:
                case EntityKind::MText:
                    fixed = !e->common.annotative;
                    break;
                default:
                    break;
            }
            Boxed b{id, fixed ? geom::bounds(*e, doc) : BBox2{}, !fixed};
            if (!b.always && b.box.empty())
                b.always = true;
            boxed.push_back(std::move(b));
        }
    }
    for (const Viewport& vp : l.viewports) {
        if (!vp.on || vp.paper.empty() || !(vp.scaleN > 0))
            continue;
        // The viewport shows the model window of the same size as the rectangle, scaled 1:N.
        exportlib::PlotLayout vl = sheet.layout;
        vl.paperPerUnit = 1.0 / vp.scaleN;
        const Vec2 half{vp.paper.width() * vp.scaleN / 2, vp.paper.height() * vp.scaleN / 2};
        vl.area.min = vp.center - half;
        vl.area.max = vp.center + half;
        vl.origin = vp.paper.min;
        vl.overflows = false;
        vl.annoScale = vp.scaleN;
        vl.layerHidden = [&doc, &vp](LayerId l) { return frozenInViewport(doc, vp, l); };

        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, true, vp.clip});
        const BBox2 reach = vl.area.inflated(2.0 / vl.paperPerUnit);
        std::vector<EntityId> near;
        for (const Boxed& b : boxed)
            if (b.always || b.box.intersects(reach))
                near.push_back(b.id);
        exportlib::appendEntities(sheet, doc, near, vl, s, [&](const Entity& e) {
            if (e.common.annotative && !e.common.annoScales.empty() &&
                std::find(e.common.annoScales.begin(), e.common.annoScales.end(), vp.scaleN) ==
                    e.common.annoScales.end())
                return true;  // made for other scales
            return frozenInViewport(doc, vp, e.common.layer) ||
                   (vp.level.value != 0 && e.common.level != vp.level);
        });
        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, false, {}});

        if (options.viewportBorders && vp.border) {
            exportlib::PlotPath border;
            border.closed = true;
            border.style.color = exportlib::plotColor(Rgba{0, 0, 0, 255}, s.color);
            border.style.widthMm = s.lineweights ? 0.25 : s.hairlineMm;
            border.pts = viewportOutline(vp);
            sheet.prims.emplace_back(std::move(border));
        }
    }
    return sheet;
}

}  // namespace tbcad::layout
