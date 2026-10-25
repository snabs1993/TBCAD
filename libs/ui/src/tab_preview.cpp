#include "tbcad/ui/tab_preview.h"

#include <algorithm>
#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/layout/layout.h"

namespace tbcad::ui {

namespace {

constexpr std::size_t kMaxPoints = 20000;
constexpr double kPixels = 128;  // the picture is about this many pixels on its longer side

struct Strokes {
    std::vector<std::vector<Vec2>> lines{};
    std::size_t points = 0;
    BBox2 box{};

    void add(std::vector<Vec2> pts, bool closed) {
        if (pts.size() < 2 || points >= kMaxPoints)
            return;
        if (closed && distance(pts.front(), pts.back()) > 0)
            pts.push_back(pts.front());
        for (Vec2 p : pts)
            box.expand(p);
        points += pts.size();
        lines.push_back(std::move(pts));
    }

    void add(const DisplayList& list) {
        for (const DisplayItem& item : list.items) {
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                add(pl->pts, pl->closed);
            } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                for (const auto& ring : fill->rings)
                    add(ring, true);
            }
        }
    }

    // Adds `pts` mapped by `map` and cut to `clip`; each piece inside becomes a stroke.
    template <class Map>
    void addClipped(const std::vector<Vec2>& pts, bool closed, const BBox2& clip, Map map) {
        std::vector<Vec2> run;
        auto flush = [&] {
            add(std::move(run), false);
            run.clear();
        };
        const std::size_t n = pts.size() + (closed ? 1 : 0);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            Vec2 a = map(pts[i]), b = map(pts[(i + 1) % pts.size()]);
            // Liang-Barsky
            double t0 = 0, t1 = 1;
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double p[4] = {-dx, dx, -dy, dy};
            const double q[4] = {a.x - clip.min.x, clip.max.x - a.x, a.y - clip.min.y,
                                 clip.max.y - a.y};
            bool inside = true;
            for (int k = 0; k < 4 && inside; ++k) {
                if (p[k] == 0) {
                    inside = q[k] >= 0;
                } else {
                    const double r = q[k] / p[k];
                    if (p[k] < 0)
                        t0 = std::max(t0, r);
                    else
                        t1 = std::min(t1, r);
                    inside = t0 <= t1;
                }
            }
            if (!inside) {
                flush();
                continue;
            }
            const Vec2 c{a.x + t0 * dx, a.y + t0 * dy}, d{a.x + t1 * dx, a.y + t1 * dy};
            if (!run.empty() && distance(run.back(), c) > 1e-9)
                flush();
            if (run.empty())
                run.push_back(c);
            run.push_back(d);
            if (t1 < 1)
                flush();
        }
        flush();
    }

    TabPreview finish() {
        TabPreview t;
        if (lines.empty() || box.empty())
            return t;
        const double w = box.max.x - box.min.x, h = box.max.y - box.min.y;
        const double side = std::max(w, h);
        if (!(side > 0))
            return t;
        for (auto& s : lines)
            for (Vec2& p : s)
                p = {(p.x - box.min.x) / side, (p.y - box.min.y) / side};
        t.strokes = std::move(lines);
        t.width = w / side;
        t.height = h / side;
        return t;
    }
};

// Entities of model space whose bounds touch `window`, drawn with `pixelSize` model units per
// picture pixel.
template <class Sink>
void eachModelEntity(const Document& doc, const BBox2& window, double pixelSize, Sink sink) {
    geom::DisplayContext dc;
    dc.pixelSizeMm = pixelSize;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        if (!window.empty()) {
            const BBox2 b = geom::bounds(*e, doc);
            if (b.empty() || b.max.x < window.min.x || b.min.x > window.max.x ||
                b.max.y < window.min.y || b.min.y > window.max.y)
                continue;
        }
        DisplayList list;
        geom::buildDisplay(doc, *e, dc, list);
        sink(list);
    }
}

}  // namespace

TabPreview previewOfModel(const Document& doc) {
    BBox2 extent;
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            if (e->common.visible)
                extent.merge(geom::bounds(*e, doc));
    if (extent.empty())
        return {};
    const double side = std::max(extent.max.x - extent.min.x, extent.max.y - extent.min.y);
    if (!(side > 0))
        return {};
    Strokes s;
    eachModelEntity(doc, BBox2{}, side / kPixels, [&](const DisplayList& l) { s.add(l); });
    return s.finish();
}

TabPreview previewOfLayout(const Document& doc, const std::string& layoutName) {
    const auto found = layout::findLayout(doc, layoutName);
    if (!found || !(found->widthMm > 0) || !(found->heightMm > 0))
        return {};
    const layout::Layout& lay = *found;
    const double sheetPixel = std::max(lay.widthMm, lay.heightMm) / kPixels;
    Strokes s;
    s.add({{0, 0}, {lay.widthMm, 0}, {lay.widthMm, lay.heightMm}, {0, lay.heightMm}}, true);
    // The sheet's own objects (title block, notes) live in a block at sheet coordinates.
    if (const BlockDef* def = doc.block(lay.content)) {
        Entity ins;
        ins.common.layer = kLayerZero;
        ins.data = Indirect<InsertData>(InsertData{lay.content, def->base, {1, 1}, 0});
        geom::DisplayContext dc;
        dc.pixelSizeMm = sheetPixel;
        DisplayList list;
        blocks::buildInsertDisplay(doc, ins, dc, list);
        s.add(list);
    }
    for (const layout::Viewport& vp : lay.viewports) {
        if (!vp.on || vp.paper.empty())
            continue;
        s.add(layout::viewportOutline(vp), true);
        if (!(vp.scaleN > 0))
            continue;
        const double n = vp.scaleN;
        const Vec2 mid{(vp.paper.min.x + vp.paper.max.x) / 2,
                       (vp.paper.min.y + vp.paper.max.y) / 2};
        const double hw = (vp.paper.max.x - vp.paper.min.x) / 2 * n;
        const double hh = (vp.paper.max.y - vp.paper.min.y) / 2 * n;
        const BBox2 window{{vp.center.x - hw, vp.center.y - hh},
                           {vp.center.x + hw, vp.center.y + hh}};
        auto map = [&](Vec2 p) {
            return Vec2{mid.x + (p.x - vp.center.x) / n, mid.y + (p.y - vp.center.y) / n};
        };
        eachModelEntity(doc, window, sheetPixel * n, [&](const DisplayList& list) {
            for (const DisplayItem& item : list.items) {
                if (s.points >= kMaxPoints)
                    return;
                if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                    s.addClipped(pl->pts, pl->closed, vp.paper, map);
                } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                    for (const auto& ring : fill->rings)
                        s.addClipped(ring, true, vp.paper, map);
                }
            }
        });
    }
    return s.finish();
}

}  // namespace tbcad::ui
