// PICKTRACE: lists every object whose drawn shape passes near a picked point, with the part of it
// that is nearest, so a line that cannot be selected can be traced to its source.
#include <algorithm>
#include <cmath>
#include <sstream>
#include <variant>

#include "extra.h"
#include "tbcad/geom/entity.h"

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
        ctx.doc().forEachEntity([&](const Entity& e) {
            if (ctx.doc().ownerBlock(e.common.id) != BlockId{} || kind(e) == EntityKind::Ray)
                return;
            geom::DisplayContext dc;
            dc.pixelSizeMm = radius / 10;
            DisplayList list;
            geom::buildDisplay(ctx.doc(), e, dc, list);
            double best = radius;
            double bestLen = 0;
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
                    const double d = segmentDistance(at.point, a, b);
                    if (d <= best) {
                        best = d;
                        bestLen = std::hypot(b.x - a.x, b.y - a.y);
                        bestLayer = pl->style.layer;
                        hit = true;
                    }
                }
            }
            if (!hit)
                return;
            const Layer* own = ctx.doc().layer(e.common.layer);
            const Layer* drawn = ctx.doc().layer(bestLayer);
            std::ostringstream line;
            line << "#" << e.common.id.value << " " << kindName(kind(e)) << " layer "
                 << (own ? own->name : "?");
            if (drawn && own != drawn)
                line << " (drawn on " << drawn->name << ")";
            line << ": " << best << " away, segment length " << bestLen << "\n";
            report << line.str();
            pick.push_back(e.common.id);
            if (++lines <= 20)
                ctx.message(line.str());
        });
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
