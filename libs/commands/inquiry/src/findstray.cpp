// FINDSTRAY: finds objects whose drawn shape reaches far outside their own bounds (a long line
// that cannot be selected), at 1:1 and at the scale of every layout viewport.
#include <algorithm>
#include <cmath>
#include <set>
#include <variant>

#include "extra.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layout/layout.h"

namespace tbcad::inquiry {

namespace {

// First display point of `e` outside its bounds grown by a margin, if any.
bool stray(const Document& doc, const Entity& e, double scale, Vec2& at) {
    const BBox2 box = geom::bounds(e, doc);
    if (box.empty())
        return false;
    const double diag = std::hypot(box.width(), box.height());
    geom::DisplayContext dc;
    dc.pixelSizeMm = 0.1 * scale;
    dc.chordTolMm = 0.05 * scale;
    dc.annoScale = scale;
    DisplayList list;
    geom::buildDisplay(doc, e, dc, list);
    const BBox2 reach = box.inflated(0.25 * diag + 10 * dc.pixelSizeMm + 4 * dc.chordTolMm);
    for (const DisplayItem& item : list.items) {
        std::vector<Vec2> pts;
        if (const auto* pl = std::get_if<DisplayPolyline>(&item))
            pts = pl->pts;
        else if (const auto* f = std::get_if<DisplayFill>(&item))
            for (const auto& ring : f->rings)
                pts.insert(pts.end(), ring.begin(), ring.end());
        else if (const auto* d = std::get_if<DisplayPoints>(&item))
            pts = d->pts;
        for (const Vec2& p : pts)
            if (!reach.contains(p)) {
                at = p;
                return true;
            }
    }
    return false;
}

class FindStray final : public cmd::Command {
public:
    std::string_view name() const override { return "FINDSTRAY"; }
    std::string_view description() const override {
        return "Select objects that draw a line far outside their own extents";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::set<double> scales{1.0};
        for (const layout::Layout& l : layout::layouts(ctx.doc()))
            for (const layout::Viewport& vp : l.viewports)
                if (vp.scaleN > 0)
                    scales.insert(vp.scaleN);
        std::vector<EntityId> found;
        int shown = 0, total = 0;
        ctx.doc().forEachEntity([&](const Entity& e) {
            const EntityKind k = kind(e);
            if (ctx.doc().ownerBlock(e.common.id) || k == EntityKind::Ray ||
                k == EntityKind::Proxy || e.common.annotative)
                return;
            for (double s : scales) {
                Vec2 at{};
                if (!stray(ctx.doc(), e, s, at))
                    continue;
                ++total;
                found.push_back(e.common.id);
                if (shown++ < 20) {
                    const Layer* ly = ctx.doc().layer(e.common.layer);
                    ctx.message("Object " + std::to_string(e.common.id.value) + " (kind " +
                                std::to_string(static_cast<int>(k)) + ", layer " +
                                (ly ? ly->name : std::string("?")) + ") draws to " +
                                std::to_string(at.x) + ", " + std::to_string(at.y) +
                                " at scale 1:" + std::to_string(s));
                }
                break;
            }
        });
        if (found.empty()) {
            ctx.message("No stray lines found.");
            co_return;
        }
        ctx.host().setSelection(found);
        ctx.message(std::to_string(total) + " object(s) selected.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFindStray() {
    return std::make_unique<FindStray>();
}

}  // namespace tbcad::inquiry
