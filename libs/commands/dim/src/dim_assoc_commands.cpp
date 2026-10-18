// DIMREASSOC and DIMDISASSOC: link dimensions to the objects they measure, or cut the link.
#include <algorithm>
#include <cstdio>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"

namespace tbcad::dimcmd {

namespace {

bool isDim(const Entity& e) {
    return entityAs<DimensionData>(e) != nullptr;
}

class DimReassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMREASSOC"; }
    std::string_view description() const override {
        return "Link dimensions to the objects whose points they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
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
            const int n = linkDimension(ctx.doc(), d, ctx.doc().ownerBlock(id));
            if (!ctx.tx().modify(id, [&](Entity& x) { *entityAs<DimensionData>(x) = d; }))
                continue;
            if (n > 0)
                ++dims;
            points += n;
        }
        ctx.message(
            std::to_string(dims) + " dimensions linked at " + std::to_string(points) +
            " points. A point is linked where it sits exactly on an end, vertex or centre.");
    }
};

class DimDisassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMDISASSOC"; }
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
