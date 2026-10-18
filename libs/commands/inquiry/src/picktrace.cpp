// PICKTRACE: lists every object whose drawn shape passes near a picked point, with the part of it
// that is nearest, so a line that cannot be selected can be traced to its source.
#include <algorithm>
#include <cmath>
#include <sstream>
#include <variant>

#include "extra.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layout/layout.h"

namespace tbcad::inquiry {

namespace {

constexpr const char* kRadiusKey = "picktrace.radius";

double segmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

const char* kindName(EntityKind k) {
    static const char* names[] = {"Line",   "Circle", "Arc",   "Polyline", "Point",  "Ellipse",
                                  "Spline", "Text",   "MText", "Hatch",    "Insert", "Dimension",
                                  "Leader", "Proxy",  "Table", "Ray",      "MLine"};
    const auto i = static_cast<std::size_t>(k);
    return i < sizeof names / sizeof *names ? names[i] : "?";
}

class PickTrace final : public cmd::Command {
public:
    std::string_view name() const override { return "PICKTRACE"; }
    std::string_view description() const override {
        return "List the objects (also inside blocks) whose drawing passes near a point";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        double radius = 1.0;
        if (auto v = ctx.host().setting(kRadiusKey)) {
            try {
                radius = std::stod(*v);
            } catch (...) {
            }
        }
        cmd::DistanceOptions ro;
        ro.defaultValue = radius;
        ro.allowZero = false;
        auto r = co_await ctx.getDistance("Search radius in drawing units", ro);
        if (!r.ok())
            co_return;
        radius = r.value;
        ctx.host().setSetting(kRadiusKey, std::to_string(radius));
        auto at = co_await ctx.getPoint("Specify a point on the line");
        if (!at.ok())
            co_return;

        std::ostringstream report;
        std::vector<EntityId> pick;
        int lines = 0;
        auto emit = [&](const std::string& text) {
            report << text;
            if (++lines <= 30)
                ctx.message(text);
        };
        // One search over `ids` around `p` (drawing units of that space) at the viewport scale.
        auto search = [&](const std::vector<EntityId>& ids, Vec2 p, double rad, double scale,
                          const std::string& where) {
            for (EntityId id : ids) {
                const Entity* ep = ctx.doc().find(id);
                if (!ep || kind(*ep) == EntityKind::Ray)
                    continue;
                const Entity& e = *ep;
                geom::DisplayContext dc;
                dc.pixelSizeMm = rad / 10;
                dc.annoScale = scale;
                DisplayList list;
                geom::buildDisplay(ctx.doc(), e, dc, list);
                double best = rad, bestLen = 0;
                LayerId bestLayer{};
                bool hit = false;
                for (const DisplayItem& item : list.items) {
                    const auto* pl = std::get_if<DisplayPolyline>(&item);
                    if (!pl)
                        continue;
                    const std::size_t n = pl->pts.size();
                    for (std::size_t i = 1; i <= n; ++i) {
                        if (i == n && !pl->closed)
                            break;
                        const Vec2 a = pl->pts[i - 1], b = pl->pts[i % n];
                        const double d = segmentDistance(p, a, b);
                        if (d <= best) {
                            best = d;
                            bestLen = std::hypot(b.x - a.x, b.y - a.y);
                            bestLayer = pl->style.layer;
                            hit = true;
                        }
                    }
                }
                if (!hit)
                    continue;
                const Layer* own = ctx.doc().layer(e.common.layer);
                const Layer* drawn = ctx.doc().layer(bestLayer);
                std::ostringstream line;
                line << where << "#" << e.common.id.value << " " << kindName(kind(e)) << " layer "
                     << (own ? own->name : "?");
                if (drawn && own != drawn)
                    line << " (drawn on " << drawn->name << ")";
                line << ": " << best << " away, segment length " << bestLen << "\n";
                emit(line.str());
                pick.push_back(e.common.id);
                // An insert: show what inside the block draws the longest single segment.
                if (const auto* ins = entityAs<InsertData>(e)) {
                    if (const BlockDef* blk = ctx.doc().block(ins->block)) {
                        emit("    block \"" + blk->name + "\" at " +
                             std::to_string(ins->position.x) + ", " +
                             std::to_string(ins->position.y) + " scale " +
                             std::to_string(ins->scale.x) + ", " + std::to_string(ins->scale.y) +
                             ", " + std::to_string(blk->entities.size()) + " objects\n");
                        struct Member {
                            double longest;
                            std::string text;
                        };
                        std::vector<Member> members;
                        for (EntityId mid : blk->entities) {
                            const Entity* me = ctx.doc().find(mid);
                            if (!me || kind(*me) == EntityKind::Ray)
                                continue;
                            geom::DisplayContext mdc;
                            mdc.pixelSizeMm = rad / 10;
                            DisplayList ml;
                            geom::buildDisplay(ctx.doc(), *me, mdc, ml);
                            double longest = 0;
                            for (const DisplayItem& it : ml.items)
                                if (const auto* mp = std::get_if<DisplayPolyline>(&it))
                                    for (std::size_t i = 1; i < mp->pts.size(); ++i)
                                        longest = std::max(
                                            longest, std::hypot(mp->pts[i].x - mp->pts[i - 1].x,
                                                                mp->pts[i].y - mp->pts[i - 1].y));
                            const BBox2 mb = geom::bounds(*me, ctx.doc());
                            const Layer* ml2 = ctx.doc().layer(me->common.layer);
                            std::ostringstream t;
                            t << "      #" << mid.value << " " << kindName(kind(*me)) << " layer "
                              << (ml2 ? ml2->name : "?") << ": longest segment " << longest;
                            if (!mb.empty())
                                t << ", extents (" << mb.min.x << ", " << mb.min.y << ")-("
                                  << mb.max.x << ", " << mb.max.y << ")";
                            t << "\n";
                            members.push_back({longest, t.str()});
                        }
                        std::sort(
                            members.begin(), members.end(),
                            [](const Member& x, const Member& y) { return x.longest > y.longest; });
                        for (std::size_t i = 0; i < members.size() && i < 5; ++i)
                            emit(members[i].text);
                    }
                }
            }
        };

        const BlockId owner = ctx.host().drawingOwner();
        if (owner == BlockId{}) {
            search(ctx.doc().modelSpace(), at.point, radius, 0, "");
        } else if (const BlockDef* sheet = ctx.doc().block(owner)) {
            // Paper space: the sheet's own objects, then the model seen through each viewport.
            search(sheet->entities, at.point, radius, 1, "sheet ");
            for (const layout::Layout& l : layout::layouts(ctx.doc())) {
                if (l.content != owner)
                    continue;
                for (const layout::Viewport& vp : l.viewports) {
                    if (!vp.on || !layout::viewportContains(vp, at.point))
                        continue;
                    const Vec2 mid = (vp.paper.min + vp.paper.max) * 0.5;
                    const Vec2 model = vp.center + (at.point - mid) * vp.scaleN;
                    search(ctx.doc().modelSpace(), model, radius * vp.scaleN, vp.scaleN,
                           "viewport " + std::to_string(vp.id) + " ");
                }
            }
        }
        if (pick.empty()) {
            ctx.message("Nothing is drawn within that distance.");
            co_return;
        }
        ctx.message(std::to_string(pick.size()) +
                    " object(s) selected; the full list is on the "
                    "clipboard (COPYRESULT also copies it).");
        ctx.host().setSetting("clipboard.text", report.str());
        ctx.host().setSelection(pick);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePickTrace() {
    return std::make_unique<PickTrace>();
}

}  // namespace tbcad::inquiry
