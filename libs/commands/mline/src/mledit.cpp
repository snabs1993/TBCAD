// MLEDIT: fixes the joints of multilines: a corner join of two walls, adding or deleting a vertex,
// closing or opening the path.
#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/mline/mline.h"

namespace tbcad::mlinecmd {

namespace {

bool isMline(const Entity& e) {
    return kind(e) == EntityKind::MLine;
}

// Where the lines through (a0, a1) and (b0, b1) meet; empty when they are parallel.
std::optional<Vec2> meet(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1) {
    const Vec2 da = a1 - a0, db = b1 - b0;
    const double den = da.x * db.y - da.y * db.x;
    if (std::abs(den) < 1e-12 * (std::abs(da.x * db.y) + std::abs(da.y * db.x) + 1e-300))
        return std::nullopt;
    const Vec2 d = b0 - a0;
    const double t = (d.x * db.y - d.y * db.x) / den;
    return a0 + da * t;
}

// The end of the path nearest to `p`: true for the first vertex, false for the last.
bool nearerToStart(const MlineData& m, Vec2 p) {
    return distance(p, m.v.front()) <= distance(p, m.v.back());
}

class MlEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEDIT"; }
    std::string_view description() const override {
        return "Join multiline corners, add or delete vertices, close or open";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Corner", "Add", "Delete", "Close", "Open"};
        ko.defaultKeyword = "Corner";
        auto k = co_await ctx.getKeyword(
            "Multiline edit [Corner join/Add vertex/Delete vertex/Close/Open] <Corner>", ko);
        if (!k.ok())
            co_return;
        cmd::EntityOptions eo;
        eo.filter = isMline;
        ctx.setUndoLabel("MLEDIT");

        if (k.keyword == "Corner") {
            auto first =
                co_await ctx.getEntity("Select the first multiline (near the end to join)", eo);
            if (!first.ok())
                co_return;
            auto second =
                co_await ctx.getEntity("Select the second multiline (near the end to join)", eo);
            if (!second.ok())
                co_return;
            if (first.id == second.id) {
                ctx.message("Select two different multilines.");
                co_return;
            }
            const auto* a = entityAs<MlineData>(*ctx.doc().find(first.id));
            const auto* b = entityAs<MlineData>(*ctx.doc().find(second.id));
            if (!a || !b || a->v.size() < 2 || b->v.size() < 2 || a->closed || b->closed) {
                ctx.message("Open multilines with at least two points are needed.");
                co_return;
            }
            const bool aStart = nearerToStart(*a, first.pickPoint);
            const bool bStart = nearerToStart(*b, second.pickPoint);
            const std::size_t ae = aStart ? 0 : a->v.size() - 1, an = aStart ? 1 : a->v.size() - 2;
            const std::size_t be = bStart ? 0 : b->v.size() - 1, bn = bStart ? 1 : b->v.size() - 2;
            auto corner = meet(a->v[an], a->v[ae], b->v[bn], b->v[be]);
            if (!corner) {
                ctx.message("The two walls are parallel; they have no corner.");
                co_return;
            }
            const Vec2 c = *corner;
            (void)ctx.tx().modify(first.id, [&](Entity& e) { entityAs<MlineData>(e)->v[ae] = c; });
            (void)ctx.tx().modify(second.id, [&](Entity& e) { entityAs<MlineData>(e)->v[be] = c; });
            ctx.message("Corner joined.");
            co_return;
        }

        auto pick = co_await ctx.getEntity("Select a multiline", eo);
        if (!pick.ok())
            co_return;
        const auto* m = entityAs<MlineData>(*ctx.doc().find(pick.id));
        if (!m)
            co_return;

        if (k.keyword == "Close" || k.keyword == "Open") {
            const bool close = k.keyword == "Close";
            if (m->closed == close) {
                ctx.message(close ? "It is closed already." : "It is open already.");
                co_return;
            }
            if (close && m->v.size() < 3) {
                ctx.message("A closed multiline needs three points.");
                co_return;
            }
            (void)ctx.tx().modify(pick.id,
                                  [&](Entity& e) { entityAs<MlineData>(e)->closed = close; });
            co_return;
        }

        if (k.keyword == "Add") {
            auto at = co_await ctx.getPoint("Specify the new vertex");
            if (!at.ok())
                co_return;
            // The vertex goes into the segment nearest to the point.
            std::size_t best = 0;
            double bestDist = 1e300;
            const std::size_t segs = m->closed ? m->v.size() : m->v.size() - 1;
            for (std::size_t i = 0; i < segs; ++i) {
                const Vec2 p = m->v[i], q = m->v[(i + 1) % m->v.size()];
                const Vec2 d = q - p;
                const double len2 = d.x * d.x + d.y * d.y;
                double t = len2 > 0 ? ((at.point - p).x * d.x + (at.point - p).y * d.y) / len2 : 0;
                t = std::clamp(t, 0.0, 1.0);
                const double dist = distance(at.point, p + d * t);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i;
                }
            }
            (void)ctx.tx().modify(pick.id, [&](Entity& e) {
                auto& v = entityAs<MlineData>(e)->v;
                v.insert(v.begin() + static_cast<std::ptrdiff_t>(best) + 1, at.point);
            });
            co_return;
        }

        // Delete: the vertex nearest to the pick.
        if (m->v.size() <= 2) {
            ctx.message("A multiline keeps at least two points.");
            co_return;
        }
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < m->v.size(); ++i)
            if (distance(m->v[i], pick.pickPoint) < distance(m->v[nearest], pick.pickPoint))
                nearest = i;
        (void)ctx.tx().modify(pick.id, [&](Entity& e) {
            auto& v = entityAs<MlineData>(e)->v;
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(nearest));
            if (v.size() < 3)
                entityAs<MlineData>(e)->closed = false;
        });
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMlEditCommand() {
    return std::make_unique<MlEditCommand>();
}

}  // namespace tbcad::mlinecmd
