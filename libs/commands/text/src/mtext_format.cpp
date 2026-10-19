// MTEXTFORMAT: the settings of a whole MTEXT that markup cannot carry: background mask (hides
// what is behind, or a colour) with its border factor, and the line spacing factor.
#include <algorithm>
#include <cmath>

#include "commands.h"

namespace tbcad::textcmd {

namespace {

class MTextFormatCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MTEXTFORMAT"; }
    std::vector<std::string> defaultAliases() const override { return {"MTF"}; }
    std::string_view description() const override {
        return "Background mask and line spacing of multiline text";
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
        cmd::KeywordOptions ko;
        ko.keywords = {"Background", "LineSpacing"};
        ko.defaultKeyword = "Background";
        auto what = co_await ctx.getKeyword("Setting [Background/LineSpacing] <Background>", ko);
        if (!what.ok())
            co_return;

        std::uint8_t mode = 0;
        int color = 7;
        double scale = 1.5, spacing = 1.0;
        if (what.keyword == "LineSpacing") {
            cmd::RealOptions ro;
            ro.defaultValue = 1.0;
            auto r = co_await ctx.getReal("Line spacing factor, 0.25 to 4 <1>", ro);
            if (!r.ok())
                co_return;
            spacing = std::clamp(r.value, 0.25, 4.0);
        } else {
            cmd::KeywordOptions bo;
            bo.keywords = {"None", "Window", "Colour"};
            bo.defaultKeyword = "Window";
            auto b = co_await ctx.getKeyword("Background [None/Window/Colour] <Window>", bo);
            if (!b.ok())
                co_return;
            if (b.keyword == "Colour") {
                mode = 2;
                cmd::IntOptions io;
                io.defaultValue = 7;
                auto c = co_await ctx.getInteger("Palette colour 1 to 255 <7>", io);
                if (!c.ok())
                    co_return;
                color = static_cast<int>(std::clamp<long long>(c.value, 1, 255));
            } else if (b.keyword != "None") {
                mode = 1;
            }
            if (mode != 0) {
                cmd::RealOptions ro;
                ro.defaultValue = 1.5;
                auto s = co_await ctx.getReal("Border factor, 1 to 5 <1.5>", ro);
                if (!s.ok())
                    co_return;
                scale = std::clamp(s.value, 1.0, 5.0);
            }
        }
        ctx.setUndoLabel("MTEXTFORMAT");
        for (EntityId id : ids)
            (void)ctx.tx().modify(id, [&](Entity& e) {
                auto* m = entityAs<MTextData>(e);
                if (!m)
                    return;
                if (what.keyword == "LineSpacing") {
                    m->lineSpacing = spacing;
                } else {
                    m->bgMode = mode;
                    m->bgColor = color;
                    m->bgScale = scale;
                }
            });
        ctx.message(std::to_string(ids.size()) + " multiline text object(s) changed.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMTextFormatCommand() {
    return std::make_unique<MTextFormatCommand>();
}

}  // namespace tbcad::textcmd
