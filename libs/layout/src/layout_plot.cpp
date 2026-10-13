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

        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, true});
        exportlib::appendEntities(sheet, doc, model, vl, s, [&](const Entity& e) {
            return frozenInViewport(doc, vp, e.common.layer) ||
                   (vp.level.value != 0 && e.common.level != vp.level);
        });
        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, false});

        if (options.viewportBorders) {
            exportlib::PlotPath border;
            border.closed = true;
            border.style.color = exportlib::plotColor(Rgba{0, 0, 0, 255}, s.color);
            border.style.widthMm = s.lineweights ? 0.25 : s.hairlineMm;
            border.pts = {vp.paper.min,
                          {vp.paper.max.x, vp.paper.min.y},
                          vp.paper.max,
                          {vp.paper.min.x, vp.paper.max.y}};
            sheet.prims.emplace_back(std::move(border));
        }
    }
    return sheet;
}

}  // namespace tbcad::layout
