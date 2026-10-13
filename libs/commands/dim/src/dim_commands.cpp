#include <algorithm>
#include <cmath>

#include "dim_internal.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"

namespace tbcad::dimcmd {

namespace {

// ---- helpers -----------------------------------------------------------------------------------

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out) {
    applyCurrentProperties(ctx.doc().header(), e);
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
        // a half-defined preview simply shows nothing
    }
}

Entity dimEntity(const Document& doc, DimensionData d) {
    d.style = doc.header().currentDimStyle;
    dim::refresh(doc, d);
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    applyCurrentProperties(doc.header(), e);
    return e;
}

void previewDim(cmd::CommandContext& ctx, const DimensionData& d, DisplayList& out) {
    previewEntity(ctx, dimEntity(ctx.doc(), d), out);
}

bool addDim(cmd::CommandContext& ctx, DimensionData d) {
    ctx.setUndoLabel(std::string(ctx.commandName()));
    auto id = ctx.tx().add(dimEntity(ctx.doc(), std::move(d)));
    if (!id) {
        ctx.message(id.error().message);
        return false;
    }
    return true;
}

void previewLine(cmd::CommandContext& ctx, Vec2 a, Vec2 b, DisplayList& out) {
    Entity e;
    e.data = LineData{a, b};
    previewEntity(ctx, std::move(e), out);
}

bool isLine(const Entity& e) {
    return kind(e) == EntityKind::Line;
}

bool isCircleOrArc(const Entity& e) {
    return kind(e) == EntityKind::Circle || kind(e) == EntityKind::Arc;
}

// Dimensionable straight piece of an object: a line, the ends of an arc, the segment of a
// polyline nearest to the pick.
bool isMeasurable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Polyline:
            return true;
        default:
            return false;
    }
}

bool measurableEnds(const Entity& e, Vec2 pick, Vec2& a, Vec2& b) {
    if (const auto* l = entityAs<LineData>(e)) {
        a = l->a;
        b = l->b;
        return true;
    }
    if (const auto* arc = entityAs<ArcData>(e)) {
        a = arc->center + Vec2{std::cos(arc->startAngle), std::sin(arc->startAngle)} * arc->radius;
        b = arc->center + Vec2{std::cos(arc->endAngle), std::sin(arc->endAngle)} * arc->radius;
        return true;
    }
    if (const auto* pl = entityAs<PolylineData>(e)) {
        const std::size_t n = pl->v.size();
        if (n < 2)
            return false;
        const std::size_t segs = pl->closed ? n : n - 1;
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < segs; ++i) {
            Vec2 p = pl->v[i].p, q = pl->v[(i + 1) % n].p;
            Vec2 d = q - p;
            double l2 = lengthSquared(d);
            double t = l2 > 0 ? std::clamp(dot(pick - p, d) / l2, 0.0, 1.0) : 0;
            double dist = distance(pick, p + d * t);
            if (dist < best) {
                best = dist;
                a = p;
                b = q;
            }
        }
        return true;
    }
    return false;
}

bool circleOf(const Entity& e, Vec2& c, double& r) {
    if (const auto* ci = entityAs<CircleData>(e)) {
        c = ci->center;
        r = ci->radius;
        return r > 0;
    }
    if (const auto* ar = entityAs<ArcData>(e)) {
        c = ar->center;
        r = ar->radius;
        return r > 0;
    }
    return false;
}

// Horizontal or vertical dimension line, whichever side of the points the cursor is on.
double autoRotation(Vec2 p1, Vec2 p2, Vec2 c) {
    BBox2 b;
    b.expand(p1);
    b.expand(p2);
    double dx = std::max({c.x - b.max.x, b.min.x - c.x, 0.0});
    double dy = std::max({c.y - b.max.y, b.min.y - c.y, 0.0});
    return dx > dy ? kPi / 2 : 0.0;
}

// ---- DIMLINEAR / DIMALIGNED --------------------------------------------------------------------

class LengthDimCommand final : public cmd::Command {
public:
    LengthDimCommand(std::string name, std::string alias, bool aligned, std::string description)
        : name_(std::move(name)),
          alias_(std::move(alias)),
          aligned_(aligned),
          description_(std::move(description)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        Vec2 p1{}, p2{};
        auto first =
            co_await ctx.getPoint("Specify first extension line origin or <select object>");
        if (first.status == Status::None) {
            cmd::EntityOptions eo;
            eo.filter = isMeasurable;
            auto o = co_await ctx.getEntity("Select object to dimension", eo);
            if (!o.ok())
                co_return;
            const Entity* e = ctx.doc().find(o.id);
            if (!e || !measurableEnds(*e, o.pickPoint, p1, p2)) {
                ctx.message("That object cannot be dimensioned.");
                co_return;
            }
        } else if (first.ok()) {
            p1 = first.point;
            cmd::PointOptions po;
            po.base = p1;
            auto* c = &ctx;
            po.preview = [c, p1](Vec2 cursor, DisplayList& out) {
                previewLine(*c, p1, cursor, out);
            };
            auto second = co_await ctx.getPoint("Specify second extension line origin", po);
            if (!second.ok())
                co_return;
            p2 = second.point;
        } else {
            co_return;
        }
        if (distance(p1, p2) < tol::kLinear) {
            ctx.message("The extension line origins coincide.");
            co_return;
        }

        enum class Mode { Auto, Horizontal, Vertical, Rotated };
        Mode mode = aligned_ ? Mode::Rotated : Mode::Auto;
        double rotation = 0;
        std::string override;
        for (;;) {
            cmd::PointOptions lo;
            lo.keywords =
                aligned_ ? std::vector<std::string>{"Text"}
                         : std::vector<std::string>{"Text", "Horizontal", "Vertical", "Rotated"};
            auto* c = &ctx;
            const bool aligned = aligned_;
            auto build = [=](Vec2 cursor) {
                DimensionData d;
                d.kind = aligned ? DimKind::Aligned : DimKind::Linear;
                d.rotation = rotation;
                switch (mode) {
                    case Mode::Auto:
                        d.rotation = autoRotation(p1, p2, cursor);
                        break;
                    case Mode::Horizontal:
                        d.rotation = 0;
                        break;
                    case Mode::Vertical:
                        d.rotation = kPi / 2;
                        break;
                    case Mode::Rotated:
                        break;
                }
                d.defPts = {p1, p2, cursor, {}};
                d.textOverride = override;
                return d;
            };
            lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                previewDim(*c, build(cursor), out);
            };
            auto where = co_await ctx.getPoint("Specify dimension line location", lo);
            if (where.status == Status::Keyword) {
                if (where.keyword == "Text") {
                    cmd::StringOptions so;
                    so.defaultValue = "<>";
                    auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
                    if (s.ok())
                        override = s.text == "<>" ? std::string() : s.text;
                } else if (where.keyword == "Horizontal") {
                    mode = Mode::Horizontal;
                } else if (where.keyword == "Vertical") {
                    mode = Mode::Vertical;
                } else if (where.keyword == "Rotated") {
                    cmd::AngleOptions ao;
                    ao.defaultValue = 0;
                    auto a = co_await ctx.getAngle("Specify angle of dimension line", ao);
                    if (a.ok()) {
                        rotation = a.value;
                        mode = Mode::Rotated;
                    }
                }
                continue;
            }
            if (!where.ok())
                co_return;
            addDim(ctx, build(where.point));
            co_return;
        }
    }

private:
    std::string name_, alias_;
    bool aligned_;
    std::string description_;
};

// ---- DIMRADIUS / DIMDIAMETER -------------------------------------------------------------------

class RadialDimCommand final : public cmd::Command {
public:
    RadialDimCommand(std::string name, std::string alias, bool diameter, std::string description)
        : name_(std::move(name)),
          alias_(std::move(alias)),
          diameter_(diameter),
          description_(std::move(description)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isCircleOrArc;
        auto o =
            co_await ctx.getEntity(diameter_ ? "Select arc or circle" : "Select arc or circle", eo);
        if (!o.ok())
            co_return;
        Vec2 center{};
        double radius = 0;
        const Entity* e = ctx.doc().find(o.id);
        if (!e || !circleOf(*e, center, radius)) {
            ctx.message("Select an arc or a circle.");
            co_return;
        }
        std::string override;
        for (;;) {
            cmd::PointOptions lo;
            lo.keywords = {"Text"};
            auto* c = &ctx;
            const bool dia = diameter_;
            auto build = [=](Vec2 cursor) {
                Vec2 v = cursor - center;
                Vec2 dir = tol::isZeroLength(v) ? Vec2{1, 0} : normalized(v);
                DimensionData d;
                d.kind = dia ? DimKind::Diameter : DimKind::Radius;
                if (dia)
                    d.defPts = {center - dir * radius, center + dir * radius, {}, {}};
                else
                    d.defPts = {center, center + dir * radius, {}, {}};
                d.textPos = cursor;
                d.textPosUser = true;
                d.textOverride = override;
                return d;
            };
            lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                previewDim(*c, build(cursor), out);
            };
            auto where = co_await ctx.getPoint("Specify dimension line location", lo);
            if (where.status == Status::Keyword) {
                cmd::StringOptions so;
                so.defaultValue = "<>";
                auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
                if (s.ok())
                    override = s.text == "<>" ? std::string() : s.text;
                continue;
            }
            if (!where.ok())
                co_return;
            addDim(ctx, build(where.point));
            co_return;
        }
    }

private:
    std::string name_, alias_;
    bool diameter_;
    std::string description_;
};

// ---- DIMANGULAR --------------------------------------------------------------------------------

class AngularDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMANGULAR"; }
    std::vector<std::string> defaultAliases() const override { return {"DAN"}; }
    std::string_view description() const override {
        return "Dimension the angle between two lines or three points";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isLine;
        auto l1 = co_await ctx.getEntity("Select first line or <specify vertex>", eo);
        DimensionData base;
        if (l1.status == Status::None) {
            auto v = co_await ctx.getPoint("Specify angle vertex");
            if (!v.ok())
                co_return;
            cmd::PointOptions po;
            po.base = v.point;
            auto a = co_await ctx.getPoint("Specify first angle endpoint", po);
            if (!a.ok())
                co_return;
            po.base = v.point;
            auto b = co_await ctx.getPoint("Specify second angle endpoint", po);
            if (!b.ok())
                co_return;
            base.kind = DimKind::Angular3Point;
            base.defPts = {a.point, b.point, v.point, {}};
        } else if (l1.ok()) {
            auto l2 = co_await ctx.getEntity("Select second line", eo);
            if (!l2.ok())
                co_return;
            const auto* a =
                ctx.doc().find(l1.id) ? entityAs<LineData>(*ctx.doc().find(l1.id)) : nullptr;
            const auto* b =
                ctx.doc().find(l2.id) ? entityAs<LineData>(*ctx.doc().find(l2.id)) : nullptr;
            if (!a || !b) {
                ctx.message("Select two lines.");
                co_return;
            }
            base.kind = DimKind::Angular2Line;
            base.defPts = {a->a, a->b, b->a, b->b};
            if (!dim::angularGeometry([&] {
                     DimensionData t = base;
                     t.textPos = (a->a + a->b + b->a + b->b) / 4;
                     return t;
                 }())
                     .ok) {
                ctx.message("The lines are parallel.");
                co_return;
            }
        } else {
            co_return;
        }

        std::string override;
        for (;;) {
            cmd::PointOptions lo;
            lo.keywords = {"Text"};
            auto* c = &ctx;
            auto build = [=](Vec2 cursor) {
                DimensionData d = base;
                if (d.kind == DimKind::Angular3Point)
                    d.defPts[3] = cursor;
                else
                    d.textPos = cursor;
                d.textOverride = override;
                return d;
            };
            lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                previewDim(*c, build(cursor), out);
            };
            auto where = co_await ctx.getPoint("Specify dimension arc line location", lo);
            if (where.status == Status::Keyword) {
                cmd::StringOptions so;
                so.defaultValue = "<>";
                auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
                if (s.ok())
                    override = s.text == "<>" ? std::string() : s.text;
                continue;
            }
            if (!where.ok())
                co_return;
            addDim(ctx, build(where.point));
            co_return;
        }
    }
};

// ---- LEADER ------------------------------------------------------------------------------------

LeaderData leaderOf(const Document& doc, std::vector<Vec2> pts, const std::string& plain) {
    LeaderData l;
    l.style = doc.header().currentDimStyle;
    l.vertices = std::move(pts);
    l.utf8 = text::markupFromPlain(plain);
    if (l.vertices.size() >= 2) {
        const DimStyle& st = dim::styleOf(doc, l.style);
        const double k = st.overallScale > 0 ? st.overallScale : 1.0;
        const Vec2 last = l.vertices.back(), prev = l.vertices[l.vertices.size() - 2];
        const double sign = last.x >= prev.x ? 1 : -1;
        l.textPos = last + Vec2{sign * (st.arrowSize + st.textGap) * k, 0};
    }
    return l;
}

class LeaderCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LEADER"; }
    std::vector<std::string> defaultAliases() const override { return {"LE"}; }
    std::string_view description() const override {
        return "Draw a line with an arrowhead and a text note";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        auto first = co_await ctx.getPoint("Specify leader start point");
        if (!first.ok())
            co_return;
        std::vector<Vec2> pts{first.point};
        for (;;) {
            cmd::PointOptions po;
            po.base = pts.back();
            auto* c = &ctx;
            po.preview = [c, pts](Vec2 cursor, DisplayList& out) {
                auto v = pts;
                v.push_back(cursor);
                Entity e;
                e.data = Indirect<LeaderData>(leaderOf(c->doc(), v, ""));
                previewEntity(*c, std::move(e), out);
            };
            auto next = co_await ctx.getPoint(
                pts.size() == 1 ? "Specify next point" : "Specify next point or <annotation>", po);
            if (next.status == Status::None)
                break;
            if (!next.ok())
                co_return;
            if (distance(next.point, pts.back()) > tol::kLinear)
                pts.push_back(next.point);
        }
        if (pts.size() < 2) {
            ctx.message("A leader needs at least two points.");
            co_return;
        }
        cmd::StringOptions so;
        so.allowEmpty = true;
        auto note = co_await ctx.getString("Enter annotation text <none>", so);
        if (note.status == Status::Cancel)
            co_return;
        std::string text = note.ok() ? note.text : std::string();
        ctx.setUndoLabel("LEADER");
        Entity e;
        e.data = Indirect<LeaderData>(leaderOf(ctx.doc(), std::move(pts), text));
        applyCurrentProperties(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    dim::registerDisplay();
    for (EntityKind k : {EntityKind::Dimension, EntityKind::Leader}) {
        blocks::ExplodeRegistry::set(
            k, [](const Document& doc, const Entity& e) -> Result<blocks::ExplodeResult> {
                auto r = dim::explodeDimension(doc, e);
                if (!r)
                    return r.error();
                return blocks::ExplodeResult{std::move(r).value(), {}};
            });
    }
    (void)registry.add(std::make_unique<LengthDimCommand>(
        "DIMLINEAR", "DLI", false, "Horizontal, vertical or rotated dimension"));
    (void)registry.add(std::make_unique<LengthDimCommand>(
        "DIMALIGNED", "DAL", true, "Dimension parallel to the measured points"));
    (void)registry.add(std::make_unique<RadialDimCommand>("DIMRADIUS", "DRA", false,
                                                          "Radius dimension of an arc or circle"));
    (void)registry.add(std::make_unique<RadialDimCommand>(
        "DIMDIAMETER", "DDI", true, "Diameter dimension of an arc or circle"));
    (void)registry.add(std::make_unique<AngularDimCommand>());
    (void)registry.add(std::make_unique<LeaderCommand>());
    (void)registry.add(makeDimStyleCommand());
}

}  // namespace tbcad::dimcmd
