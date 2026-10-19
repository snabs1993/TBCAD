// DIMREASSOC and DIMDISASSOC: link dimensions to the objects they measure, or cut the link.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"

namespace tbcad::dimcmd {

namespace {

bool isDim(const Entity& e) {
    return entityAs<DimensionData>(e) != nullptr;
}

// How many definition points of a dimension DIMREASSOC asks for, and what each one is.
std::vector<std::string> assocPrompts(const DimensionData& d) {
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            return {"first extension line origin", "second extension line origin"};
        case DimKind::Radius:
            return {"centre", "point on the arc or circle"};
        case DimKind::Diameter:
            return {"first diameter point", "second diameter point"};
        case DimKind::Angular3Point:
            return {"first angle endpoint", "second angle endpoint", "angle vertex"};
        case DimKind::Angular2Line:
            return {"start of the first line", "end of the first line", "start of the second line",
                    "end of the second line"};
        default:
            return {};
    }
}

// The link for a point picked (snapped) on an object: a vertex or centre, else a point on a
// circle or arc.
std::optional<DimAssoc> assocAt(const Document& doc, Vec2 p, BlockId owner) {
    if (auto a = findDimAssoc(doc, p, owner))
        return a;
    return findDimAssocOnCurve(doc, p, owner);
}

// DIMREASSOCIATE, as in AutoCAD: for each dimension, each of its points in turn. A picked point
// (on an end, vertex, centre or on a circle) links it there; Enter keeps the point, linking it
// where it already sits on an object; Select object links a linear dimension to both ends of a
// line or arc, or a radius or diameter dimension to a circle or arc.
class DimReassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMREASSOC"; }
    std::vector<std::string> defaultAliases() const override { return {"DIMREASSOCIATE"}; }
    std::string_view description() const override {
        return "Link dimensions to the objects whose points they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to re-associate", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMREASSOC");
        int dims = 0, points = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isDim(*e))
                continue;
            DimensionData d = *entityAs<DimensionData>(*e);
            const BlockId owner = ctx.doc().ownerBlock(id);
            const std::vector<std::string> what = assocPrompts(d);
            int linked = 0;
            for (std::size_t i = 0; i < what.size(); ++i) {
                cmd::PointOptions po;
                const bool canSelect =
                    i == 0 && (d.kind == DimKind::Linear || d.kind == DimKind::Aligned ||
                               d.kind == DimKind::Radius || d.kind == DimKind::Diameter);
                if (canSelect)
                    po.keywords = {"Select"};
                auto r = co_await ctx.getPoint(
                    "Specify " + what[i] + (canSelect ? " or [Select object]" : "") + " <next>",
                    po);
                if (r.status == Status::Cancel)
                    co_return;
                if (r.status == Status::Keyword && r.keyword == "Select") {
                    cmd::EntityOptions eo;
                    eo.filter = [](const Entity& t) {
                        return entityAs<LineData>(t) || entityAs<ArcData>(t) ||
                               entityAs<CircleData>(t);
                    };
                    auto o = co_await ctx.getEntity("Select object", eo);
                    if (o.status == Status::Cancel)
                        co_return;
                    if (o.ok())
                        linked += linkToObject(ctx.doc(), d, o.id);
                    break;
                }
                std::optional<DimAssoc> a;
                if (r.ok()) {
                    a = assocAt(ctx.doc(), r.point, owner);
                    if (!a)
                        ctx.message("No end, vertex, centre or curve at that point; the " +
                                    what[i] + " is kept.");
                } else {
                    a = assocAt(ctx.doc(), d.defPts[i], owner);  // Enter: where it already sits
                }
                if (!a)
                    continue;
                const Entity* t = ctx.doc().find(a->target);
                if (const auto p = t ? dimAssocPoint(*t, *a) : std::nullopt) {
                    d.defPts[i] = *p;
                    d.assoc[i] = *a;
                    ++linked;
                }
            }
            const Document& doc = ctx.doc();
            if (!ctx.tx().modify(id, [&](Entity& x) {
                    *entityAs<DimensionData>(x) = d;
                    if (auto fn = dimRefresh())
                        fn(doc, x);
                }))
                continue;
            if (linked > 0)
                ++dims;
            points += linked;
        }
        ctx.message(std::to_string(dims) + " dimensions linked at " + std::to_string(points) +
                    " points.");
    }

private:
    // Select object: a line or arc gives a linear dimension its two ends (the nearer one first),
    // a circle or arc gives a radius or diameter dimension its centre and curve points.
    static int linkToObject(const Document& doc, DimensionData& d, EntityId target) {
        const Entity* t = doc.find(target);
        if (!t)
            return 0;
        int n = 0;
        auto link = [&](std::size_t i, DimAssoc a) {
            a.target = target;
            if (const auto p = dimAssocPoint(*t, a)) {
                d.defPts[i] = *p;
                d.assoc[i] = a;
                ++n;
            }
        };
        using K = DimAssoc::Kind;
        if (d.kind == DimKind::Linear || d.kind == DimKind::Aligned) {
            if (!entityAs<LineData>(*t) && !entityAs<ArcData>(*t))
                return 0;
            const auto a0 = dimAssocPoint(*t, {K::Vertex, target, 0});
            const auto a1 = dimAssocPoint(*t, {K::Vertex, target, 1});
            if (!a0 || !a1)
                return 0;
            const bool swap = distance(*a0, d.defPts[0]) + distance(*a1, d.defPts[1]) >
                              distance(*a1, d.defPts[0]) + distance(*a0, d.defPts[1]);
            link(0, {K::Vertex, {}, swap ? 1u : 0u});
            link(1, {K::Vertex, {}, swap ? 0u : 1u});
            return n;
        }
        const CircleData* c = entityAs<CircleData>(*t);
        const ArcData* arc = entityAs<ArcData>(*t);
        if (!c && !arc)
            return 0;
        const Vec2 centre = c ? c->center : arc->center;
        auto onCurve = [&](Vec2 toward) {
            Vec2 v = toward - centre;
            if (tol::isZeroLength(v))
                v = {1, 0};
            const double ang = normalizeAngle(std::atan2(v.y, v.x));
            return DimAssoc{K::OnCurve,
                            {},
                            static_cast<std::uint32_t>(std::llround(ang / kTwoPi * 4294967296.0) &
                                                       0xFFFFFFFFll)};
        };
        if (d.kind == DimKind::Radius) {
            link(0, {K::Center, {}, 0});
            link(1, onCurve(d.defPts[1]));
        } else if (d.kind == DimKind::Diameter) {
            const Vec2 v = d.defPts[0] - centre;
            link(0, onCurve(d.defPts[0]));
            link(1, onCurve(centre - v));
        }
        return n;
    }
};

class DimDisassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMDISASSOC"; }
    std::vector<std::string> defaultAliases() const override { return {"DIMDISASSOCIATE"}; }
    std::string_view description() const override {
        return "Cut the links between dimensions and the objects they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to disassociate", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMDISASSOC");
        int n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isDim(*e))
                continue;
            bool linked = false;
            for (const DimAssoc& a : entityAs<DimensionData>(*e)->assoc)
                linked = linked || a.kind != DimAssoc::Kind::None;
            if (!linked)
                continue;
            if (ctx.tx().modify(id, [](Entity& x) { entityAs<DimensionData>(x)->assoc = {}; }))
                ++n;
        }
        ctx.message(std::to_string(n) + " dimensions disassociated.");
    }
};

// DIMREGEN: every dimension takes the current position of the points it follows and refreshes its
// measurement and text.
class DimRegenCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMREGEN"; }
    std::string_view description() const override {
        return "Update all associative dimensions to the objects they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("DIMREGEN");
        int n = 0;
        for (EntityId id : ctx.doc().modelSpace()) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isDim(*e))
                continue;
            DimensionData d = *entityAs<DimensionData>(*e);
            for (std::size_t i = 0; i < d.assoc.size(); ++i) {
                if (d.assoc[i].kind == DimAssoc::Kind::None)
                    continue;
                const Entity* t = ctx.doc().find(d.assoc[i].target);
                if (!t)
                    continue;
                if (auto p = dimAssocPoint(*t, d.assoc[i]))
                    d.defPts[i] = *p;
            }
            const Document& doc = ctx.doc();
            if (ctx.tx().modify(id, [&](Entity& x) {
                    *entityAs<DimensionData>(x) = d;
                    if (auto fn = dimRefresh())
                        fn(doc, x);
                }))
                ++n;
        }
        ctx.message(std::to_string(n) + " dimensions updated.");
        co_return;
    }
};

// DIMINSPECT: marks dimensions as inspection dimensions, written after the measurement as
// "<> [label rate%]"; Remove takes the mark off again.
class DimInspectCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMINSPECT"; }
    std::string_view description() const override {
        return "Add or remove an inspection label and rate on dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to inspect", so);
        if (!sel.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Add", "Remove"};
        ko.defaultKeyword = "Add";
        auto act = co_await ctx.getKeyword("Inspection [Add/Remove] <Add>", ko);
        if (!act.ok())
            co_return;
        std::string label;
        double rate = 100;
        if (act.keyword == "Add") {
            cmd::StringOptions so2;
            so2.allowEmpty = true;
            auto l = co_await ctx.getString("Inspection label (Enter for none)", so2);
            if (l.status == cmd::Status::Cancel)
                co_return;
            if (l.ok())
                label = cmd::trim(l.text);
            cmd::RealOptions ro;
            ro.defaultValue = 100.0;
            auto r = co_await ctx.getReal("Inspection rate in percent <100>", ro);
            if (!r.ok())
                co_return;
            rate = std::clamp(r.value, 0.0, 100.0);
        }
        ctx.setUndoLabel("DIMINSPECT");
        char rateText[32];
        std::snprintf(rateText, sizeof rateText, "%g", rate);
        const std::string mark = " [" + (label.empty() ? "" : label + " ") + rateText + "%]";
        for (EntityId id : sel.ids) {
            (void)ctx.tx().modify(id, [&](Entity& x) {
                auto* d = entityAs<DimensionData>(x);
                if (!d)
                    return;
                std::string t = d->textOverride.empty() ? "<>" : d->textOverride;
                const std::size_t at = t.rfind(" [");
                if (at != std::string::npos && t.back() == ']')
                    t.erase(at);  // an earlier inspection mark
                d->textOverride = act.keyword == "Add" ? t + mark : (t == "<>" ? "" : t);
            });
        }
        ctx.message(std::string("Inspection ") +
                    (act.keyword == "Add" ? "added to " : "removed from ") +
                    std::to_string(sel.ids.size()) + " dimensions.");
    }
};

// DIMUPDATE: gives the selected dimensions the current dimension style and refreshes them.
class DimUpdateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMUPDATE"; }
    std::string_view description() const override {
        return "Apply the current dimension style to the selected dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to update", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMUPDATE");
        const DimStyleId style = ctx.doc().header().currentDimStyle;
        const Document& doc = ctx.doc();
        int n = 0;
        for (EntityId id : sel.ids) {
            if (ctx.tx().modify(id, [&](Entity& x) {
                    if (auto* d = entityAs<DimensionData>(x)) {
                        d->style = style;
                        if (auto fn = dimRefresh())
                            fn(doc, x);
                    }
                }))
                ++n;
        }
        ctx.message(std::to_string(n) + " dimensions updated.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeDimUpdateCommand() {
    return std::make_unique<DimUpdateCommand>();
}
std::unique_ptr<cmd::Command> makeDimRegenCommand() {
    return std::make_unique<DimRegenCommand>();
}
std::unique_ptr<cmd::Command> makeDimInspectCommand() {
    return std::make_unique<DimInspectCommand>();
}
std::unique_ptr<cmd::Command> makeDimReassocCommand() {
    return std::make_unique<DimReassocCommand>();
}
std::unique_ptr<cmd::Command> makeDimDisassocCommand() {
    return std::make_unique<DimDisassocCommand>();
}

}  // namespace tbcad::dimcmd
