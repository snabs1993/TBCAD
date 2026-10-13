// DIMCONTINUE, DIMBASELINE and DIMTEDIT: chains of linear dimensions and dimension text editing.
#include <algorithm>
#include <cmath>

#include "dim_internal.h"
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

// The most recently drawn linear or aligned dimension of model space.
EntityId lastLengthDim(const Document& doc) {
    const auto& ms = doc.modelSpace();
    for (auto it = ms.rbegin(); it != ms.rend(); ++it) {
        const Entity* e = doc.find(*it);
        if (e && isLengthDim(*e))
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
        EntityId baseId = lastLengthDim(ctx.doc());
        for (;;) {
            if (!baseId) {
                cmd::EntityOptions eo;
                eo.filter = isLengthDim;
                auto o = co_await ctx.getEntity("Select base dimension", eo);
                if (!o.ok())
                    co_return;
                baseId = o.id;
            }
            const Entity* be = ctx.doc().find(baseId);
            const auto* base = be ? entityAs<DimensionData>(*be) : nullptr;
            if (!base || !isLengthDim(*be)) {
                ctx.message("Select a linear or aligned dimension.");
                co_return;
            }
            DimensionData prev = *base;
            const DimStyle& st = dim::styleOf(ctx.doc(), prev.style);
            const double k = st.overallScale > 0 ? st.overallScale : 1.0;
            const double spacing = st.textHeight * 2.5 * k;

            Vec2 origin = baseline_ ? prev.defPts[0] : prev.defPts[1];
            auto* c = &ctx;
            const bool baseline = baseline_;
            auto build = [=](Vec2 second) {
                DimensionData d = prev;
                d.defPts[0] = origin;
                d.defPts[1] = second;
                d.defPts[2] = baseline ? prev.defPts[2] + normalOf(prev) * (sideOf(prev) * spacing)
                                       : prev.defPts[2];
                d.textPosUser = false;
                d.textOverride.clear();
                return d;
            };
            cmd::PointOptions po;
            po.base = prev.defPts[1];
            po.keywords = {"Select"};
            po.preview = [c, build](Vec2 cursor, DisplayList& out) {
                DimensionData d = build(cursor);
                dim::refresh(c->doc(), d);
                Entity e;
                e.data = Indirect<DimensionData>(std::move(d));
                previewEntity(*c, std::move(e), out);
            };
            auto next =
                co_await ctx.getPoint("Specify second extension line origin or [Select]", po);
            if (next.status == Status::Keyword) {
                baseId = EntityId{};
                continue;
            }
            if (!next.ok())
                co_return;
            DimensionData d = build(next.point);
            if (distance(d.defPts[0], d.defPts[1]) < tol::kLinear) {
                ctx.message("The extension line origins coincide.");
                continue;
            }
            dim::refresh(ctx.doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            ctx.setUndoLabel(std::string(ctx.commandName()));
            auto id = ctx.tx().add(std::move(e));
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
            // The next one chains from the dimension just made.
            baseId = id.value();
        }
    }

private:
    std::string name_, alias_;
    bool baseline_;
    std::string description_;
};

class TextEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"DTE"}; }
    std::string_view description() const override {
        return "Move dimension text, or reset it to its automatic place";
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
        po.keywords = {"Home"};
        auto* c = &ctx;
        po.preview = [c, original](Vec2 cursor, DisplayList& out) {
            DimensionData d = original;
            d.textPos = cursor;
            d.textPosUser = true;
            dim::refresh(c->doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            previewEntity(*c, std::move(e), out);
        };
        auto where = co_await ctx.getPoint("Specify new text location or [Home]", po);
        const bool home = where.status == Status::Keyword && where.keyword == "Home";
        if (!where.ok() && !home)
            co_return;
        ctx.setUndoLabel("DIMTEDIT");
        auto r = ctx.tx().modify(id, [&](Entity& ent) {
            auto* d = entityAs<DimensionData>(ent);
            if (!d)
                return;
            if (home) {
                d->textPosUser = false;
            } else {
                d->textPos = where.point;
                d->textPosUser = true;
            }
            dim::refresh(ctx.doc(), *d);
        });
        if (!r)
            ctx.message(r.error().message);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeContinueCommand() {
    return std::make_unique<ChainCommand>("DIMCONTINUE", "DCO", false,
                                          "Continue a chain of dimensions from the last one");
}

std::unique_ptr<cmd::Command> makeBaselineCommand() {
    return std::make_unique<ChainCommand>("DIMBASELINE", "DBA", true,
                                          "Stack dimensions from the same first extension line");
}

std::unique_ptr<cmd::Command> makeTextEditCommand() {
    return std::make_unique<TextEditCommand>();
}

}  // namespace tbcad::dimcmd
