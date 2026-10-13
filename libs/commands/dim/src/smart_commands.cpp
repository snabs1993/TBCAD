// DIM: one command that picks the dimension type from the object under the cursor, and MLEADER:
// several leader lines that end at one landing point and carry text or a block.
#include <algorithm>
#include <cmath>

#include "dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"

namespace tbcad::dimcmd {

namespace {

bool isDimensionable(const Entity& e) {
    return isMeasurable(e) || isCircleOrArc(e);
}

bool axisAligned(Vec2 a, Vec2 b) {
    Vec2 d = b - a;
    const double len = length(d);
    return len > 0 && (std::abs(d.x) < 1e-6 * len || std::abs(d.y) < 1e-6 * len);
}

// ---- DIM ---------------------------------------------------------------------------------------

class SmartDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIM"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
    std::string_view description() const override {
        return "Dimension what you pick: length, radius, diameter or angle";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        for (;;) {
            cmd::EntityOptions eo;
            eo.filter = isDimensionable;
            auto o = co_await ctx.getEntity("Select object to dimension <exit>", eo);
            if (!o.ok())
                co_return;
            const Entity* e = ctx.doc().find(o.id);
            if (!e)
                co_return;

            // What is being dimensioned, and the keywords that change it.
            enum class Mode { Linear, Aligned, Radius, Diameter, Angular };
            Mode mode = Mode::Linear;
            Vec2 center{}, p1{}, p2{};
            double radius = 0;
            LineData first{};
            LineData second{};
            if (isCircleOrArc(*e)) {
                if (!circleOf(*e, center, radius)) {
                    ctx.message("Select an arc or a circle.");
                    continue;
                }
                mode = kind(*e) == EntityKind::Circle ? Mode::Diameter : Mode::Radius;
            } else {
                if (!measurableEnds(*e, o.pickPoint, p1, p2) || distance(p1, p2) < tol::kLinear) {
                    ctx.message("That object cannot be dimensioned.");
                    continue;
                }
                mode = axisAligned(p1, p2) ? Mode::Linear : Mode::Aligned;
                if (const auto* l = entityAs<LineData>(*e))
                    first = *l;
            }
            const bool isLineObject = kind(*e) == EntityKind::Line;

            std::string override;
            bool placed = false;
            while (!placed) {
                cmd::PointOptions lo;
                const bool radial = mode == Mode::Radius || mode == Mode::Diameter;
                if (radial)
                    lo.keywords = {"Text", mode == Mode::Radius ? "Diameter" : "Radius"};
                else if (mode == Mode::Angular)
                    lo.keywords = {"Text"};
                else if (isLineObject)
                    lo.keywords = {"Text", "Aligned", "Linear", "Angular"};
                else
                    lo.keywords = {"Text", "Aligned", "Linear"};
                auto* c = &ctx;
                auto build = [=](Vec2 cursor) {
                    DimensionData d;
                    switch (mode) {
                        case Mode::Linear:
                            d.kind = DimKind::Linear;
                            d.rotation = autoRotation(p1, p2, cursor);
                            d.defPts = {p1, p2, cursor, {}};
                            break;
                        case Mode::Aligned:
                            d.kind = DimKind::Aligned;
                            d.rotation = std::atan2(p2.y - p1.y, p2.x - p1.x);
                            d.defPts = {p1, p2, cursor, {}};
                            break;
                        case Mode::Radius:
                        case Mode::Diameter: {
                            Vec2 v = cursor - center;
                            Vec2 dir = tol::isZeroLength(v) ? Vec2{1, 0} : normalized(v);
                            d.kind = mode == Mode::Diameter ? DimKind::Diameter : DimKind::Radius;
                            if (mode == Mode::Diameter)
                                d.defPts = {center - dir * radius, center + dir * radius, {}, {}};
                            else
                                d.defPts = {center, center + dir * radius, {}, {}};
                            d.textPos = cursor;
                            d.textPosUser = true;
                            break;
                        }
                        case Mode::Angular:
                            d.kind = DimKind::Angular2Line;
                            d.defPts = {first.a, first.b, second.a, second.b};
                            d.textPos = cursor;
                            break;
                    }
                    d.textOverride = override;
                    return d;
                };
                lo.preview = [c, build](Vec2 cursor, DisplayList& out) {
                    previewDim(*c, build(cursor), out);
                };
                const char* prompt = mode == Mode::Angular ? "Specify dimension arc location"
                                     : radial              ? "Specify dimension text location"
                                                           : "Specify dimension line location";
                auto where = co_await ctx.getPoint(prompt, lo);
                if (where.status == cmd::Status::Keyword) {
                    if (where.keyword == "Text") {
                        cmd::StringOptions so;
                        so.defaultValue = "<>";
                        auto s =
                            co_await ctx.getString("Dimension text, <> is the measurement", so);
                        if (s.ok())
                            override = s.text == "<>" ? std::string() : s.text;
                    } else if (where.keyword == "Aligned") {
                        mode = Mode::Aligned;
                    } else if (where.keyword == "Linear") {
                        mode = Mode::Linear;
                    } else if (where.keyword == "Radius") {
                        mode = Mode::Radius;
                    } else if (where.keyword == "Diameter") {
                        mode = Mode::Diameter;
                    } else if (where.keyword == "Angular") {
                        cmd::EntityOptions lo2;
                        lo2.filter = [firstId = o.id](const Entity& other) {
                            return isLine(other) && other.common.id != firstId;
                        };
                        auto o2 = co_await ctx.getEntity("Select second line", lo2);
                        const Entity* e2 = o2.ok() ? ctx.doc().find(o2.id) : nullptr;
                        const auto* l2 = e2 ? entityAs<LineData>(*e2) : nullptr;
                        if (!l2)
                            continue;
                        DimensionData t;
                        t.kind = DimKind::Angular2Line;
                        t.defPts = {first.a, first.b, l2->a, l2->b};
                        t.textPos = (first.a + first.b + l2->a + l2->b) / 4;
                        if (!dim::angularGeometry(t).ok) {
                            ctx.message("The lines are parallel.");
                            continue;
                        }
                        second = *l2;
                        mode = Mode::Angular;
                    }
                    continue;
                }
                if (!where.ok())
                    co_return;
                addDim(ctx, build(where.point));
                placed = true;
            }
        }
    }
};

// ---- MLEADER -----------------------------------------------------------------------------------

class MLeaderCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEADER"; }
    std::vector<std::string> defaultAliases() const override { return {"MLD"}; }
    std::string_view description() const override {
        return "Leader lines to one note or block, with as many arrowheads as you like";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        const MLeaderStyle mls = currentMLeaderStyle(ctx.doc());
        const DimStyleId sid = mls.dim;
        auto tip = co_await ctx.getPoint("Specify arrowhead location");
        if (!tip.ok())
            co_return;
        std::vector<Vec2> tips{tip.point};

        cmd::PointOptions po;
        po.base = tip.point;
        auto* c = &ctx;
        const Vec2 t0 = tip.point;
        po.preview = [c, t0, sid](Vec2 cursor, DisplayList& out) {
            Entity e;
            e.data = Indirect<LeaderData>(leaderOf(c->doc(), {t0, cursor}, "", sid));
            previewEntity(*c, std::move(e), out);
        };
        auto land = co_await ctx.getPoint("Specify landing location", po);
        if (!land.ok())
            co_return;
        const Vec2 landing = land.point;

        for (;;) {
            cmd::PointOptions more;
            more.base = landing;
            std::vector<Vec2> shown = tips;
            more.preview = [c, shown, landing, sid](Vec2 cursor, DisplayList& out) {
                for (Vec2 t : shown) {
                    Entity e;
                    e.data = Indirect<LeaderData>(leaderOf(c->doc(), {t, landing}, "", sid));
                    previewEntity(*c, std::move(e), out);
                }
                Entity e;
                e.data = Indirect<LeaderData>(leaderOf(c->doc(), {cursor, landing}, "", sid));
                previewEntity(*c, std::move(e), out);
            };
            auto next =
                co_await ctx.getPoint("Specify another arrowhead location or <content>", more);
            if (next.status == Status::None)
                break;
            if (!next.ok())
                co_return;
            if (distance(next.point, landing) > tol::kLinear)
                tips.push_back(next.point);
        }

        cmd::KeywordOptions ko;
        ko.keywords = {"Text", "Block", "None"};
        ko.defaultKeyword = mls.content;
        auto content =
            co_await ctx.getKeyword("Content type [Text/Block/None] <" + mls.content + ">", ko);
        if (!content.ok())
            co_return;

        std::string text;
        const BlockDef* block = nullptr;
        if (content.keyword == "Text") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto note = co_await ctx.getString("Enter annotation text <none>", so);
            if (note.status == Status::Cancel)
                co_return;
            if (note.ok())
                text = note.text;
        } else if (content.keyword == "Block") {
            cmd::StringOptions so;
            auto name = co_await ctx.getString("Enter block name", so);
            if (!name.ok())
                co_return;
            block = ctx.doc().blockByName(name.text);
            if (!block) {
                ctx.message("There is no block named \"" + name.text + "\".");
                co_return;
            }
        }

        ctx.setUndoLabel("MLEADER");
        // The first branch carries the note; the others only reach the same landing point.
        bool firstBranch = true;
        Vec2 contentPos = landing;
        for (Vec2 t : tips) {
            LeaderData l =
                leaderOf(ctx.doc(), {t, landing}, firstBranch ? text : std::string(), sid);
            if (firstBranch)
                contentPos = l.textPos;
            firstBranch = false;
            Entity e;
            e.data = Indirect<LeaderData>(std::move(l));
            auto id = ctx.tx().add(std::move(e));
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
        }
        if (block) {
            InsertData ins;
            ins.block = block->id;
            ins.position = contentPos;
            Entity e;
            e.data = std::move(ins);
            auto id = ctx.tx().add(std::move(e));
            if (!id)
                ctx.message(id.error().message);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSmartDimCommand() {
    return std::make_unique<SmartDimCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderCommand() {
    return std::make_unique<MLeaderCommand>();
}

}  // namespace tbcad::dimcmd
