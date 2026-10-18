// DIM: one command that picks the dimension type from the object under the cursor, and MLEADER:
// several leader lines that end at one landing point and carry text or a block.
#include <algorithm>
#include <cmath>
#include <optional>

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
                if (!circleOf(*e, center, radius, &o.pickPoint)) {
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
        scaleFirstAnnotationToView(ctx);  // text readable at this view, as dimensions get
        const MLeaderStyle mls = currentMLeaderStyle(ctx.doc());
        const DimStyleId sid = mls.dim;
        // Text typed instead of a point is the note itself (AutoCAD's content-first habit). The
        // content type is an option of the first prompt only; the style's type is the default.
        std::string contentType = mls.content;
        std::optional<std::string> typedNote;
        auto isOption = [](const cmd::PointResult& r, const char* name) {
            return r.status == Status::Keyword && r.keyword == name;
        };
        cmd::PointResult tip;
        for (;;) {
            cmd::PointOptions first;
            first.freeText = true;
            first.keywords = {"Content"};
            tip = co_await ctx.getPoint(
                "Specify arrowhead location or [Content type] or type the note text", first);
            if (isOption(tip, "Content")) {
                cmd::KeywordOptions ko;
                ko.keywords = {"Text", "Block", "None"};
                ko.defaultKeyword = contentType;
                auto k = co_await ctx.getKeyword(
                    "Content type [Text/Block/None] <" + contentType + ">", ko);
                if (!k.ok())
                    co_return;
                contentType = k.keyword;
                continue;
            }
            if (tip.status == Status::Keyword) {
                typedNote = tip.keyword;
                ctx.message("Note: \"" + tip.keyword + "\". Now pick where the arrow points.");
                tip = co_await ctx.getPoint("Specify arrowhead location");
            }
            break;
        }
        if (!tip.ok())
            co_return;
        std::vector<Vec2> tips{tip.point};

        auto* c = &ctx;
        Vec2 landing{};
        for (;;) {
            cmd::PointOptions po;
            po.base = tips.back();
            po.freeText = true;
            po.keywords = {"Add"};
            const std::vector<Vec2> shown = tips;
            po.preview = [c, shown, sid](Vec2 cursor, DisplayList& out) {
                for (Vec2 t : shown) {
                    Entity e;
                    e.data = Indirect<LeaderData>(leaderOf(c->doc(), {t, cursor}, "", sid));
                    previewEntity(*c, std::move(e), out);
                }
            };
            auto land = co_await ctx.getPoint(
                "Specify landing location or [Add another arrowhead] or type the note text", po);
            if (isOption(land, "Add")) {
                auto more = co_await ctx.getPoint("Specify another arrowhead location");
                if (!more.ok())
                    co_return;
                tips.push_back(more.point);
                continue;
            }
            if (land.status == Status::Keyword) {
                typedNote = land.keyword;
                ctx.message("Note: \"" + land.keyword + "\".");
                continue;
            }
            if (!land.ok())
                co_return;
            landing = land.point;
            break;
        }

        std::string text;
        const BlockDef* block = nullptr;
        if (typedNote) {
            text = *typedNote;
        } else if (contentType == "Text") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto note = co_await ctx.getString("Enter annotation text <none>", so);
            if (note.status == Status::Cancel)
                co_return;
            if (note.ok())
                text = note.text;
        } else if (contentType == "Block") {
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
        MLeaderData m;
        m.style = sid;
        for (Vec2 t : tips)
            m.lines.push_back({t, landing});
        m.landing = landing;
        const DimStyle& ds = dim::styleOf(ctx.doc(), sid);
        const double k = ds.overallScale > 0 ? ds.overallScale : 1.0;
        const double side = landing.x >= tips.front().x ? 1 : -1;
        m.contentPos = landing + Vec2{side * ds.arrowSize * k, 0};
        if (block) {
            m.content = MLeaderData::Content::Block;
            m.block = block->id;
        } else if (!text.empty()) {
            m.utf8 = text;
        } else {
            m.content = MLeaderData::Content::None;
        }
        Entity e;
        e.data = Indirect<MLeaderData>(std::move(m));
        applyCurrentProperties(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
    }
};

// ---- MLEADERALIGN ------------------------------------------------------------------------------

// Lines up the landing ends of several leaders in a column or a row with a reference leader. The
// arrowheads stay where they point; each note moves with its landing end.
class MLeaderAlignCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEADERALIGN"; }
    std::vector<std::string> defaultAliases() const override { return {"MLA"}; }
    std::string_view description() const override {
        return "Line up the landing ends of leaders with a reference leader";
    }

    // The point where a leader's lines end and its note begins.
    static std::optional<Vec2> landingOf(const Entity& e) {
        if (const auto* m = entityAs<MLeaderData>(e))
            return m->landing;
        if (const auto* l = entityAs<LeaderData>(e); l && !l->vertices.empty())
            return l->vertices.back();
        return std::nullopt;
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return landingOf(e).has_value(); };
        auto sel = co_await ctx.getSelection("Select leaders to align", so);
        if (!sel.ok())
            co_return;
        cmd::EntityOptions eo;
        eo.filter = so.filter;
        auto ref = co_await ctx.getEntity("Select the leader to align to", eo);
        if (!ref.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Vertical", "Horizontal"};
        ko.defaultKeyword = "Vertical";
        auto dir = co_await ctx.getKeyword(
            "Align in a [Vertical/Horizontal] line <Vertical> (same X / same Y)", ko);
        if (!dir.ok())
            co_return;
        const Entity* r = ctx.doc().find(ref.id);
        const auto anchor = r ? landingOf(*r) : std::nullopt;
        if (!anchor)
            co_return;
        const bool vertical = dir.keyword == "Vertical";
        ctx.setUndoLabel("MLEADERALIGN");
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            if (id == ref.id)
                continue;
            const Entity* e = ctx.doc().find(id);
            const auto end = e ? landingOf(*e) : std::nullopt;
            if (!end)
                continue;
            const Vec2 d = vertical ? Vec2{anchor->x - end->x, 0} : Vec2{0, anchor->y - end->y};
            if (!ctx.tx().modify(id, [d](Entity& x) {
                    if (auto* m = entityAs<MLeaderData>(x)) {
                        // The lines that ended at the landing follow it; arrowheads stay put.
                        for (auto& line : m->lines)
                            if (!line.empty() && distance(line.back(), m->landing) < 1e-9)
                                line.back() += d;
                        m->landing += d;
                        m->contentPos += d;
                    } else if (auto* ld = entityAs<LeaderData>(x)) {
                        ld->vertices.back() += d;
                        ld->textPos += d;
                    }
                }))
                continue;
            ++n;
        }
        ctx.message(std::to_string(n) + " leaders aligned.");
    }
};

// ---- MLEADERCOLLECT ----------------------------------------------------------------------------

// Merges several note multileaders into the first one: every arrow keeps its place and its line now
// ends at the first leader's landing, and the notes are joined, one per line. Leaders whose content
// is a block are left alone.
class MLeaderCollectCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEADERCOLLECT"; }
    std::vector<std::string> defaultAliases() const override { return {"MLC"}; }
    std::string_view description() const override {
        return "Merge several multileader notes into one multileader";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            const auto* m = entityAs<MLeaderData>(e);
            return m && m->content == MLeaderData::Content::Text;
        };
        auto sel =
            co_await ctx.getSelection("Select multileaders to collect (the first stays)", so);
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && so.filter(*e))
                ids.push_back(id);
        if (ids.size() < 2) {
            ctx.message("Select at least two multileaders with a text note.");
            co_return;
        }
        ctx.setUndoLabel("MLEADERCOLLECT");
        const EntityId keep = ids.front();
        const auto* first = entityAs<MLeaderData>(*ctx.doc().find(keep));
        const Vec2 landing = first->landing;
        std::vector<std::vector<Vec2>> lines = first->lines;
        std::string text = first->utf8;
        for (std::size_t i = 1; i < ids.size(); ++i) {
            const auto* m = entityAs<MLeaderData>(*ctx.doc().find(ids[i]));
            const Vec2 d = landing - m->landing;
            for (auto line : m->lines) {
                // The landing end joins the first leader's landing; the arrow end stays.
                if (!line.empty() && distance(line.back(), m->landing) < 1e-9)
                    line.back() += d;
                else
                    line.push_back(landing);
                lines.push_back(std::move(line));
            }
            if (!m->utf8.empty())
                text += (text.empty() ? "" : "\\P") + m->utf8;
        }
        if (!ctx.tx().modify(keep, [&](Entity& e) {
                auto* m = entityAs<MLeaderData>(e);
                m->lines = std::move(lines);
                m->utf8 = std::move(text);
            }))
            co_return;
        for (std::size_t i = 1; i < ids.size(); ++i)
            (void)ctx.tx().erase(ids[i]);
        ctx.message(std::to_string(ids.size()) + " multileaders collected into one.");
    }
};

// ---- MLEADEREDIT -------------------------------------------------------------------------------

// Adds an arrowhead (a leader line to the landing) to a multileader, or removes the one nearest
// to the pick point.
class MLeaderEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEADEREDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"MLE"}; }
    std::string_view description() const override {
        return "Add or remove leader lines (arrowheads) of a multileader";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return entityAs<MLeaderData>(e) != nullptr; };
        auto pick = co_await ctx.getEntity("Select a multileader", eo);
        if (!pick.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Add", "Remove"};
        ko.defaultKeyword = "Add";
        auto k = co_await ctx.getKeyword("Leader lines [Add/Remove] <Add>", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "Add") {
            for (;;) {
                auto tip = co_await ctx.getPoint("Specify arrowhead location or Enter to finish",
                                                 cmd::PointOptions{});
                if (tip.status == cmd::Status::None)
                    co_return;
                if (!tip.ok())
                    co_return;
                ctx.setUndoLabel("MLEADEREDIT");
                const Vec2 at = tip.point;
                (void)ctx.tx().modify(pick.id, [at](Entity& e) {
                    auto* m = entityAs<MLeaderData>(e);
                    m->lines.push_back({at, m->landing});
                });
            }
        } else {
            for (;;) {
                const Entity* e = ctx.doc().find(pick.id);
                const auto* m = e ? entityAs<MLeaderData>(*e) : nullptr;
                if (!m || m->lines.size() < 2) {
                    ctx.message("A multileader keeps at least one leader line.");
                    co_return;
                }
                auto at = co_await ctx.getPoint(
                    "Specify the arrowhead to remove (nearest one) or Enter to finish",
                    cmd::PointOptions{});
                if (!at.ok())
                    co_return;
                std::size_t best = 0;
                double bestDist = 1e300;
                for (std::size_t i = 0; i < m->lines.size(); ++i) {
                    if (m->lines[i].empty())
                        continue;
                    const double d = distance(m->lines[i].front(), at.point);
                    if (d < bestDist) {
                        bestDist = d;
                        best = i;
                    }
                }
                ctx.setUndoLabel("MLEADEREDIT");
                (void)ctx.tx().modify(pick.id, [best](Entity& en) {
                    auto* md = entityAs<MLeaderData>(en);
                    md->lines.erase(md->lines.begin() + static_cast<std::ptrdiff_t>(best));
                });
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSmartDimCommand() {
    return std::make_unique<SmartDimCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderEditCommand() {
    return std::make_unique<MLeaderEditCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderAlignCommand() {
    return std::make_unique<MLeaderAlignCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderCollectCommand() {
    return std::make_unique<MLeaderCollectCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderCommand() {
    return std::make_unique<MLeaderCommand>();
}

}  // namespace tbcad::dimcmd
