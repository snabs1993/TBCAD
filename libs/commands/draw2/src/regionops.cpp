#include <algorithm>

#include "commands.h"
#include "common.h"
#include "tbcad/geom/polybool.h"
#include "tbcad/modify/break_join.h"

namespace tbcad::draw2::detail {

namespace {

constexpr double kMaxChord = 0.005;  // drawing units; arcs and circles become polygons this fine
constexpr double kMinChord = 4e-7;   // above the merge distance of the boolean operations

// How finely an object is flattened: its curves deviate from the polygon by at most this much,
// which keeps areas accurate to a few parts in a million whatever the size of the shape.
double chordFor(const Entity& e) {
    std::vector<geom::CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return kMaxChord;
    BBox2 b = curves.front().bounds();
    for (const geom::CurveRef& c : curves) {
        b.expand(c.bounds().min);
        b.expand(c.bounds().max);
    }
    return std::clamp(distance(b.min, b.max) * 5e-7, kMinChord, kMaxChord);
}

// The outline of a closed object (closed polyline, circle, full ellipse, closed spline) as a
// polygon with arcs flattened. False for anything open or not a curve.
bool ringOf(const Entity& e, geom::Ring& ring) {
    switch (kind(e)) {
        case EntityKind::Polyline:
            if (!entityAs<PolylineData>(e)->closed)
                return false;
            break;
        case EntityKind::Circle:
            break;
        case EntityKind::Ellipse: {
            const auto* el = entityAs<EllipseData>(e);
            if (!tol::nearlyEqualAngle(el->startParam, el->endParam) &&
                std::fabs(el->endParam - el->startParam) < kTwoPi - 1e-9)
                return false;
            break;
        }
        case EntityKind::Spline:
            break;
        default:
            return false;
    }
    std::vector<geom::CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return false;
    if (kind(e) == EntityKind::Spline && !curves[0].isClosed())
        return false;
    return geom::outlinePoints(e, chordFor(e), ring) && ring.size() >= 3;
}

bool closedObject(const Entity& e) {
    geom::Ring r;
    return ringOf(e, r);
}

// Everything the selection describes as one region (the union of its objects).
geom::Region regionOfSelection(cmd::CommandContext& ctx, const std::vector<EntityId>& ids,
                               int& used) {
    geom::Region acc;
    used = 0;
    for (EntityId id : ids) {
        const Entity* e = ctx.doc().find(id);
        geom::Ring r;
        if (!e || !ringOf(*e, r))
            continue;
        geom::Region one;
        one.rings.push_back(std::move(r));
        acc = used == 0 ? geom::normalized(one) : geom::boolean(acc, one, geom::BoolOp::Union);
        ++used;
    }
    return acc;
}

// The ring as a closed polyline, with the arcs of the original objects brought back. A ring that
// is a single circle comes back as a circle.
Entity ringEntity(const geom::Ring& ring, double chord, const EntityCommon& common,
                  bool allowCircle = true) {
    const geom::ArcFit fit = geom::fitArcs(ring, chord);
    Entity e;
    if (fit.circle && !allowCircle) {
        // Two half circles: the outline as a closed polyline.
        PolylineData d;
        d.closed = true;
        for (double sign : {1.0, -1.0}) {
            PolyVertex v;
            v.p = fit.center + Vec2{fit.radius * sign, 0};
            v.bulge = (ring.size() > 2 && cross(ring[1] - ring[0], ring[2] - ring[1]) < 0) ? -1 : 1;
            d.v.push_back(v);
        }
        e.data = std::move(d);
    } else if (fit.circle) {
        e.data = CircleData{fit.center, fit.radius};
    } else {
        PolylineData d;
        d.closed = true;
        for (const geom::ArcVertex& av : fit.vertices) {
            PolyVertex v;
            v.p = av.p;
            v.bulge = av.bulge;
            d.v.push_back(v);
        }
        e.data = std::move(d);
    }
    e.common = common;
    e.common.id = {};
    return e;
}

// Replaces the sources by the polygons of `result`, which take the first source's properties.
void replace(cmd::CommandContext& ctx, const std::vector<EntityId>& sources,
             const geom::Region& result) {
    const Entity* first = sources.empty() ? nullptr : ctx.doc().find(sources.front());
    const EntityCommon common = first ? first->common : EntityCommon{};
    double chord = kMinChord;
    for (EntityId id : sources)
        if (const Entity* e = ctx.doc().find(id))
            chord = std::max(chord, chordFor(*e));
    for (EntityId id : sources)
        (void)ctx.tx().erase(id);
    for (const geom::Ring& r : result.rings) {
        auto id = ctx.tx().add(ringEntity(r, chord, common));
        if (!id)
            ctx.message(id.error().message);
    }
}

// A curve that may be one piece of a closed loop.
bool openPiece(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
            return true;
        case EntityKind::Polyline:
            return !entityAs<PolylineData>(e)->closed;
        default:
            return false;
    }
}

cmd::SelectionOptions closedOnly() {
    cmd::SelectionOptions so;
    so.filter = [](const Entity& e) { return closedObject(e); };
    return so;
}

// REGION: closed objects become plain closed polylines (the region's outline); the originals go.
class RegionCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "REGION"; }
    std::vector<std::string> defaultAliases() const override { return {"REG"}; }
    std::string_view description() const override {
        return "Turn closed objects into closed polylines (regions)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return closedObject(e) || openPiece(e); };
        auto sel = co_await ctx.getSelection(
            "Select closed objects or lines and arcs that form loops", so);
        if (!sel.ok() || sel.ids.empty())
            co_return;
        int made = 0;
        // Lines, arcs and open polylines that meet end to end become closed loops first.
        std::vector<EntityId> pieces;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && openPiece(*e))
                pieces.push_back(id);
        if (!pieces.empty()) {
            std::vector<const Entity*> es;
            for (EntityId id : pieces)
                es.push_back(ctx.doc().find(id));
            modify::JoinResult joined = modify::joinEntities(es, 1e-4);
            const auto isLoop = [](const Entity& e) {
                const auto* pl = entityAs<PolylineData>(e);
                return kind(e) == EntityKind::Circle || (pl && pl->closed);
            };
            if (!joined.created.empty() &&
                std::all_of(joined.created.begin(), joined.created.end(),
                            [&](const Entity& e) { return isLoop(e); })) {
                ctx.checkpoint();
                for (std::size_t i : joined.consumed)
                    (void)ctx.tx().erase(pieces[i]);
                for (Entity& e : joined.created) {
                    // Circles from two arcs come out as plain closed polylines like all regions.
                    if (ctx.tx().add(std::move(e)))
                        ++made;
                }
            } else {
                ctx.message("Those lines and arcs do not form closed loops.");
            }
        }
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            geom::Ring r;
            if (!e || !ringOf(*e, r))
                continue;
            const geom::Region norm = geom::normalized({{r}});
            Entity out =
                ringEntity(norm.rings.empty() ? r : norm.rings[0], chordFor(*e), e->common, false);
            if (!made)
                ctx.checkpoint();
            (void)ctx.tx().erase(id);
            if (ctx.tx().add(std::move(out)))
                ++made;
        }
        ctx.message(std::to_string(made) + (made == 1 ? " region created." : " regions created."));
    }
};

class BoolCommand final : public cmd::Command {
public:
    BoolCommand(std::string name, std::string alias, std::string description, geom::BoolOp op)
        : name_(std::move(name)),
          alias_(std::move(alias)),
          description_(std::move(description)),
          op_(op) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        if (op_ == geom::BoolOp::Subtract) {
            auto base = co_await ctx.getSelection("Select regions to subtract from", closedOnly());
            if (!base.ok() || base.ids.empty())
                co_return;
            auto cut = co_await ctx.getSelection("Select regions to subtract", closedOnly());
            if (!cut.ok() || cut.ids.empty())
                co_return;
            int nb = 0, nc = 0;
            const geom::Region a = regionOfSelection(ctx, base.ids, nb);
            const geom::Region b = regionOfSelection(ctx, cut.ids, nc);
            if (!nb || !nc) {
                ctx.message("Nothing to subtract: select closed objects.");
                co_return;
            }
            const geom::Region r = geom::boolean(a, b, op_);
            ctx.checkpoint();
            std::vector<EntityId> all = base.ids;
            // Only the objects that were used go; the subtracted ones go too, as in AutoCAD.
            all.insert(all.end(), cut.ids.begin(), cut.ids.end());
            replace(ctx, all, r);
            ctx.message(r.empty() ? "The result is empty."
                                  : std::to_string(r.rings.size()) + " polygon(s) created.");
            co_return;
        }
        auto sel = co_await ctx.getSelection("Select regions", closedOnly());
        if (!sel.ok() || sel.ids.size() < 2) {
            if (sel.ok())
                ctx.message("Select at least two closed objects.");
            co_return;
        }
        geom::Region acc;
        int n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            geom::Ring ring;
            if (!e || !ringOf(*e, ring))
                continue;
            geom::Region one;
            one.rings.push_back(std::move(ring));
            acc = n == 0 ? geom::normalized(one) : geom::boolean(acc, one, op_);
            ++n;
        }
        if (n < 2) {
            ctx.message("Select at least two closed objects.");
            co_return;
        }
        ctx.checkpoint();
        replace(ctx, sel.ids, acc);
        ctx.message(acc.empty() ? "The result is empty."
                                : std::to_string(acc.rings.size()) + " polygon(s) created.");
    }

private:
    std::string name_, alias_, description_;
    geom::BoolOp op_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeRegion() {
    return std::make_unique<RegionCommand>();
}
std::unique_ptr<cmd::Command> makeUnion() {
    return std::make_unique<BoolCommand>("UNION", "UNI", "Merge closed objects into one region",
                                         geom::BoolOp::Union);
}
std::unique_ptr<cmd::Command> makeSubtract() {
    return std::make_unique<BoolCommand>(
        "SUBTRACT", "SU", "Cut closed objects out of other closed objects", geom::BoolOp::Subtract);
}
std::unique_ptr<cmd::Command> makeIntersect() {
    return std::make_unique<BoolCommand>(
        "INTERSECT", "IN", "Keep only the area shared by closed objects", geom::BoolOp::Intersect);
}

}  // namespace tbcad::draw2::detail
