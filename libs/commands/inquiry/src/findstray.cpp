// FINDSTRAY: ranks objects (and the objects inside blocks) whose drawn shape has a stray segment:
// a vertex far outside the object's own extents, or one segment much longer than the rest of its
// curve. Checked at 1:1 and at the scale of every layout viewport.
#include <algorithm>
#include <cmath>
#include <set>
#include <variant>

#include "extra.h"
#include "tbcad/blocks/display.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layout/layout.h"

namespace tbcad::inquiry {

namespace {

const char* kindName(EntityKind k) {
    static const char* names[] = {"Line",   "Circle", "Arc",   "Polyline", "Point",  "Ellipse",
                                  "Spline", "Text",   "MText", "Hatch",    "Insert", "Dimension",
                                  "Leader", "Proxy",  "Table", "Ray",      "MLine"};
    const auto i = static_cast<std::size_t>(k);
    return i < sizeof names / sizeof *names ? names[i] : "?";
}

struct Hit {
    double score = 0;  // 1 = as far as the object is big, or one segment as long as its curve
    EntityId id{};
    EntityKind kind{};
    std::string layer, why;
};

void check(const Document& doc, const Entity& e, double scale, std::vector<Hit>& hits) {
    const BBox2 box = geom::bounds(e, doc);
    if (box.empty())
        return;
    const double diag = std::hypot(box.width(), box.height());
    geom::DisplayContext dc;
    dc.pixelSizeMm = 0.1 * scale;
    dc.chordTolMm = 0.05 * scale;
    dc.annoScale = scale;
    DisplayList list;
    geom::buildDisplay(doc, e, dc, list);
    const BBox2 reach = box.inflated(0.25 * diag + 10 * dc.pixelSizeMm + 4 * dc.chordTolMm);
    const EntityKind k = kind(e);
    const bool curved =
        k != EntityKind::Line && k != EntityKind::Polyline && k != EntityKind::Point;

    Hit best;
    best.id = e.common.id;
    best.kind = k;
    auto consider = [&](double score, std::string why) {
        if (score > best.score) {
            best.score = score;
            best.why = std::move(why);
        }
    };
    auto outside = [&](Vec2 p) {
        if (reach.contains(p))
            return;
        const double dx = std::max({reach.min.x - p.x, 0.0, p.x - reach.max.x});
        const double dy = std::max({reach.min.y - p.y, 0.0, p.y - reach.max.y});
        consider(std::hypot(dx, dy) / std::max(diag, dc.pixelSizeMm),
                 "point " + std::to_string(p.x) + ", " + std::to_string(p.y) + " outside extents");
    };
    for (const DisplayItem& item : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            for (const Vec2& p : pl->pts)
                outside(p);
            // One segment far longer than all the others of a curve.
            if (curved && pl->pts.size() >= 8) {
                double longest = 0, second = 0, total = 0;
                std::size_t at = 0;
                for (std::size_t i = 1; i < pl->pts.size(); ++i) {
                    const double len = std::hypot(pl->pts[i].x - pl->pts[i - 1].x,
                                                  pl->pts[i].y - pl->pts[i - 1].y);
                    total += len;
                    if (len > longest) {
                        second = longest;
                        longest = len;
                        at = i;
                    } else if (len > second) {
                        second = len;
                    }
                }
                if (longest > 20 * dc.pixelSizeMm && longest > 8 * second && longest > 0.3 * total)
                    consider(longest / std::max(diag, dc.pixelSizeMm),
                             "one segment of length " + std::to_string(longest) + " near " +
                                 std::to_string(pl->pts[at].x) + ", " +
                                 std::to_string(pl->pts[at].y));
            }
        } else if (const auto* f = std::get_if<DisplayFill>(&item)) {
            for (const auto& ring : f->rings)
                for (const Vec2& p : ring)
                    outside(p);
        } else if (const auto* d = std::get_if<DisplayPoints>(&item)) {
            for (const Vec2& p : d->pts)
                outside(p);
        }
    }
    if (best.score > 0) {
        const Layer* ly = doc.layer(e.common.layer);
        best.layer = ly ? ly->name : std::string("?");
        best.why += " at 1:" + std::to_string(scale);
        if (const BlockDef* b = doc.block(doc.ownerBlock(e.common.id)))
            best.why += " (in block " + b->name + ")";
        hits.push_back(std::move(best));
    }
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
        std::vector<Hit> hits;
        std::size_t inBlocks = 0;
        // Each block is drawn once per scale, not again for every reference to it (blocks nested
        // a dozen deep took minutes).
        blocks::BlockDisplayCache cache;
        blocks::BlockDisplayScope scope(cache);
        ctx.doc().forEachEntity([&](const Entity& e) {
            const EntityKind k = kind(e);
            if (k == EntityKind::Ray || k == EntityKind::Proxy || e.common.annotative)
                return;
            const bool owned = ctx.doc().ownerBlock(e.common.id) != BlockId{};
            // A reference inside a block: what it draws is checked as the members of its block.
            if (owned && k == EntityKind::Insert)
                return;
            const std::size_t before = hits.size();
            for (double s : scales)
                check(ctx.doc(), e, s, hits);
            if (owned)
                inBlocks += hits.size() - before;
        });
        if (hits.empty()) {
            ctx.message("No stray lines found.");
            co_return;
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const Hit& a, const Hit& b) { return a.score > b.score; });
        std::vector<EntityId> pick;
        std::string full;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const Hit& h = hits[i];
            const std::string line = "#" + std::to_string(h.id.value) + " " + kindName(h.kind) +
                                     " layer " + h.layer + ": " + h.why + " (x" +
                                     std::to_string(h.score).substr(0, 5) + ")";
            full += line + "\n";
            if (i < 20)
                ctx.message(line);
        }
        ctx.host().setSetting("clipboard.text", full);
        for (const Hit& h : hits)
            if (ctx.doc().ownerBlock(h.id) == BlockId{})
                pick.push_back(h.id);
        ctx.message(std::to_string(hits.size()) + " suspect(s), " + std::to_string(inBlocks) +
                    " inside blocks; " + std::to_string(pick.size()) +
                    " selected. The full list is on the clipboard.");
        if (!pick.empty())
            ctx.host().setSelection(pick);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFindStray() {
    return std::make_unique<FindStray>();
}

}  // namespace tbcad::inquiry
