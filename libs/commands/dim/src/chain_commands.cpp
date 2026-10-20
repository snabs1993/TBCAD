// DIMCONTINUE, DIMBASELINE, DIMSPACE and DIMTEDIT: chains of linear dimensions and dimension text
// editing.
#include <algorithm>
#include <cmath>
#include <optional>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"

namespace tbcad::dimcmd {

namespace {

bool isLengthDim(const Entity& e) {
    const auto* d = entityAs<DimensionData>(e);
    return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned);
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
            const DimStyle& st = dim::styleOf(ctx.doc(), prev.style);
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
        eo.filter = isLengthDim;
        auto base = co_await ctx.getEntity("Select base dimension", eo);
        if (!base.ok())
            co_return;
        const Entity* be = ctx.doc().find(base.id);
        const auto* b = be ? entityAs<DimensionData>(*be) : nullptr;
        if (!b)
            co_return;
        const DimensionData baseDim = *b;
        const Vec2 n = normalOf(baseDim);
        const double baseOff = lineOffset(baseDim, n);

        cmd::SelectionOptions so;
        so.allowPreselection = false;
        so.filter = isLengthDim;
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
            if (!d || !isLengthDim(*e))
                continue;
            const Vec2 dn = normalOf(*d);
            if (std::abs(cross(dn, n)) > 1e-6)
                continue;  // not parallel to the base
            const double sign = dot(dn, n) >= 0 ? 1.0 : -1.0;
            items.push_back({id, sign * lineOffset(*d, dn)});
        }
        if (items.empty()) {
            ctx.message("No dimensions parallel to the base dimension were selected.");
            co_return;
        }

        const DimStyle& st = dim::styleOf(ctx.doc(), baseDim.style);
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
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            if (const auto* l = entityAs<LineData>(*e)) {
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
                    if (!a || !b)
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
        if (mode == Mode::Ordinate) {
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
    const DimStyle& st = dim::styleOf(doc, d.style);
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

// DIMBREAK [DBR]: opens gaps in the dimension and extension lines where other objects cross them,
// or closes the gaps again. The gaps are stored with the dimension (DimensionData::breaks).
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
        so.filter = isAnyDim;
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
            const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d0)
                co_return;
            DimensionData bare = *d0;
            bare.breaks.clear();
            const dim::Layout L = dim::layoutDimension(ctx.doc(), bare);
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
                if (auto* d = entityAs<DimensionData>(ent))
                    d->breaks.push_back(b);
            });
            if (!r)
                ctx.message(r.error().message);
            co_return;
        }

        std::vector<EntityId> cutters;
        if (mode.keyword == "Auto") {
            cmd::SelectionOptions co;
            co.allowPreselection = false;
            co.keywords = {"All"};
            auto picked = co_await ctx.getSelection(
                "Select objects to break dimensions with or [All] <All>", co);
            if (picked.status == Status::Cancel)
                co_return;
            if (picked.ok() && !picked.ids.empty())
                cutters = picked.ids;
            else
                cutters =
                    ctx.doc().modelSpace();  // by value: begin() and end() of two calls differ
        }

        ctx.setUndoLabel("DIMBREAK");
        std::size_t changed = 0, gaps = 0;
        for (EntityId id : dims.ids) {
            const Entity* e = ctx.doc().find(id);
            const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d0)
                continue;
            std::vector<DimBreak> found;
            if (mode.keyword == "Auto") {
                DimensionData bare = *d0;
                bare.breaks.clear();
                const dim::Layout L = dim::layoutDimension(ctx.doc(), bare);
                const DimStyle& st = dim::styleOf(ctx.doc(), d0->style);
                const double gap = 1.5 * st.arrowSize * (st.overallScale > 0 ? st.overallScale : 1);
                for (const dim::Layout::Line& line : L.lines) {
                    if (line.pts.size() != 2 || line.role == dim::Role::Text)
                        continue;
                    const Vec2 a = line.pts[0], dir = line.pts[1] - a;
                    const double len = std::hypot(dir.x, dir.y);
                    if (!(len > 1e-9))
                        continue;
                    Entity seg;
                    seg.data = LineData{line.pts[0], line.pts[1]};
                    for (EntityId cid : cutters) {
                        const Entity* c = ctx.doc().find(cid);
                        if (!c || cid == id || kind(*c) == EntityKind::Dimension)
                            continue;
                        std::vector<geom::EntityIntersection> hits;
                        if (!geom::intersectEntities(seg, *c, hits))
                            continue;
                        for (const geom::EntityIntersection& h : hits) {
                            const double t = h.tA;
                            if (t < 1e-6 || t > 1 - 1e-6)
                                continue;
                            const double half = gap / 2 / len;
                            DimBreak b;
                            b.a = a + dir * std::max(0.0, t - half);
                            b.b = a + dir * std::min(1.0, t + half);
                            if (std::find(found.begin(), found.end(), b) == found.end())
                                found.push_back(b);
                        }
                    }
                }
            }
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                auto* d = entityAs<DimensionData>(ent);
                if (!d)
                    return;
                if (mode.keyword == "Remove")
                    d->breaks.clear();
                else
                    d->breaks = found;
            });
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            ++changed;
            gaps += found.size();
        }
        if (mode.keyword == "Remove")
            ctx.message(std::to_string(changed) + " dimension(s): breaks removed.");
        else
            ctx.message(std::to_string(gaps) + " break(s) in " + std::to_string(changed) +
                        " dimension(s).");
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
        auto pick = co_await ctx.getEntity("Select a linear or aligned dimension", eo);
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
        auto where = co_await ctx.getPoint(
            "Specify jog location on the dimension line or [Remove] (Enter = middle)", po);
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
            if (remove)
                d->jogs.clear();
            else
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
