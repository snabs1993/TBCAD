#include <algorithm>
#include <cmath>
#include <optional>

#include "dim_internal.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"

namespace tbcad::dimcmd {

namespace {

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

// ---- DIMARC ------------------------------------------------------------------------------------

class ArcLengthDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMARC"; }
    std::vector<std::string> defaultAliases() const override { return {"DAR"}; }
    std::string_view description() const override { return "Dimension the length of an arc"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return entityAs<ArcData>(e) != nullptr; };
        auto o = co_await ctx.getEntity("Select arc", eo);
        if (!o.ok())
            co_return;
        const Entity* e = ctx.doc().find(o.id);
        const auto* arc = e ? entityAs<ArcData>(*e) : nullptr;
        if (!arc) {
            ctx.message("Select an arc.");
            co_return;
        }
        const ArcData a = *arc;
        std::string override;
        for (;;) {
            cmd::PointOptions lo;
            lo.keywords = {"Text"};
            auto* c = &ctx;
            auto build = [=](Vec2 cursor) {
                DimensionData d;
                d.kind = DimKind::ArcLength;
                d.defPts = {
                    a.center,
                    a.center + Vec2{std::cos(a.startAngle), std::sin(a.startAngle)} * a.radius,
                    a.center + Vec2{std::cos(a.endAngle), std::sin(a.endAngle)} * a.radius, cursor};
                d.textOverride = override;
                dim::refresh(c->doc(), d);
                return d;
            };
            lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                previewDim(*c, build(cursor), out);
            };
            auto where = co_await ctx.getPoint("Specify arc length dimension location", lo);
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

// ---- DIMORDINATE -------------------------------------------------------------------------------

class OrdinateDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMORDINATE"; }
    std::vector<std::string> defaultAliases() const override { return {"DOR"}; }
    std::string_view description() const override {
        return "Dimension the X or Y coordinate of a point from the origin";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        auto feature = co_await ctx.getPoint("Specify feature location");
        if (!feature.ok())
            co_return;
        const Vec2 P = feature.point;
        std::optional<bool> forceX;  // set by the Xdatum / Ydatum keywords
        std::string override;
        for (;;) {
            cmd::PointOptions lo;
            lo.base = P;
            lo.keywords = {"Xdatum", "Ydatum", "Text"};
            auto* c = &ctx;
            auto build = [=](Vec2 cursor) {
                DimensionData d;
                d.kind = DimKind::Ordinate;
                const Vec2 off = cursor - P;
                // Without a keyword the leader runs along the direction the cursor moved most.
                const bool xDatum = forceX ? *forceX : std::abs(off.y) >= std::abs(off.x);
                d.rotation = xDatum ? 0 : kPi / 2;
                d.defPts = {P, cursor, Vec2{0, 0}, Vec2{}};
                d.textOverride = override;
                dim::refresh(c->doc(), d);
                return d;
            };
            lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                previewDim(*c, build(cursor), out);
            };
            auto where =
                co_await ctx.getPoint("Specify leader endpoint or [Xdatum/Ydatum/Text]", lo);
            if (where.status == Status::Keyword) {
                if (where.keyword == "Xdatum")
                    forceX = true;
                else if (where.keyword == "Ydatum")
                    forceX = false;
                else {
                    cmd::StringOptions so;
                    so.defaultValue = "<>";
                    auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
                    if (s.ok())
                        override = s.text == "<>" ? std::string() : s.text;
                }
                continue;
            }
            if (!where.ok())
                co_return;
            addDim(ctx, build(where.point));
            co_return;
        }
    }
};

// ---- DIMJOGGED ---------------------------------------------------------------------------------

class JoggedDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMJOGGED"; }
    std::vector<std::string> defaultAliases() const override { return {"DJO"}; }
    std::string_view description() const override {
        return "Radius dimension with a jog for arcs whose centre is off the sheet";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isCircleOrArc;
        auto o = co_await ctx.getEntity("Select arc or circle", eo);
        if (!o.ok())
            co_return;
        Vec2 center{};
        double radius = 0;
        const Entity* e = ctx.doc().find(o.id);
        if (!e || !circleOf(*e, center, radius)) {
            ctx.message("Select an arc or a circle.");
            co_return;
        }
        auto cc = co_await ctx.getPoint("Specify center location override");
        if (!cc.ok())
            co_return;
        const Vec2 override = cc.point;
        cmd::PointOptions po;
        po.base = override;
        auto where = co_await ctx.getPoint("Specify dimension line location", po);
        if (!where.ok())
            co_return;
        const Vec2 spot = where.point;
        const Vec2 dir = normalized(spot - center);
        const Vec2 onArc = center + (tol::isZeroLength(dir) ? Vec2{1, 0} : dir) * radius;
        cmd::PointOptions jo;
        jo.base = spot;
        auto* c = &ctx;
        auto build = [=](Vec2 jog) {
            DimensionData d;
            d.kind = DimKind::Jogged;
            d.defPts = {center, onArc, override, jog};
            d.textPos = spot;
            d.textPosUser = true;
            dim::refresh(c->doc(), d);
            return d;
        };
        jo.preview = [c, build](Vec2 cursor, DisplayList& out) {
            previewDim(*c, build(cursor), out);
        };
        const Vec2 mid = (override + onArc) / 2;
        auto jog = co_await ctx.getPoint("Specify jog location <midpoint>", jo);
        if (jog.status == Status::Cancel)
            co_return;
        addDim(ctx, build(jog.ok() ? jog.point : mid));
    }
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

class LeaderCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LEADER"; }
    std::vector<std::string> defaultAliases() const override { return {"LE"}; }
    std::string_view description() const override {
        return "Draw a line with an arrowhead and a text note";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        scaleFirstAnnotationToView(ctx);
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
    setDimRefresh([](const Document& doc, Entity& e) {
        if (auto* d = entityAs<DimensionData>(e))
            dim::refresh(doc, *d);
    });
    for (EntityKind k : {EntityKind::Dimension, EntityKind::Leader, EntityKind::MLeader}) {
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
    (void)registry.add(std::make_unique<ArcLengthDimCommand>());
    (void)registry.add(std::make_unique<OrdinateDimCommand>());
    (void)registry.add(std::make_unique<JoggedDimCommand>());
    (void)registry.add(std::make_unique<LeaderCommand>());
    (void)registry.add(makeSmartDimCommand());
    (void)registry.add(makeMLeaderCommand());
    (void)registry.add(makeMLeaderAlignCommand());
    (void)registry.add(makeMLeaderCollectCommand());
    (void)registry.add(makeMLeaderEditCommand());
    (void)registry.add(makeDimReassocCommand());
    (void)registry.add(makeDimDisassocCommand());
    (void)registry.add(makeMLeaderStyleCommand());
    (void)registry.add(makeContinueCommand());
    (void)registry.add(makeBaselineCommand());
    (void)registry.add(makeTextEditCommand());
    (void)registry.add(makeEditCommand());
    (void)registry.add(makeBreakCommand());
    (void)registry.add(makeJogLineCommand());
    (void)registry.add(makeSpaceCommand());
    (void)registry.add(makeQuickDimCommand());
    (void)registry.add(makeDimStyleCommand());
    (void)registry.add(makeDimSetupCommand());
    (void)registry.add(makeDimOverrideCommand());
    (void)registry.add(makeDimStylesPanelCommand());
}

}  // namespace tbcad::dimcmd
