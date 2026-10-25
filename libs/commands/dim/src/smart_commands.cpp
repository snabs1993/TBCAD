// DIM: one command that picks the dimension type from the object under the cursor, and MLEADER:
// several leader lines that end at one landing point and carry text or a block.
#include <algorithm>
#include <cmath>
#include <optional>

#include "dim_internal.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"

namespace tbcad::dimcmd {

namespace {

bool isDimensionable(const Entity& e) {
    return isMeasurable(e) || isCircleOrArc(e);
}

bool axisAligned(Vec2 a, Vec2 b, double ucs) {
    Vec2 d = b - a;
    d = {d.x * std::cos(-ucs) - d.y * std::sin(-ucs), d.x * std::sin(-ucs) + d.y * std::cos(-ucs)};
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
            eo.keywords = {"Baseline", "Continue"};
            auto o = co_await ctx.getEntity("Select object to dimension <exit>", eo);
            // Baseline / Continue: DIMBASELINE or DIMCONTINUE, then back to this prompt.
            if (o.status == Status::Keyword &&
                (o.keyword == "Baseline" || o.keyword == "Continue")) {
                auto chain =
                    o.keyword == "Baseline" ? makeBaselineCommand() : makeContinueCommand();
                co_await chain->run(ctx);
                continue;
            }
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
                mode = axisAligned(p1, p2, ctx.host().ucsAngle()) ? Mode::Linear : Mode::Aligned;
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
                const double ucs = ctx.host().ucsAngle();
                auto build = [=](Vec2 cursor) {
                    DimensionData d;
                    switch (mode) {
                        case Mode::Linear:
                            d.kind = DimKind::Linear;
                            d.rotation = autoRotation(p1, p2, cursor, ucs);
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
        // As AutoCAD: arrowhead first (the default), leader Landing first or Content first, and
        // Options for the content type, the number of points of a leader line and the dogleg.
        // Text typed instead of a point is the note itself.
        std::string contentType = mls.content;
        long long maxPoints = mls.maxPoints;  // points of a leader line, arrowhead and landing
        bool dogleg = mls.dogleg;
        double firstAngle = 0, secondAngle = 0;  // degrees; 0 = free
        std::optional<std::string> typedNote;
        const BlockDef* block = nullptr;
        auto isOption = [](const cmd::PointResult& r, const char* name) {
            return r.status == Status::Keyword && r.keyword == name;
        };
        enum class Order { Arrow, Landing, Content } order = Order::Arrow;
        cmd::PointResult first;
        for (;;) {
            cmd::PointOptions fo;
            fo.freeText = true;
            fo.keywords = {"Landing", "Content", "Options"};
            first = co_await ctx.getPoint(
                "Specify leader arrowhead location or [leader Landing first/Content first/Options] "
                "or type the note text",
                fo);
            if (isOption(first, "Options")) {
                for (;;) {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"Type", "Maxpoints", "First", "Second", "Dogleg", "eXit"};
                    ko.defaultKeyword = "eXit";
                    auto k = co_await ctx.getKeyword(
                        "Enter an option [Type of content/Maxpoints/First angle/Second "
                        "angle/Dogleg/eXit] "
                        "<eXit>",
                        ko);
                    if (!k.ok())
                        co_return;
                    if (k.keyword == "eXit")
                        break;
                    if (k.keyword == "Type") {
                        cmd::KeywordOptions to;
                        to.keywords = {"Text", "Block", "None"};
                        to.defaultKeyword = contentType;
                        auto t = co_await ctx.getKeyword(
                            "Content type [Text/Block/None] <" + contentType + ">", to);
                        if (!t.ok())
                            co_return;
                        contentType = t.keyword;
                    } else if (k.keyword == "Maxpoints") {
                        cmd::IntOptions io;
                        io.min = 2;
                        io.max = 100;
                        io.defaultValue = maxPoints;
                        auto n = co_await ctx.getInteger(
                            "Maximum points of a leader line <" + std::to_string(maxPoints) + ">",
                            io);
                        if (!n.ok())
                            co_return;
                        maxPoints = n.value;
                    } else if (k.keyword == "First" || k.keyword == "Second") {
                        double& inc = k.keyword == "First" ? firstAngle : secondAngle;
                        auto a = co_await ctx.getReal(
                            std::string(k.keyword == "First" ? "First" : "Second") +
                                " segment angle increment in degrees, 0 for any <" +
                                std::to_string(static_cast<int>(inc)) + ">",
                            cmd::RealOptions{.defaultValue = inc});
                        if (!a.ok())
                            co_return;
                        if (a.value < 0 || a.value >= 180)
                            ctx.message("The angle must be from 0 to below 180.");
                        else
                            inc = a.value;
                    } else {
                        cmd::KeywordOptions yo;
                        yo.keywords = {"Yes", "No"};
                        yo.defaultKeyword = dogleg ? "Yes" : "No";
                        auto y = co_await ctx.getKeyword(
                            std::string("Use a dogleg [Yes/No] <") + (dogleg ? "Yes" : "No") + ">",
                            yo);
                        if (!y.ok())
                            co_return;
                        dogleg = y.keyword == "Yes";
                    }
                }
                continue;
            }
            if (isOption(first, "Landing"))
                order = Order::Landing;
            else if (isOption(first, "Content"))
                order = Order::Content;
            else if (first.status == Status::Keyword) {
                typedNote = first.keyword;
                ctx.message("Note: \"" + first.keyword + "\". Now pick where the arrow points.");
                first = co_await ctx.getPoint("Specify leader arrowhead location");
            }
            break;
        }

        // The note (or block) asked for when the content type needs one: first in Content-first
        // order, last otherwise.
        bool contentAsked = false;
        std::string text;
        bool cancelled = false;
        auto takeContent = [&](cmd::StringResult r, bool isBlock) {
            contentAsked = true;
            if (r.status == Status::Cancel) {
                cancelled = true;
                return;
            }
            if (!r.ok())
                return;
            if (isBlock) {
                block = ctx.doc().blockByName(r.text);
                if (!block) {
                    ctx.message("There is no block named \"" + r.text + "\".");
                    cancelled = true;
                }
            } else {
                text = r.text;
            }
        };

        // The style's block is offered first.
        const std::string blockPrompt =
            mls.block.empty() ? "Enter block name" : "Enter block name <" + mls.block + ">";
        cmd::StringOptions blockOptions;
        if (!mls.block.empty())
            blockOptions.defaultValue = mls.block;

        auto* c = &ctx;
        std::vector<std::vector<Vec2>> lines;  // each from its arrowhead; the landing is added
        Vec2 landing{};
        std::optional<Vec2> contentAt;  // Content first: where the note goes

        if (order == Order::Content) {
            if (contentType == "Block") {
                takeContent(co_await ctx.getString(blockPrompt, blockOptions), true);
            } else if (contentType == "Text") {
                cmd::StringOptions so;
                so.allowEmpty = true;
                takeContent(co_await ctx.getString("Enter the note", so), false);
            }
            if (cancelled)
                co_return;
            auto at = co_await ctx.getPoint("Specify content location");
            if (!at.ok())
                co_return;
            contentAt = at.point;
        }
        if (order == Order::Landing || order == Order::Content) {
            if (order == Order::Landing) {
                auto at = co_await ctx.getPoint("Specify leader landing location");
                if (!at.ok())
                    co_return;
                landing = at.point;
            }
            // Arrowheads to the landing (or to the content point, which the landing follows).
            for (;;) {
                const Vec2 to = contentAt ? *contentAt : landing;
                cmd::PointOptions po;
                po.base = to;
                po.preview = [c, to, sid](Vec2 cursor, DisplayList& out) {
                    Entity e;
                    e.data = Indirect<LeaderData>(leaderOf(c->doc(), {cursor, to}, "", sid));
                    previewEntity(*c, std::move(e), out);
                };
                auto tip = co_await ctx.getPoint(
                    lines.empty() ? "Specify leader arrowhead location"
                                  : "Specify another arrowhead location or Enter to finish",
                    po);
                if (tip.status == Status::None && !lines.empty())
                    break;
                if (!tip.ok())
                    co_return;
                lines.push_back({tip.point});
            }
        } else {
            if (!first.ok())
                co_return;
            lines.push_back({first.point});
            // The next point keeps the angle of its segment on a multiple of the increment.
            auto snapped = [&](Vec2 prev, Vec2 pt) {
                const double inc =
                    (lines.front().size() == 1 ? firstAngle : secondAngle) * kPi / 180;
                const double len = length(pt - prev);
                if (!(inc > 0) || !(len > 0))
                    return pt;
                const double a = std::round(std::atan2((pt - prev).y, (pt - prev).x) / inc) * inc;
                return prev + Vec2{std::cos(a), std::sin(a)} * len;
            };
            for (;;) {
                cmd::PointOptions po;
                po.base = lines.back().back();
                po.freeText = true;
                po.keywords = {"Add"};
                const std::vector<std::vector<Vec2>> shown = lines;
                po.preview = [c, shown, sid, snapped, prev = lines.back().back()](
                                 Vec2 cursor, DisplayList& out) {
                    for (const auto& l : shown) {
                        std::vector<Vec2> pts = l;
                        pts.push_back(snapped(prev, cursor));
                        Entity e;
                        e.data = Indirect<LeaderData>(leaderOf(c->doc(), pts, "", sid));
                        previewEntity(*c, std::move(e), out);
                    }
                };
                // With more than two points a leader line bends at each point before the landing.
                const bool more = static_cast<long long>(lines.front().size()) + 1 < maxPoints &&
                                  lines.size() == 1;
                auto land = co_await ctx.getPoint(
                    more ? "Specify next point or [Add another arrowhead] or type the note text"
                         : "Specify landing location or [Add another arrowhead] or type the note "
                           "text",
                    po);
                if (isOption(land, "Add")) {
                    auto tip = co_await ctx.getPoint("Specify another arrowhead location");
                    if (!tip.ok())
                        co_return;
                    lines.push_back({tip.point});
                    continue;
                }
                if (land.status == Status::Keyword) {
                    typedNote = land.keyword;
                    ctx.message("Note: \"" + land.keyword + "\".");
                    continue;
                }
                if (land.status == Status::None && lines.front().size() > 1) {
                    // Enter: the last bend is the landing.
                    landing = lines.front().back();
                    lines.front().pop_back();
                    break;
                }
                if (!land.ok())
                    co_return;
                const Vec2 landAt = snapped(lines.front().back(), land.point);
                if (more) {
                    lines.front().push_back(landAt);
                    continue;
                }
                landing = landAt;
                break;
            }
        }

        if (typedNote) {
            text = *typedNote;
        } else if (!contentAsked && contentType == "Text") {
            cmd::StringOptions so;
            so.allowEmpty = true;
            takeContent(co_await ctx.getString("Enter annotation text <none>", so), false);
        } else if (!contentAsked && contentType == "Block") {
            takeContent(co_await ctx.getString(blockPrompt, blockOptions), true);
        }
        if (cancelled)
            co_return;
        // A block's attributes are filled in as for INSERT (unless ATTREQ is off).
        std::vector<AttribValue> attribs;
        if (block) {
            attribs = blocks::defaultAttribs(ctx.doc(), block->id);
            if (ctx.host().setting("block.attreq").value_or("1") != "0") {
                for (const Entity* ad : blocks::attributeDefinitions(ctx.doc(), block->id)) {
                    const AttDefInfo& info = *entityAs<TextData>(*ad)->attdef;
                    if (info.constant || info.preset)
                        continue;
                    for (AttribValue& v : attribs) {
                        if (v.tag != info.tag)
                            continue;
                        const std::string label = info.prompt.empty() ? info.tag : info.prompt;
                        auto r = co_await ctx.getString(
                            "Enter attribute value for " + label + " <" + v.value + ">",
                            cmd::StringOptions{.defaultValue = v.value, .allowEmpty = true});
                        if (!r.ok())
                            co_return;
                        v.value = r.text;
                    }
                }
            }
        }

        ctx.setUndoLabel("MLEADER");
        MLeaderData m;
        m.style = sid;
        m.dogleg = dogleg;
        const DimStyle& ds = dim::styleOf(ctx.doc(), sid);
        const double k = ds.overallScale > 0 ? ds.overallScale : 1.0;
        const double doglegLen =
            !dogleg ? 0.0 : (mls.doglegLength > 0 ? mls.doglegLength : ds.arrowSize) * k;
        if (contentAt) {
            // The note sits where it was put; the landing is one dogleg towards the arrows.
            const double side = contentAt->x >= lines.front().front().x ? 1 : -1;
            landing = *contentAt - Vec2{side * doglegLen, 0};
        }
        for (auto& l : lines) {
            l.push_back(landing);
            m.lines.push_back(std::move(l));
        }
        m.landing = landing;
        const Vec2 from = m.lines.front().size() > 1 ? m.lines.front()[m.lines.front().size() - 2]
                                                     : m.lines.front().front();
        const double side = landing.x >= from.x ? 1 : -1;
        m.contentPos = contentAt ? *contentAt : landing + Vec2{side * doglegLen, 0};
        if (block) {
            m.content = MLeaderData::Content::Block;
            m.block = block->id;
            m.attribs = std::move(attribs);
        } else if (!text.empty()) {
            m.utf8 = text;
        } else {
            m.content = MLeaderData::Content::None;
        }
        Entity e;
        e.data = Indirect<MLeaderData>(std::move(m));
        applyCurrentProperties(ctx.doc().header(), e);
        if (mls.annotative && !e.common.annotative) {
            e.common.annotative = true;
            e.common.annoScales =
                ScaleList(std::vector<double>{currentAnnotationScale(ctx.doc().header())});
        }
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

    // The last segment of the (first) leader line, pointing at the landing.
    static std::optional<Vec2> lastSegment(const Entity& e) {
        if (const auto* m = entityAs<MLeaderData>(e)) {
            if (m->lines.empty() || m->lines.front().empty())
                return std::nullopt;
            const auto& l = m->lines.front();
            const Vec2 from =
                l.size() > 1 && distance(l.back(), m->landing) < 1e-9 ? l[l.size() - 2] : l.back();
            const Vec2 v = m->landing - from;
            return tol::isZeroLength(v) ? std::nullopt : std::optional<Vec2>(v);
        }
        if (const auto* l = entityAs<LeaderData>(e); l && l->vertices.size() >= 2) {
            const Vec2 v = l->vertices.back() - l->vertices[l->vertices.size() - 2];
            return tol::isZeroLength(v) ? std::nullopt : std::optional<Vec2>(v);
        }
        return std::nullopt;
    }

    // Moves the landing end by `d`: the lines that ended there follow, arrowheads stay put, and
    // the note goes with it.
    static bool moveLanding(cmd::CommandContext& ctx, EntityId id, Vec2 d) {
        return ctx.tx()
            .modify(id,
                    [d](Entity& x) {
                        if (auto* m = entityAs<MLeaderData>(x)) {
                            for (auto& line : m->lines)
                                if (!line.empty() && distance(line.back(), m->landing) < 1e-9)
                                    line.back() += d;
                            m->landing += d;
                            m->contentPos += d;
                        } else if (auto* ld = entityAs<LeaderData>(x)) {
                            ld->vertices.back() += d;
                            ld->textPos += d;
                        }
                    })
            .ok();
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
        ko.keywords = {"Vertical", "Horizontal", "Spacing", "Distribute", "Parallel"};
        ko.defaultKeyword = "Vertical";
        // Spacing 0 keeps each leader where it is along the line; above 0 the leaders are placed
        // that far apart in their current order, the reference staying where it is.
        double spacing = 0;
        cmd::KeywordResult dir;
        while (true) {
            dir = co_await ctx.getKeyword(
                "Align in a [Vertical/Horizontal/Spacing] line, or "
                "[Distribute/Parallel] <Vertical>",
                ko);
            if (!dir.ok())
                co_return;
            if (dir.keyword != "Spacing")
                break;
            cmd::DistanceOptions dopt;
            dopt.defaultValue = spacing;
            auto sp = co_await ctx.getDistance("Spacing between leaders (0 keeps positions)", dopt);
            if (!sp.ok())
                co_return;
            spacing = std::max(0.0, sp.value);
        }
        const Entity* r = ctx.doc().find(ref.id);
        const auto anchor = r ? landingOf(*r) : std::nullopt;
        if (!anchor)
            co_return;
        const bool vertical = dir.keyword == "Vertical";
        if (dir.keyword == "Distribute") {
            // As AutoCAD: the landings are spread evenly between two points, in their order
            // along that line.
            auto p1 = co_await ctx.getPoint("Specify first point");
            if (!p1.ok())
                co_return;
            cmd::PointOptions po;
            po.base = p1.point;
            auto p2 = co_await ctx.getPoint("Specify second point", po);
            if (!p2.ok())
                co_return;
            const Vec2 a = p1.point, ab = p2.point - p1.point;
            struct Spot {
                EntityId id;
                Vec2 end;
            };
            std::vector<Spot> spots;
            for (EntityId id : sel.ids) {
                const Entity* e = ctx.doc().find(id);
                if (const auto end = e ? landingOf(*e) : std::nullopt)
                    spots.push_back({id, *end});
            }
            if (std::none_of(spots.begin(), spots.end(),
                             [&](const Spot& sp) { return sp.id == ref.id; }))
                spots.push_back({ref.id, *anchor});
            std::stable_sort(spots.begin(), spots.end(), [&](const Spot& x, const Spot& y) {
                return dot(x.end - a, ab) < dot(y.end - a, ab);
            });
            ctx.setUndoLabel("MLEADERALIGN");
            const double n = static_cast<double>(spots.size());
            for (std::size_t i = 0; i < spots.size(); ++i) {
                const double t = n > 1 ? static_cast<double>(i) / (n - 1) : 0.5;
                moveLanding(ctx, spots[i].id, a + ab * t - spots[i].end);
            }
            ctx.message(std::to_string(spots.size()) + " leaders distributed.");
            co_return;
        }
        if (dir.keyword == "Parallel") {
            // The last segment of every leader line turns parallel to the reference's, keeping
            // its length and the point it starts from.
            const auto refDir = lastSegment(*r);
            if (!refDir) {
                ctx.message("The reference leader has no line to follow.");
                co_return;
            }
            ctx.setUndoLabel("MLEADERALIGN");
            std::size_t n = 0;
            for (EntityId id : sel.ids) {
                if (id == ref.id)
                    continue;
                const Entity* e = ctx.doc().find(id);
                const auto seg = e ? lastSegment(*e) : std::nullopt;
                const auto end = e ? landingOf(*e) : std::nullopt;
                if (!seg || !end)
                    continue;
                const Vec2 from = *end - *seg;
                const Vec2 u = normalized(*refDir);
                // the same sense as before, so the note stays on its side
                const double len = length(*seg) * (dot(*seg, u) >= 0 ? 1.0 : -1.0);
                moveLanding(ctx, id, from + u * len - *end);
                ++n;
            }
            ctx.message(std::to_string(n) + " leaders made parallel.");
            co_return;
        }
        ctx.setUndoLabel("MLEADERALIGN");

        // Target landing of every leader, the reference included.
        struct Item {
            EntityId id;
            Vec2 end;
        };
        std::vector<Item> items;
        for (EntityId id : sel.ids) {
            if (id == ref.id)
                continue;
            const Entity* e = ctx.doc().find(id);
            if (const auto end = e ? landingOf(*e) : std::nullopt)
                items.push_back({id, *end});
        }
        items.push_back({ref.id, *anchor});
        auto along = [&](const Item& it) { return vertical ? it.end.y : it.end.x; };
        std::stable_sort(items.begin(), items.end(),
                         [&](const Item& x, const Item& y) { return along(x) < along(y); });
        std::size_t refIndex = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].id == ref.id)
                refIndex = i;

        std::size_t n = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& it = items[i];
            if (it.id == ref.id)
                continue;
            const Vec2 target = vertical ? Vec2{anchor->x, spacing > 0 ? anchor->y : it.end.y}
                                         : Vec2{spacing > 0 ? anchor->x : it.end.x, anchor->y};
            Vec2 want = target;
            if (spacing > 0) {
                const double off =
                    (static_cast<double>(i) - static_cast<double>(refIndex)) * spacing;
                (vertical ? want.y : want.x) += off;
            }
            if (!moveLanding(ctx, it.id, want - it.end))
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
// AIMLEADEREDITADD and AIMLEADEREDITREMOVE (the ribbon's Add Leader / Remove Leader buttons in
// AutoCAD) are the same command with the choice made.
class MLeaderEditCommand final : public cmd::Command {
public:
    MLeaderEditCommand() = default;
    MLeaderEditCommand(std::string name, std::string mode)
        : name_(std::move(name)), mode_(std::move(mode)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override {
        return mode_.empty() ? std::vector<std::string>{"MLE"} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        return mode_ == "Add"      ? "Add leader lines (arrowheads) to a multileader"
               : mode_ == "Remove" ? "Remove leader lines (arrowheads) from a multileader"
                                   : "Add or remove leader lines (arrowheads) of a multileader";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return entityAs<MLeaderData>(e) != nullptr; };
        auto pick = co_await ctx.getEntity("Select a multileader", eo);
        if (!pick.ok())
            co_return;
        std::string mode = mode_;
        if (mode.empty()) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add", "Remove"};
            ko.defaultKeyword = "Add";
            auto k = co_await ctx.getKeyword("Leader lines [Add/Remove] <Add>", ko);
            if (!k.ok())
                co_return;
            mode = k.keyword;
        }
        if (mode == "Add") {
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

private:
    std::string name_ = "MLEADEREDIT";
    std::string mode_{};
};

}  // namespace

std::unique_ptr<cmd::Command> makeSmartDimCommand() {
    return std::make_unique<SmartDimCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderEditCommand() {
    return std::make_unique<MLeaderEditCommand>();
}

std::unique_ptr<cmd::Command> makeMLeaderEditAddCommand() {
    return std::make_unique<MLeaderEditCommand>("AIMLEADEREDITADD", "Add");
}

std::unique_ptr<cmd::Command> makeMLeaderEditRemoveCommand() {
    return std::make_unique<MLeaderEditCommand>("AIMLEADEREDITREMOVE", "Remove");
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
