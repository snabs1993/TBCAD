// DIMCONTINUE, DIMBASELINE, DIMSPACE and DIMTEDIT: chains of linear dimensions and dimension text
// editing.
#include <algorithm>
#include <cmath>
#include <optional>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/box_grid.h"

namespace tbcad::dimcmd {

namespace {

bool isLengthDim(const Entity& e) {
    const auto* d = entityAs<DimensionData>(e);
    return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned);
}

bool isAngularDim(const Entity& e) {
    const auto* d = entityAs<DimensionData>(e);
    return d && (d->kind == DimKind::Angular2Line || d->kind == DimKind::Angular3Point);
}

// The point that sets an angular dimension's arc radius: the arc point of a two-line one (also
// its text point), the fourth point of a three-point one.
Vec2& angularArcPoint(DimensionData& d) {
    return d.kind == DimKind::Angular2Line ? d.textPos : d.defPts[3];
}

bool isAnyDim(const Entity& e) {
    return kind(e) == EntityKind::Dimension;
}

Vec2 unitAt(double a) {
    return {std::cos(a), std::sin(a)};
}

double angleOf(Vec2 v) {
    return std::atan2(v.y, v.x);
}

bool isAngularDim(const DimensionData& d) {
    return d.kind == DimKind::Angular2Line || d.kind == DimKind::Angular3Point;
}

// Dimensions DIMCONTINUE and DIMBASELINE build on, as in AutoCAD: linear, aligned, angular and
// ordinate.
bool isChainDim(const Entity& e) {
    const auto* d = entityAs<DimensionData>(e);
    return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned ||
                 d->kind == DimKind::Ordinate || isAngularDim(*d));
}

// The most recently drawn dimension of model space that a chain can continue.
EntityId lastChainDim(const Document& doc) {
    const auto& ms = doc.modelSpace();
    for (auto it = ms.rbegin(); it != ms.rend(); ++it) {
        const Entity* e = doc.find(*it);
        if (e && isChainDim(*e))
            return *it;
    }
    return EntityId{};
}

// Unit vector across the dimension line and the side (+1/-1) the line is on.
Vec2 normalOf(const DimensionData& d) {
    return Vec2{-std::sin(d.rotation), std::cos(d.rotation)};
}

double sideOf(const DimensionData& d) {
    return dot(d.defPts[2] - d.defPts[0], normalOf(d)) >= 0 ? 1.0 : -1.0;
}

// The next dimension of a chain from `prev` to the picked point. Linear and aligned: from the
// second (continue) or first (baseline) extension line. Angular: around the same vertex, from the
// side of the previous arc nearest the pick (continue, same radius) or the side away from it
// (baseline, one step further out). Ordinate: the same datum and axis, the leader ending in line
// with the previous one.
DimensionData chainNext(const DimensionData& prev, Vec2 pick, bool baseline, double spacing) {
    DimensionData d = prev;
    d.textPosUser = false;
    d.textOverride.clear();
    d.breaks.clear();
    d.jogs.clear();
    d.assoc = {};
    if (prev.kind == DimKind::Ordinate) {
        const Vec2 n = Vec2{-std::sin(prev.rotation), std::cos(prev.rotation)};  // leader direction
        d.defPts[0] = pick;
        d.defPts[1] = pick + n * dot(prev.defPts[1] - pick, n);
        return d;
    }
    if (isAngularDim(prev)) {
        const dim::AngularGeometry g = dim::angularGeometry(prev);
        const Vec2 dirs[2] = {unitAt(g.startAngle), unitAt(g.startAngle + g.sweep)};
        const double a = angleOf(pick - g.vertex);
        auto gapTo = [&](Vec2 dir) {
            const double t = std::abs(normalizeAngle(a - angleOf(dir)));
            return std::min(t, kTwoPi - t);
        };
        const bool nearEnd = gapTo(dirs[1]) <= gapTo(dirs[0]);
        // continue: from the side nearest the pick; baseline: from the other one
        const Vec2 from = dirs[nearEnd != baseline ? 1 : 0];
        const double r = g.radius + (baseline ? spacing : 0.0);
        d.kind = DimKind::Angular3Point;
        d.defPts[2] = g.vertex;
        d.defPts[0] = g.vertex + from * r;
        d.defPts[1] = pick;
        // the arc point halfway round the smaller angle between the two sides
        const Vec2 to = normalized(pick - g.vertex);
        Vec2 mid = normalized(from + to);
        if (tol::isZeroLength(mid))
            mid = perp(from);
        d.defPts[3] = g.vertex + mid * r;
        return d;
    }
    const Vec2 origin = baseline ? prev.defPts[0] : prev.defPts[1];
    d.defPts[0] = origin;
    d.defPts[1] = pick;
    d.defPts[2] =
        baseline ? prev.defPts[2] + normalOf(prev) * (sideOf(prev) * spacing) : prev.defPts[2];
    return d;
}

bool degenerateChain(const DimensionData& d) {
    if (d.kind == DimKind::Ordinate)
        return false;
    if (isAngularDim(d))
        return distance(d.defPts[1], d.defPts[2]) < tol::kLinear ||
               std::abs(cross(normalized(d.defPts[0] - d.defPts[2]),
                              normalized(d.defPts[1] - d.defPts[2]))) < 1e-9;
    return distance(d.defPts[0], d.defPts[1]) < tol::kLinear;
}

class ChainCommand final : public cmd::Command {
public:
    ChainCommand(std::string name, std::string alias, bool baseline, std::string description)
        : name_(std::move(name)),
          alias_(std::move(alias)),
          baseline_(baseline),
          description_(std::move(description)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        EntityId baseId = lastChainDim(ctx.doc());
        // What this run made, with the base each one was built on, for Undo.
        std::vector<std::pair<EntityId, EntityId>> made;
        for (;;) {
            if (!baseId) {
                cmd::EntityOptions eo;
                eo.filter = isChainDim;
                auto o = co_await ctx.getEntity(
                    baseline_ ? "Select base dimension" : "Select continued dimension", eo);
                if (!o.ok())
                    co_return;
                baseId = o.id;
            }
            const Entity* be = ctx.doc().find(baseId);
            const auto* base = be ? entityAs<DimensionData>(*be) : nullptr;
            if (!base || !isChainDim(*be)) {
                ctx.message("Select a linear, aligned, angular or ordinate dimension.");
                co_return;
            }
            const DimensionData prev = *base;
            const DimStyle& st = dim::drawStyleOf(ctx.doc(), prev);
            const double k = st.overallScale > 0 ? st.overallScale : 1.0;
            const double spacing =
                st.baselineSpacing > 0 ? st.baselineSpacing * k : st.textHeight * 2.5 * k;
            const bool ordinate = prev.kind == DimKind::Ordinate;

            auto* c = &ctx;
            const bool baseline = baseline_;
            auto build = [=](Vec2 pick) { return chainNext(prev, pick, baseline, spacing); };
            cmd::PointOptions po;
            if (!ordinate && !isAngularDim(prev))
                po.base = prev.defPts[1];
            po.keywords = {"Undo", "Select"};
            po.preview = [c, build](Vec2 cursor, DisplayList& out) {
                DimensionData d = build(cursor);
                dim::refresh(c->doc(), d);
                Entity e;
                e.data = Indirect<DimensionData>(std::move(d));
                previewEntity(*c, std::move(e), out);
            };
            auto next = co_await ctx.getPoint(
                ordinate ? "Specify feature location or [Undo/Select]"
                         : "Specify second extension line origin or [Undo/Select]",
                po);
            if (next.status == Status::Keyword && next.keyword == "Undo") {
                if (made.empty()) {
                    ctx.message("Nothing to undo.");
                    continue;
                }
                (void)ctx.tx().erase(made.back().first);
                baseId = made.back().second;
                made.pop_back();
                continue;
            }
            if (next.status == Status::Keyword) {
                baseId = EntityId{};
                continue;
            }
            if (!next.ok())
                co_return;
            DimensionData d = build(next.point);
            if (degenerateChain(d)) {
                ctx.message("The extension line origins coincide.");
                continue;
            }
            linkDimension(ctx.doc(), d, ctx.tx().defaultOwner());
            dim::refresh(ctx.doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            applyAnnotativeDefault(ctx.doc().header(), e);
            ctx.setUndoLabel(std::string(ctx.commandName()));
            auto id = ctx.tx().add(std::move(e));
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
            made.emplace_back(id.value(), baseId);
            // The next one chains from the dimension just made.
            baseId = id.value();
        }
    }

private:
    std::string name_, alias_;
    bool baseline_;
    std::string description_;
};

// Signed distance of a length dimension's line along `n` (a unit vector across the dimension).
double lineOffset(const DimensionData& d, Vec2 n) {
    return dot(d.defPts[2], n);
}

class SpaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMSPACE"; }
    std::vector<std::string> defaultAliases() const override { return {"DSP"}; }
    std::string_view description() const override {
        return "Even out the spacing between parallel linear dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return isLengthDim(e) || isAngularDim(e); };
        auto base = co_await ctx.getEntity("Select base dimension", eo);
        if (!base.ok())
            co_return;
        const Entity* be = ctx.doc().find(base.id);
        const auto* b = be ? entityAs<DimensionData>(*be) : nullptr;
        if (!b)
            co_return;
        const DimensionData baseDim = *b;
        // Angular dimensions about the same vertex are spaced by their arc radius, as in AutoCAD.
        const bool angular = isAngularDim(*be);
        const dim::AngularGeometry baseArc =
            angular ? dim::angularGeometry(baseDim) : dim::AngularGeometry{};
        if (angular && !baseArc.ok)
            co_return;
        const Vec2 n = angular ? Vec2{} : normalOf(baseDim);
        const double baseOff = angular ? baseArc.radius : lineOffset(baseDim, n);

        cmd::SelectionOptions so;
        so.allowPreselection = false;
        so.filter = [angular](const Entity& e) {
            return angular ? isAngularDim(e) : isLengthDim(e);
        };
        auto sel = co_await ctx.getSelection("Select dimensions to space", so);
        if (!sel.ok())
            co_return;

        struct Item {
            EntityId id;
            double off;
        };
        std::vector<Item> items;
        for (EntityId id : sel.ids) {
            if (id == base.id)
                continue;
            const Entity* e = ctx.doc().find(id);
            const auto* d = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d)
                continue;
            if (angular) {
                const dim::AngularGeometry g = dim::angularGeometry(*d);
                if (!isAngularDim(*e) || !g.ok ||
                    distance(g.vertex, baseArc.vertex) > tol::kLinear * (1 + baseArc.radius))
                    continue;  // not about the same vertex
                items.push_back({id, g.radius});
                continue;
            }
            if (!isLengthDim(*e))
                continue;
            const Vec2 dn = normalOf(*d);
            if (std::abs(cross(dn, n)) > 1e-6)
                continue;  // not parallel to the base
            const double sign = dot(dn, n) >= 0 ? 1.0 : -1.0;
            items.push_back({id, sign * lineOffset(*d, dn)});
        }
        if (items.empty()) {
            ctx.message(
                angular ? "No angular dimensions about the base dimension's vertex were selected."
                        : "No dimensions parallel to the base dimension were selected.");
            co_return;
        }

        const DimStyle& st = dim::drawStyleOf(ctx.doc(), baseDim);
        const double autoGap = 2.0 * st.textHeight * (st.overallScale > 0 ? st.overallScale : 1.0);
        cmd::DistanceOptions dopt;
        dopt.keywords = {"Auto"};
        dopt.defaultValue = autoGap;
        auto gap = co_await ctx.getDistance("Enter value or [Auto] <Auto>", dopt);
        double spacing = autoGap;
        if (gap.status == Status::Ok)
            spacing = gap.value;
        else if (!(gap.status == Status::Keyword && gap.keyword == "Auto"))
            co_return;

        std::vector<Item> above, below;
        for (const Item& it : items)
            (it.off >= baseOff ? above : below).push_back(it);
        std::sort(above.begin(), above.end(),
                  [](const Item& a, const Item& c) { return a.off < c.off; });
        std::sort(below.begin(), below.end(),
                  [](const Item& a, const Item& c) { return a.off > c.off; });

        ctx.setUndoLabel("DIMSPACE");
        auto place = [&](const std::vector<Item>& list, double dir) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                const double target = baseOff + dir * spacing * static_cast<double>(i + 1);
                const double delta = target - list[i].off;
                (void)ctx.tx().modify(list[i].id, [&](Entity& ent) {
                    auto* d = entityAs<DimensionData>(ent);
                    if (!d)
                        return;
                    if (angular) {
                        // The arc point moves along its radius to the new arc.
                        if (!(target > tol::kLinear))
                            return;
                        Vec2& arc = angularArcPoint(*d);
                        const Vec2 w = arc - baseArc.vertex;
                        if (tol::isZeroLength(w))
                            return;
                        arc = baseArc.vertex + normalized(w) * target;
                        dim::refresh(ctx.doc(), *d);
                        return;
                    }
                    const Vec2 dn = normalOf(*d);
                    const double sign = dot(dn, n) >= 0 ? 1.0 : -1.0;
                    const Vec2 move = dn * (sign * delta);
                    d->defPts[2] = d->defPts[2] + move;
                    if (d->textPosUser)
                        d->textPos = d->textPos + move;
                    dim::refresh(ctx.doc(), *d);
                });
            }
        };
        place(above, 1.0);
        place(below, -1.0);
        ctx.message("Spaced " + std::to_string(items.size()) + " dimensions.");
    }
};

// QDIM: one chain of linear dimensions through the key points of the selected objects.
class QuickDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "QDIM"; }
    std::string_view description() const override {
        return "Dimension the key points of the selected objects in one go";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.allowPreselection = true;
        auto sel = co_await ctx.getSelection("Select geometry to dimension", so);
        if (!sel.ok())
            co_return;
        std::vector<Vec2> pts;
        // Linear, aligned and ordinate dimensions in the selection give their extension line
        // origins and are replaced by the new ones, as in AutoCAD (QDIM edits existing
        // dimensions this way).
        std::vector<EntityId> replaced;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            if (const auto* d = entityAs<DimensionData>(*e)) {
                if (d->kind == DimKind::Linear || d->kind == DimKind::Aligned) {
                    pts.push_back(d->defPts[0]);
                    pts.push_back(d->defPts[1]);
                    replaced.push_back(id);
                } else if (d->kind == DimKind::Ordinate) {
                    pts.push_back(d->defPts[0]);
                    replaced.push_back(id);
                }
            } else if (const auto* l = entityAs<LineData>(*e)) {
                pts.push_back(l->a);
                pts.push_back(l->b);
            } else if (const auto* p = entityAs<PolylineData>(*e)) {
                for (const auto& v : p->v)
                    pts.push_back(v.p);
            } else if (const auto* c = entityAs<CircleData>(*e)) {
                pts.push_back(c->center);
            } else if (const auto* a = entityAs<ArcData>(*e)) {
                pts.push_back(a->center);
            } else if (const auto* pt = entityAs<PointData>(*e)) {
                pts.push_back(pt->p);
            }
        }
        // seTtings Intersection: the points where the selected objects cross count too.
        auto addIntersections = [&] {
            std::vector<geom::EntityIntersection> hits;
            for (std::size_t i = 0; i < sel.ids.size(); ++i)
                for (std::size_t j = i + 1; j < sel.ids.size(); ++j) {
                    const Entity* a = ctx.doc().find(sel.ids[i]);
                    const Entity* b = ctx.doc().find(sel.ids[j]);
                    if (!a || !b || kind(*a) == EntityKind::Dimension ||
                        kind(*b) == EntityKind::Dimension)
                        continue;
                    hits.clear();
                    if (geom::intersectEntities(*a, *b, hits))
                        for (const auto& h : hits)
                            pts.push_back(h.p);
                }
        };
        // Horizontal and vertical are the UCS axes: work in the UCS frame for the box and the keys.
        const double ucs = ctx.host().ucsAngle();
        const double uc = std::cos(-ucs), us = std::sin(-ucs);
        auto inUcs = [&](Vec2 p) { return Vec2{p.x * uc - p.y * us, p.x * us + p.y * uc}; };
        BBox2 box;
        for (const Vec2& p : pts)
            box.expand(inUcs(p));
        if (pts.size() < 2 || box.empty()) {
            ctx.message("Nothing to dimension.");
            co_return;
        }
        enum class Mode { Continuous, Staggered, Baseline, Ordinate };
        Mode mode = Mode::Continuous;
        bool intersections = false;
        std::optional<Vec2> datum;  // datumPoint: where Baseline starts and Ordinate measures from
        cmd::PointResult at;
        for (;;) {
            static constexpr const char* kModeNames[] = {"Continuous", "Staggered", "Baseline",
                                                         "Ordinate"};
            cmd::PointOptions po;
            po.keywords = {"Continuous", "Staggered",  "Baseline", "Ordinate", "Radius",
                           "Diameter",   "datumPoint", "Edit",     "seTtings"};
            at = co_await ctx.getPoint(
                std::string("Specify dimension line position or [Continuous/Staggered/Baseline/"
                            "Ordinate/Radius/Diameter/datumPoint/Edit/seTtings] <") +
                    kModeNames[static_cast<int>(mode)] + ">",
                po);
            if (at.status != cmd::Status::Keyword)
                break;
            if (at.keyword == "seTtings") {
                cmd::KeywordOptions ko;
                ko.keywords = {"Endpoint", "Intersection"};
                auto pr = co_await ctx.getKeyword(
                    "Associative dimension priority [Endpoint/Intersection] <Endpoint>", ko);
                if (pr.status == cmd::Status::Keyword && pr.keyword == "Intersection" &&
                    !intersections) {
                    intersections = true;
                    addIntersections();
                }
                continue;
            }
            if (at.keyword == "Edit") {
                // Remove key points by picking them, or Add new ones, until eXit or Enter.
                bool adding = false;
                const double px = ctx.host().view().scale;
                for (;;) {
                    cmd::PointOptions eo;
                    eo.keywords = {adding ? "Remove" : "Add", "eXit"};
                    auto ep = co_await ctx.getPoint(
                        adding ? "Indicate dimension point to add, or [Remove/eXit] <eXit>"
                               : "Indicate dimension point to remove, or [Add/eXit] <eXit>",
                        eo);
                    if (ep.status == cmd::Status::Keyword) {
                        if (ep.keyword == "eXit")
                            break;
                        adding = ep.keyword == "Add";
                        continue;
                    }
                    if (!ep.ok())
                        break;
                    if (adding) {
                        pts.push_back(ep.point);
                        ctx.message("One dimension point added.");
                        continue;
                    }
                    // The nearest key point within a few pixels (or a little of the drawing).
                    BBox2 all;
                    for (const Vec2& p : pts)
                        all.expand(p);
                    const double reach =
                        px > 0 ? 10 * px : 0.02 * std::max(all.width(), all.height());
                    std::size_t best = pts.size();
                    double bestD = reach;
                    for (std::size_t i = 0; i < pts.size(); ++i)
                        if (const double dd = distance(pts[i], ep.point); dd <= bestD) {
                            bestD = dd;
                            best = i;
                        }
                    if (best == pts.size()) {
                        ctx.message("No dimension point there.");
                        continue;
                    }
                    // Every copy of that point goes (objects meeting there each gave one).
                    const Vec2 gone = pts[best];
                    std::erase_if(pts, [&](Vec2 q) { return distance(q, gone) <= tol::kLinear; });
                    ctx.message("One dimension point removed.");
                }
                continue;
            }
            if (at.keyword == "datumPoint") {
                auto dp = co_await ctx.getPoint("Select new datum point");
                if (!dp.ok())
                    co_return;
                datum = dp.point;
                continue;
            }
            if (at.keyword != "Radius" && at.keyword != "Diameter") {
                for (int m = 0; m < 4; ++m)
                    if (at.keyword == kModeNames[m])
                        mode = static_cast<Mode>(m);
                continue;
            }
            // Radius and Diameter dimension every circle and arc of the selection at once.
            const bool radius = at.keyword == "Radius";
            int made = 0;
            for (EntityId id : sel.ids) {
                const Entity* e = ctx.doc().find(id);
                if (!e)
                    continue;
                Vec2 centre{};
                double r = 0;
                if (const auto* c = entityAs<CircleData>(*e)) {
                    centre = c->center;
                    r = c->radius;
                } else if (const auto* a = entityAs<ArcData>(*e)) {
                    centre = a->center;
                    r = a->radius;
                } else {
                    continue;
                }
                const Vec2 dir{std::cos(kPi / 4), std::sin(kPi / 4)};
                DimensionData d;
                d.kind = radius ? DimKind::Radius : DimKind::Diameter;
                if (radius) {
                    d.defPts = {centre, centre + dir * r, Vec2{}, Vec2{}};
                } else {
                    d.defPts = {centre - dir * r, centre + dir * r, Vec2{}, Vec2{}};
                }
                dim::refresh(ctx.doc(), d);
                if (!addDim(ctx, std::move(d)))
                    co_return;
                ++made;
            }
            ctx.message(made ? std::to_string(made) + " dimensions created."
                             : "No circles or arcs in the selection.");
            co_return;
        }
        if (!at.ok())
            co_return;
        // Edit may have changed the key points.
        box = BBox2{};
        for (const Vec2& p : pts)
            box.expand(inUcs(p));
        if (pts.size() < 2 || box.empty()) {
            ctx.message("Nothing to dimension.");
            co_return;
        }
        // Outside the box vertically: horizontal dimensions; otherwise vertical ones.
        const Vec2 atU = inUcs(at.point);
        const bool horizontal = atU.y > box.max.y || atU.y < box.min.y;
        const double rotation = ucs + (horizontal ? 0.0 : kPi / 2);
        auto key = [&](Vec2 p) { return horizontal ? inUcs(p).x : inUcs(p).y; };
        if (datum && mode != Mode::Ordinate)
            pts.push_back(*datum);
        std::sort(pts.begin(), pts.end(), [&](Vec2 a, Vec2 b) { return key(a) < key(b); });
        std::vector<Vec2> uniq;
        for (const Vec2& p : pts)
            if (uniq.empty() || std::abs(key(p) - key(uniq.back())) > tol::kLinear)
                uniq.push_back(p);
        const DimStyle& st = dim::styleOf(ctx.doc(), ctx.doc().header().currentDimStyle);
        const double k = st.overallScale > 0 ? st.overallScale : 1.0;
        const double spacing =
            st.baselineSpacing > 0 ? st.baselineSpacing * k : st.textHeight * 2.5 * k;
        const Vec2 normal{-std::sin(rotation), std::cos(rotation)};
        const Vec2 offU = atU - box.center();
        const double side = (horizontal ? offU.y : offU.x) >= 0 ? 1.0 : -1.0;
        auto retire = [&] {
            for (EntityId id : replaced)
                (void)ctx.tx().erase(id);
        };
        if (mode == Mode::Ordinate) {
            retire();
            // One ordinate dimension per key point, measured from the datum (the origin unless
            // datumPoint moved it), the leaders ending on the line through the picked point.
            const Vec2 origin = datum ? *datum : Vec2{0, 0};
            for (const Vec2& p : uniq) {
                DimensionData d;
                d.kind = DimKind::Ordinate;
                d.rotation = rotation;
                d.defPts = {p, p + normal * dot(at.point - p, normal), origin, Vec2{}};
                dim::refresh(ctx.doc(), d);
                if (!addDim(ctx, std::move(d)))
                    co_return;
            }
            ctx.message(std::to_string(uniq.size()) + " dimensions created.");
            co_return;
        }
        if (uniq.size() < 2) {
            ctx.message("The key points are all at the same place along that direction.");
            co_return;
        }
        retire();
        if (mode == Mode::Staggered) {
            // Pairs from the outside in: the innermost pair sits at the picked line, each pair
            // further out one step further from the objects.
            const std::size_t pairs = uniq.size() / 2;
            for (std::size_t i = 0; i < pairs; ++i) {
                DimensionData d;
                d.kind = DimKind::Linear;
                d.rotation = rotation;
                const double out = side * spacing * double(pairs - 1 - i);
                d.defPts = {uniq[i], uniq[uniq.size() - 1 - i], at.point + normal * out, Vec2{}};
                dim::refresh(ctx.doc(), d);
                if (!addDim(ctx, std::move(d)))
                    co_return;
            }
            ctx.message(std::to_string(pairs) + " dimensions created.");
            co_return;
        }
        // Baseline starts at the datum when one was given, else at the first key point.
        std::size_t first = 0;
        if (mode == Mode::Baseline && datum)
            for (std::size_t i = 0; i < uniq.size(); ++i)
                if (std::abs(key(uniq[i]) - key(*datum)) <= tol::kLinear)
                    first = i;
        std::size_t step = 0;
        for (std::size_t i = 0; i + 1 < uniq.size(); ++i) {
            DimensionData d;
            d.kind = DimKind::Linear;
            d.rotation = rotation;
            if (mode == Mode::Baseline) {
                // Every dimension starts at the base point; each sits one step further out.
                const std::size_t to = i < first ? i : i + 1;
                d.defPts = {uniq[first], uniq[to],
                            at.point + normal * (side * spacing * double(step++)), Vec2{}};
            } else {
                d.defPts = {uniq[i], uniq[i + 1], at.point, Vec2{}};
            }
            dim::refresh(ctx.doc(), d);
            if (!addDim(ctx, std::move(d)))
                co_return;
        }
        ctx.message(std::to_string(uniq.size() - 1) + " dimensions created.");
    }
};

// Where the dimension line of a linear or aligned dimension runs: its two ends and direction.
struct DimLine {
    Vec2 a{}, b{}, u{};
};
std::optional<DimLine> dimLineOf(const DimensionData& d) {
    if (d.kind != DimKind::Linear && d.kind != DimKind::Aligned)
        return std::nullopt;
    Vec2 u = d.kind == DimKind::Linear ? Vec2{std::cos(d.rotation), std::sin(d.rotation)}
                                       : normalized(d.defPts[1] - d.defPts[0]);
    if (tol::isZeroLength(u))
        u = {1, 0};
    const Vec2 n = perp(u);
    const Vec2 e =
        d.oblique == 0 ? n : normalized(n * std::cos(d.oblique) - u * std::sin(d.oblique));
    const double lean = dot(e, n);
    const Vec2 P = d.defPts[2];
    return DimLine{d.defPts[0] + e * (dot(P - d.defPts[0], n) / lean),
                   d.defPts[1] + e * (dot(P - d.defPts[1], n) / lean), u};
}

// DIMTEDIT Left / Right / Center: the text point on the dimension line, next to the extension
// line on the left (as the text reads) or the right, or in the middle.
std::optional<Vec2> justifiedText(const Document& doc, const DimensionData& d,
                                  std::string_view where) {
    const auto line = dimLineOf(d);
    if (!line)
        return std::nullopt;
    if (where == "Center")
        return lerp(line->a, line->b, 0.5);
    const DimStyle& st = dim::drawStyleOf(doc, d);
    const double k = st.overallScale > 0 ? st.overallScale : 1.0;
    const double edge =
        2 * st.arrowSize * k + st.textGap * k + dim::layoutDimension(doc, d).text.width / 2;
    // "Left" as the text reads: text along a line reads left to right or bottom to top.
    Vec2 t = line->u;
    if (t.x < -1e-9 || (std::abs(t.x) <= 1e-9 && t.y < 0))
        t = -t;
    const bool aFirst = dot(line->a, t) <= dot(line->b, t);
    const Vec2 left = aFirst ? line->a : line->b, right = aFirst ? line->b : line->a;
    const double len = distance(left, right);
    if (len < 2 * edge)
        return lerp(left, right, 0.5);
    return where == "Left" ? left + t * edge : right - t * edge;
}

class TextEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"DTE"}; }
    std::string_view description() const override {
        return "Move or turn dimension text, line it up left, right or centre, or reset it to its "
               "automatic place";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isAnyDim;
        auto o = co_await ctx.getEntity("Select dimension", eo);
        if (!o.ok())
            co_return;
        const Entity* e = ctx.doc().find(o.id);
        const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
        if (!d0)
            co_return;
        const DimensionData original = *d0;
        const EntityId id = o.id;

        cmd::PointOptions po;
        po.keywords = {"Left", "Right", "Center", "Home", "Angle"};
        auto* c = &ctx;
        po.preview = [c, original](Vec2 cursor, DisplayList& out) {
            DimensionData d = original;
            dim::placeText(c->doc(), d, cursor);
            dim::refresh(c->doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            previewEntity(*c, std::move(e), out);
        };
        auto where = co_await ctx.getPoint(
            "Specify new location for dimension text or [Left/Right/Center/Home/Angle]", po);
        const std::string kw = where.status == Status::Keyword ? where.keyword : std::string();
        if (!where.ok() && kw.empty())
            co_return;
        double angle = 0;
        if (kw == "Angle") {
            auto a = co_await ctx.getAngle("Specify angle for dimension text, 0 = default",
                                           cmd::AngleOptions{.defaultValue = 0.0});
            if (!a.ok())
                co_return;
            angle = a.value;
        }
        std::optional<Vec2> lined;
        if (kw == "Left" || kw == "Right" || kw == "Center") {
            lined = justifiedText(ctx.doc(), original, kw);
            if (!lined) {
                ctx.message(
                    "Left, Right and Center line up the text of linear and aligned "
                    "dimensions only.");
                co_return;
            }
        }
        ctx.setUndoLabel("DIMTEDIT");
        auto r = ctx.tx().modify(id, [&](Entity& ent) {
            auto* d = entityAs<DimensionData>(ent);
            if (!d)
                return;
            if (kw == "Home") {
                d->textPosUser = false;
            } else if (kw == "Angle") {
                d->textRotated = std::abs(angle) > 1e-9;
                d->textRotation = d->textRotated ? angle : 0;
            } else if (lined) {
                d->textPos = *lined;
                d->textPosUser = true;
            } else {
                dim::placeText(ctx.doc(), *d, where.point);
            }
            dim::refresh(ctx.doc(), *d);
        });
        if (!r)
            ctx.message(r.error().message);
    }
};

class EditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"DED"}; }
    std::string_view description() const override {
        return "Replace the text of dimensions, return the text to its automatic place and angle, "
               "rotate the text or lean the extension lines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::KeywordOptions ko;
        ko.keywords = {"Home", "New", "Rotate", "Oblique"};
        ko.defaultKeyword = "Home";
        auto k = co_await ctx.getKeyword(
            "Enter type of dimension editing [Home/New/Rotate/Oblique] <Home>", ko);
        if (!k.ok())
            co_return;
        const bool home = k.keyword == "Home";
        const bool rotate = k.keyword == "Rotate";
        const bool oblique = k.keyword == "Oblique";
        std::string text;
        double angle = 0;
        if (k.keyword == "New") {
            cmd::StringOptions so;
            so.defaultValue = "<>";
            auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
            if (!s.ok())
                co_return;
            text = s.text == "<>" ? std::string() : s.text;
        } else if (rotate) {
            auto a = co_await ctx.getAngle("Specify angle for dimension text, 0 = default",
                                           cmd::AngleOptions{.defaultValue = 0.0});
            if (!a.ok())
                co_return;
            angle = a.value;
        } else if (oblique) {
            auto a =
                co_await ctx.getAngle("Enter obliquing angle of the extension lines, Enter = none",
                                      cmd::AngleOptions{.defaultValue = std::nan("")});
            if (a.status == cmd::Status::Cancel)
                co_return;
            angle = a.ok() ? a.value : std::nan("");
        }
        cmd::SelectionOptions so;
        so.allowPreselection = false;
        so.filter = isAnyDim;
        auto sel = co_await ctx.getSelection("Select dimensions", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMEDIT");
        std::size_t skipped = 0;
        for (EntityId id : sel.ids) {
            bool done = true;
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                auto* d = entityAs<DimensionData>(ent);
                if (!d)
                    return;
                if (home) {
                    // Back to the automatic place and angle; replaced text stays (as in AutoCAD).
                    d->textPosUser = false;
                    d->textRotated = false;
                    d->textRotation = 0;
                } else if (rotate) {
                    d->textRotated = std::abs(angle) > 1e-9;
                    d->textRotation = d->textRotated ? angle : 0;
                } else if (oblique) {
                    if (d->kind != DimKind::Linear && d->kind != DimKind::Aligned) {
                        done = false;
                        return;
                    }
                    // The angle is measured from the X axis; the lean is from the perpendicular
                    // of the dimension line.
                    double skew = 0;
                    if (!std::isnan(angle)) {
                        const Vec2 u = d->kind == DimKind::Linear
                                           ? Vec2{std::cos(d->rotation), std::sin(d->rotation)}
                                           : d->defPts[1] - d->defPts[0];
                        const double perp = std::atan2(u.y, u.x) + kPi / 2;
                        skew = std::remainder(angle - perp, kPi);
                    }
                    if (std::abs(skew) > 85.0 * kPi / 180) {
                        done = false;
                        return;
                    }
                    d->oblique = skew;
                } else {
                    d->textOverride = text;
                }
                dim::refresh(ctx.doc(), *d);
            });
            if (!r)
                ctx.message(r.error().message);
            else if (!done)
                ++skipped;
        }
        if (skipped)
            ctx.message(std::to_string(skipped) +
                        " dimension(s) skipped: only linear and aligned dimensions lean, and the "
                        "angle must stay within 85 degrees of the dimension's own perpendicular.");
    }
};

// What a dimension is broken at: an object's own curves, or for objects without curve geometry
// what they draw (text as the box round it), as in AutoCAD. A drawn line only breaks where it
// crosses, not where it ends on the dimension (a chained dimension's extension line).
struct BreakShape {
    EntityId source{};
    Entity shape{};
    BBox2 box{};           // empty: no quick test (rays and xlines)
    std::size_t ends = 0;  // segments of an open drawn line, whose ends do not break; 0 = curve
    bool area = false;     // a text box: the gap takes the whole part of the line inside it
};

void breakShapes(const Document& doc, const Entity& e, std::vector<BreakShape>& out) {
    switch (kind(e)) {
        case EntityKind::Point:
        case EntityKind::Hatch:
            return;
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            out.push_back({e.common.id, e, geom::bounds(e), 0});
            return;
        case EntityKind::Ray:
            out.push_back({e.common.id, e, BBox2{}, 0});
            return;
        default:
            break;
    }
    geom::DisplayContext dc;
    dc.chordTolMm = 0.01;
    dc.quickText = kind(e) == EntityKind::Text || kind(e) == EntityKind::MText;
    DisplayList list;
    geom::buildDisplay(doc, e, dc, list);
    for (const DisplayItem& item : list.items) {
        const auto* pl = std::get_if<DisplayPolyline>(&item);
        if (!pl || pl->pts.size() < 2)
            continue;
        PolylineData data;
        for (Vec2 p : pl->pts)
            data.v.push_back({p});
        data.closed = pl->closed;
        BreakShape b;
        b.source = e.common.id;
        b.shape.data = std::move(data);
        for (Vec2 p : pl->pts)
            b.box.expand(p);
        b.ends = pl->closed ? 0 : pl->pts.size() - 1;
        b.area = dc.quickText && pl->closed;
        out.push_back(std::move(b));
    }
}

// The shapes that may break dimensions, with a grid to find those near a line quickly (a whole
// drawing can be the break objects). Index it once every shape is in; it must not move after.
struct BreakSet {
    std::vector<BreakShape> shapes;
    std::vector<BBox2> boxes;            // per shape; empty for rays and xlines
    std::vector<std::size_t> unbounded;  // rays and xlines: always tried
    std::optional<geom::BoxGrid> grid;
    void index() {
        boxes.clear();
        unbounded.clear();
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            boxes.push_back(shapes[i].box);
            if (shapes[i].box.empty())
                unbounded.push_back(i);
        }
        if (shapes.size() >= 64)  // a few are quicker to try one by one
            grid.emplace(boxes);
    }
};

// Where the shapes cross `curve` (a line or arc of dimension `self`), as spans of its parameter:
// one point where a line crosses, from where the curve goes into a text box to where it comes out.
struct BreakHit {
    double t0 = 0, t1 = 0;
    EntityId source{};
};
std::vector<BreakHit> breakHits(const Entity& curve, EntityId self, const BreakSet& set) {
    std::vector<BreakHit> spans;
    const BBox2 box = geom::bounds(curve);
    std::vector<std::size_t> near;
    if (set.grid) {
        near = set.unbounded;
        set.grid->query(box, [&](std::size_t i) { near.push_back(i); });
    } else {
        for (std::size_t i = 0; i < set.shapes.size(); ++i)
            if (set.boxes[i].empty() || set.boxes[i].intersects(box))
                near.push_back(i);
    }
    std::sort(near.begin(), near.end());
    std::vector<geom::EntityIntersection> hits;
    for (std::size_t i : near) {
        const BreakShape& s = set.shapes[i];
        if (s.source == self)
            continue;
        hits.clear();
        if (!geom::intersectEntities(curve, s.shape, hits))
            continue;
        if (s.area && hits.size() >= 2) {
            auto [lo, hi] = std::minmax_element(
                hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.tA < b.tA; });
            spans.push_back({lo->tA, hi->tA, s.source});
            continue;
        }
        for (const geom::EntityIntersection& h : hits) {
            if (s.ends && (h.tB < 1e-6 || h.tB > static_cast<double>(s.ends) - 1e-6))
                continue;
            spans.push_back({h.tA, h.tA, s.source});
        }
    }
    return spans;
}

// What DIMBREAK breaks: dimensions, leaders and multileaders.
bool isBreakable(const Entity& e) {
    const EntityKind k = kind(e);
    return k == EntityKind::Dimension || k == EntityKind::Leader || k == EntityKind::MLeader;
}

// The DIMBREAK fields of a dimension, leader or multileader.
struct BreakRefs {
    std::vector<DimBreak>* breaks = nullptr;
    std::vector<EntityId>* with = nullptr;
    std::uint32_t* autoN = nullptr;
};
BreakRefs breakRefs(Entity& e) {
    if (auto* d = entityAs<DimensionData>(e))
        return {&d->breaks, &d->breakWith, &d->autoBreaks};
    if (auto* d = entityAs<LeaderData>(e))
        return {&d->breaks, &d->breakWith, &d->autoBreaks};
    if (auto* d = entityAs<MLeaderData>(e))
        return {&d->breaks, &d->breakWith, &d->autoBreaks};
    return {};
}

// The layout without any gaps (nor jogs, for a dimension).
dim::Layout bareLayout(const Document& doc, const Entity& e) {
    if (const auto* d = entityAs<DimensionData>(e)) {
        DimensionData bare = *d;
        bare.breaks.clear();
        bare.jogs.clear();
        return dim::layoutDimension(doc, bare);
    }
    if (const auto* d = entityAs<LeaderData>(e)) {
        LeaderData bare = *d;
        bare.breaks.clear();
        return dim::layoutLeader(doc, bare);
    }
    if (const auto* d = entityAs<MLeaderData>(e)) {
        MLeaderData bare = *d;
        bare.breaks.clear();
        return dim::layoutMLeader(doc, bare);
    }
    return {};
}

double arrowOf(const Document& doc, DimStyleId style) {
    const DimStyle& st = dim::styleOf(doc, style);
    return st.arrowSize * (st.overallScale > 0 ? st.overallScale : 1);
}

// The lines DIMBREAK may cut: the dimension and extension lines of a dimension (not its arrows),
// the leader lines with their hook or dogleg of a leader; and the arc of an angular or arc length
// dimension. The gap is 1.5 arrow sizes wide.
struct BreakPaths {
    std::vector<std::vector<Vec2>> lines;
    std::vector<dim::Layout::Arc> arcs;
    double gap = 0;
};
BreakPaths breakPaths(const Document& doc, const Entity& e) {
    BreakPaths p;
    if (const auto* d = entityAs<DimensionData>(e)) {
        const dim::Layout L = bareLayout(doc, e);
        for (const dim::Layout::Line& line : L.lines)
            if (line.pts.size() == 2 && line.role != dim::Role::Text)
                p.lines.push_back(line.pts);
        for (const dim::Layout::Arc& arc : L.arcs)
            if (arc.role != dim::Role::Text && arc.radius > 1e-9)
                p.arcs.push_back(arc);
        p.gap = 1.5 * arrowOf(doc, d->style);
    } else if (const auto* ld = entityAs<LeaderData>(e)) {
        const double a = arrowOf(doc, ld->style);
        if (ld->vertices.size() >= 2) {
            p.lines.push_back(ld->vertices);
            const Vec2 last = ld->vertices.back();
            if (!ld->utf8.empty())
                p.lines.push_back({last, last + Vec2{ld->textPos.x >= last.x ? a : -a, 0}});
        }
        p.gap = 1.5 * a;
    } else if (const auto* ml = entityAs<MLeaderData>(e)) {
        for (const auto& pts : ml->lines)
            if (pts.size() >= 2)
                p.lines.push_back(pts);
        if (ml->dogleg)
            p.lines.push_back({ml->landing, ml->contentPos});
        p.gap = 1.5 * arrowOf(doc, ml->style);
    }
    return p;
}

// The automatic DIMBREAK gaps of `e` where the shapes cross its lines and arcs; `hitBy` gets the
// objects that made one.
std::vector<DimBreak> autoGaps(const Document& doc, const Entity& e, const BreakSet& shapes,
                               std::vector<EntityId>& hitBy) {
    std::vector<DimBreak> found;
    if (shapes.shapes.empty())
        return found;
    const EntityId self = e.common.id;
    const BreakPaths P = breakPaths(doc, e);
    const double gap = P.gap;
    auto add = [&](const DimBreak& b, EntityId source) {
        if (std::find(found.begin(), found.end(), b) == found.end())
            found.push_back(b);
        if (std::find(hitBy.begin(), hitBy.end(), source) == hitBy.end())
            hitBy.push_back(source);
    };
    for (const std::vector<Vec2>& pts : P.lines) {
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec2 a = pts[i], dir = pts[i + 1] - a;
            const double len = std::hypot(dir.x, dir.y);
            if (!(len > 1e-9))
                continue;
            Entity seg;
            seg.data = LineData{a, pts[i + 1]};
            for (const BreakHit& h : breakHits(seg, self, shapes)) {
                if (h.t1 < 1e-6 || h.t0 > 1 - 1e-6)
                    continue;
                const double half = gap / 2 / len;
                add({a + dir * std::max(0.0, h.t0 - half), a + dir * std::min(1.0, h.t1 + half)},
                    h.source);
            }
        }
    }
    // The arcs of angular and arc length dimensions break the same way.
    for (const dim::Layout::Arc& arc : P.arcs) {
        double sweep = normalizeAngle(arc.endAngle - arc.startAngle);
        if (sweep < 1e-12)
            sweep = kTwoPi;
        Entity curve;
        curve.data = ArcData{arc.center, arc.radius, normalizeAngle(arc.startAngle),
                             normalizeAngle(arc.endAngle)};
        if (sweep >= kTwoPi)
            curve.data = CircleData{arc.center, arc.radius};
        for (const BreakHit& h : breakHits(curve, self, shapes)) {
            double u0 = normalizeAngle(h.t0 - arc.startAngle);
            double u1 = normalizeAngle(h.t1 - arc.startAngle);
            if (u0 > u1)
                std::swap(u0, u1);
            if (u1 < 1e-6 || u0 > sweep - 1e-6)
                continue;
            const double half = gap / 2 / arc.radius;
            add({arc.center + unitAt(arc.startAngle + std::max(0.0, u0 - half)) * arc.radius,
                 arc.center + unitAt(arc.startAngle + std::min(sweep, u1 + half)) * arc.radius},
                h.source);
        }
    }
    return found;
}

// The gaps placed by hand (DIMBREAK Manual): those after the automatic ones.
std::vector<DimBreak> manualGaps(const std::vector<DimBreak>& breaks, std::uint32_t autoN) {
    const std::size_t from = std::min<std::size_t>(autoN, breaks.size());
    return {breaks.begin() + static_cast<std::ptrdiff_t>(from), breaks.end()};
}

// Sets the automatic gaps, keeping those placed by hand after them.
void setAutoGaps(const BreakRefs& r, std::vector<DimBreak> gaps, std::vector<EntityId> with) {
    const std::vector<DimBreak> manual = manualGaps(*r.breaks, *r.autoN);
    *r.autoN = static_cast<std::uint32_t>(gaps.size());
    gaps.insert(gaps.end(), manual.begin(), manual.end());
    *r.breaks = std::move(gaps);
    *r.with = std::move(with);
}

}  // namespace

// Works the automatic DIMBREAK gaps of a dimension, leader or multileader out again from the
// objects that break it (after it or one of them changed); an object that is gone no longer
// breaks it.
void refreshAutoBreaks(const Document& doc, Entity& e) {
    const BreakRefs r = breakRefs(e);
    if (!r.with || r.with->empty())
        return;
    BreakSet shapes;
    std::vector<EntityId> alive;
    for (EntityId id : *r.with) {
        if (const Entity* c = doc.find(id)) {
            alive.push_back(id);
            breakShapes(doc, *c, shapes.shapes);
        }
    }
    shapes.index();
    std::vector<EntityId> hitBy;
    std::vector<DimBreak> gaps = autoGaps(doc, e, shapes, hitBy);
    setAutoGaps(r, std::move(gaps), std::move(alive));
}

namespace {

// DIMBREAK [DBR]: opens gaps in the dimension and extension lines where other objects cross them,
// or closes the gaps again; leaders and multileaders break the same way. The gaps are stored with
// the object (DimensionData::breaks, LeaderData::breaks, MLeaderData::breaks).
class BreakCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMBREAK"; }
    std::vector<std::string> defaultAliases() const override { return {"DBR"}; }
    std::string_view description() const override {
        return "Break dimension and extension lines where other objects cross them";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::SelectionOptions so;
        so.filter = isBreakable;
        auto dims = co_await ctx.getSelection("Select dimensions to add or remove breaks", so);
        if (!dims.ok() || dims.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Auto", "Manual", "Remove"};
        ko.defaultKeyword = "Auto";
        auto mode = co_await ctx.getKeyword("Break [Auto/Manual/Remove] <Auto>", ko);
        if (!mode.ok())
            co_return;
        if (mode.keyword == "Manual") {
            // As AutoCAD: two points on a dimension or extension line of one dimension; the
            // gap between them is added to the breaks the dimension already has.
            if (dims.ids.size() != 1) {
                ctx.message("Manual breaks one dimension at a time.");
                co_return;
            }
            const EntityId id = dims.ids.front();
            auto p1 = co_await ctx.getPoint("Specify first break point");
            if (!p1.ok())
                co_return;
            cmd::PointOptions po;
            po.base = p1.point;
            auto p2 = co_await ctx.getPoint("Specify second break point", po);
            if (!p2.ok())
                co_return;
            const Entity* e = ctx.doc().find(id);
            if (!e || !isBreakable(*e))
                co_return;
            const dim::Layout L = bareLayout(ctx.doc(), *e);
            // the straight line of the dimension nearest the first point
            double best = 1e300;
            Vec2 la{}, lb{};
            for (const dim::Layout::Line& line : L.lines) {
                if (line.role == dim::Role::Text)
                    continue;
                for (std::size_t i = 0; i + 1 < line.pts.size(); ++i) {
                    const Vec2 a = line.pts[i], b = line.pts[i + 1];
                    const Vec2 ab = b - a;
                    const double len2 = dot(ab, ab);
                    if (!(len2 > 1e-18))
                        continue;
                    const double t = std::clamp(dot(p1.point - a, ab) / len2, 0.0, 1.0);
                    const double dd = distance(p1.point, a + ab * t);
                    if (dd < best) {
                        best = dd;
                        la = a;
                        lb = b;
                    }
                }
            }
            if (best == 1e300) {
                ctx.message("The dimension has no line to break.");
                co_return;
            }
            const Vec2 ab = lb - la;
            const double len2 = dot(ab, ab);
            auto onLine = [&](Vec2 p) {
                return la + ab * std::clamp(dot(p - la, ab) / len2, 0.0, 1.0);
            };
            DimBreak b{onLine(p1.point), onLine(p2.point)};
            if (distance(b.a, b.b) < tol::kLinear) {
                ctx.message("The break points are at the same place on the line.");
                co_return;
            }
            ctx.setUndoLabel("DIMBREAK");
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                if (const BreakRefs br = breakRefs(ent); br.breaks)
                    br.breaks->push_back(b);
            });
            if (!r)
                ctx.message(r.error().message);
            co_return;
        }

        BreakSet shapes;
        if (mode.keyword == "Auto") {
            cmd::SelectionOptions co;
            co.allowPreselection = false;
            co.keywords = {"All"};
            auto picked = co_await ctx.getSelection(
                "Select objects to break dimensions with or [All] <All>", co);
            if (picked.status == Status::Cancel)
                co_return;
            std::vector<EntityId> cutters;
            if (picked.ok() && !picked.ids.empty())
                cutters = picked.ids;
            else
                cutters =
                    ctx.doc().modelSpace();  // by value: begin() and end() of two calls differ
            // Only objects near the lines of the dimensions can break them.
            BBox2 near;
            for (EntityId id : dims.ids) {
                const Entity* e = ctx.doc().find(id);
                if (!e || !isBreakable(*e))
                    continue;
                const dim::Layout L = bareLayout(ctx.doc(), *e);
                for (const dim::Layout::Line& line : L.lines)
                    for (Vec2 p : line.pts)
                        near.expand(p);
                for (const dim::Layout::Arc& arc : L.arcs) {
                    near.expand(arc.center - Vec2{arc.radius, arc.radius});
                    near.expand(arc.center + Vec2{arc.radius, arc.radius});
                }
            }
            auto curveKind = [](EntityKind k) {
                return k == EntityKind::Line || k == EntityKind::Circle || k == EntityKind::Arc ||
                       k == EntityKind::Polyline || k == EntityKind::Ellipse ||
                       k == EntityKind::Spline;
            };
            for (EntityId cid : cutters) {
                const Entity* c = ctx.doc().find(cid);
                if (!c)
                    continue;
                if (curveKind(kind(*c)) && !near.empty() && !geom::bounds(*c).intersects(near))
                    continue;
                const std::size_t from = shapes.shapes.size();
                breakShapes(ctx.doc(), *c, shapes.shapes);
                if (near.empty())
                    continue;
                // What another object draws far away is dropped before the search.
                auto& v = shapes.shapes;
                v.erase(std::remove_if(v.begin() + static_cast<std::ptrdiff_t>(from), v.end(),
                                       [&](const BreakShape& sh) {
                                           return !sh.box.empty() && !sh.box.intersects(near);
                                       }),
                        v.end());
            }
            shapes.index();
        }

        ctx.setUndoLabel("DIMBREAK");
        std::size_t changed = 0, gaps = 0;
        for (EntityId id : dims.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isBreakable(*e))
                continue;
            std::vector<DimBreak> found;
            std::vector<EntityId> hitBy;
            if (mode.keyword == "Auto")
                found = autoGaps(ctx.doc(), *e, shapes, hitBy);
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                const BreakRefs br = breakRefs(ent);
                if (!br.breaks)
                    return;
                if (mode.keyword == "Remove") {
                    br.breaks->clear();
                    br.with->clear();
                    *br.autoN = 0;
                    return;
                }
                // The new automatic gaps replace the old ones; gaps placed by hand stay.
                setAutoGaps(br, found, hitBy);
            });
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            ++changed;
            gaps += found.size();
        }
        if (mode.keyword == "Remove")
            ctx.message(std::to_string(changed) + " object(s): breaks removed.");
        else
            ctx.message(std::to_string(gaps) + " break(s) in " + std::to_string(changed) +
                        " object(s).");
    }
};

class JogLineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMJOGLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"DJL"}; }
    std::string_view description() const override {
        return "Add or remove a jog in the dimension line of a linear dimension";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) {
            const auto* d = entityAs<DimensionData>(e);
            return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned);
        };
        eo.keywords = {"Remove"};
        auto pick = co_await ctx.getEntity("Select dimension to add jog or [Remove]", eo);
        if (pick.status == Status::Keyword) {
            // As AutoCAD: pick the jogged dimension whose jog goes.
            cmd::EntityOptions ro;
            ro.filter = [](const Entity& e) {
                const auto* d = entityAs<DimensionData>(e);
                return d && !d->jogs.empty();
            };
            auto jogged = co_await ctx.getEntity("Select jog to remove", ro);
            if (!jogged.ok())
                co_return;
            ctx.setUndoLabel("DIMJOGLINE");
            auto r = ctx.tx().modify(jogged.id, [](Entity& ent) {
                if (auto* d = entityAs<DimensionData>(ent))
                    d->jogs.clear();
            });
            ctx.message(r ? "Jog line removed." : r.error().message);
            co_return;
        }
        if (!pick.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
        if (!d0)
            co_return;
        // The dimension line as drawn without jogs: the longest straight piece takes the jog.
        DimensionData bare = *d0;
        bare.jogs.clear();
        bare.breaks.clear();
        const dim::Layout L = dim::layoutDimension(ctx.doc(), bare);
        Vec2 a{}, b{};
        double best = 0;
        for (const dim::Layout::Line& line : L.lines) {
            if (line.role != dim::Role::DimLine || line.pts.size() != 2)
                continue;
            const double len = distance(line.pts[0], line.pts[1]);
            if (len > best) {
                best = len;
                a = line.pts[0];
                b = line.pts[1];
            }
        }
        if (!(best > 0)) {
            ctx.message("This dimension has no dimension line to put a jog in.");
            co_return;
        }
        cmd::PointOptions po;
        po.allowNone = true;
        po.keywords = {"Remove"};
        auto where = co_await ctx.getPoint("Specify jog location or [Remove] (Enter = middle)", po);
        if (where.status == Status::Cancel)
            co_return;
        const bool remove = where.status == Status::Keyword;
        Vec2 at = (a + b) * 0.5;
        if (where.ok()) {
            const Vec2 u = (b - a) * (1 / best);
            const double s = std::clamp(dot(where.point - a, u), 0.0, best);
            at = a + u * s;
        }
        ctx.setUndoLabel("DIMJOGLINE");
        auto r = ctx.tx().modify(pick.id, [&](Entity& ent) {
            auto* d = entityAs<DimensionData>(ent);
            if (!d)
                return;
            // One jog per dimension, as in AutoCAD: a new one replaces the old.
            d->jogs.clear();
            if (!remove)
                d->jogs.push_back(at);
        });
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message(remove ? "Jog lines removed." : "Jog line added.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeEditCommand() {
    return std::make_unique<EditCommand>();
}

std::unique_ptr<cmd::Command> makeContinueCommand() {
    return std::make_unique<ChainCommand>("DIMCONTINUE", "DCO", false,
                                          "Continue a chain of dimensions from the last one");
}

std::unique_ptr<cmd::Command> makeBaselineCommand() {
    return std::make_unique<ChainCommand>("DIMBASELINE", "DBA", true,
                                          "Stack dimensions from the same first extension line");
}

std::unique_ptr<cmd::Command> makeSpaceCommand() {
    return std::make_unique<SpaceCommand>();
}

std::unique_ptr<cmd::Command> makeQuickDimCommand() {
    return std::make_unique<QuickDimCommand>();
}

std::unique_ptr<cmd::Command> makeJogLineCommand() {
    return std::make_unique<JogLineCommand>();
}

std::unique_ptr<cmd::Command> makeBreakCommand() {
    return std::make_unique<BreakCommand>();
}

std::unique_ptr<cmd::Command> makeTextEditCommand() {
    return std::make_unique<TextEditCommand>();
}

}  // namespace tbcad::dimcmd
