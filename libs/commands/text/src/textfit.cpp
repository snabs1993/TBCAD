// TEXTFIT: stretches single-line text to run exactly between two picked points (the Fit
// justification, with the rotation taken from the points). MTPROP: selects multiline text and
// opens the Properties panel on it.
#include "commands.h"
#include "tbcad/geom/entity.h"

namespace tbcad::textcmd {

namespace {

class TextFitCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXTFIT"; }
    std::string_view description() const override {
        return "Fit single-line text between two points";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return entityAs<TextData>(e) != nullptr; };
        auto sel = co_await ctx.getSelection("Select single-line text to fit", so);
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && so.filter(*e))
                ids.push_back(id);
        if (ids.empty()) {
            ctx.message("No single-line text selected.");
            co_return;
        }
        auto p1 = co_await ctx.getPoint("Specify text start point", cmd::PointOptions{});
        if (!p1.ok())
            co_return;
        cmd::PointOptions po;
        po.base = p1.point;
        auto p2 = co_await ctx.getPoint("Specify text end point", po);
        if (!p2.ok())
            co_return;
        const Vec2 span = p2.point - p1.point;
        if (length(span) < tol::kLinear) {
            ctx.message("The two points coincide.");
            co_return;
        }
        ctx.setUndoLabel("TEXTFIT");
        std::size_t n = 0;
        for (EntityId id : ids) {
            const bool ok = ctx.tx()
                                .modify(id,
                                        [&](Entity& e) {
                                            auto* t = entityAs<TextData>(e);
                                            if (!t)
                                                return;
                                            t->h = HAlign::Fit;
                                            t->v = VAlign::Baseline;
                                            t->position = p1.point;
                                            t->alignPoint = p2.point;
                                            t->rotation = std::atan2(span.y, span.x);
                                        })
                                .ok();
            if (ok)
                ++n;
        }
        ctx.message(std::to_string(n) + " text objects fitted.");
    }
};

class MTextPropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MTPROP"; }
    std::string_view description() const override {
        return "Open the Properties panel for multiline text";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return entityAs<MTextData>(e) != nullptr; };
        auto sel = co_await ctx.getSelection("Select multiline text", so);
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && so.filter(*e))
                ids.push_back(id);
        if (ids.empty()) {
            ctx.message("No multiline text selected.");
            co_return;
        }
        ctx.host().setSelection(ids);
        ctx.host().openPanel("properties");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTextFitCommand() {
    return std::make_unique<TextFitCommand>();
}
std::unique_ptr<cmd::Command> makeMTextPropCommand() {
    return std::make_unique<MTextPropCommand>();
}

}  // namespace tbcad::textcmd
