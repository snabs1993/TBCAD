// TCOUNT: numbers the selected single-line text objects in the order they were selected.
#include <cmath>
#include <cstdio>

#include "commands.h"

namespace tbcad::textcmd {

namespace {

std::string numberText(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

class TCountCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TCOUNT"; }
    std::string_view description() const override {
        return "Number the selected text objects in the order they are selected";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            const auto* t = entityAs<TextData>(e);
            return t && !t->attdef;
        };
        auto sel = co_await ctx.getSelection("Select text objects to number", so);
        if (!sel.ok())
            co_return;
        cmd::RealOptions ro;
        ro.defaultValue = 1.0;
        auto start = co_await ctx.getReal("Starting number <1>", ro);
        if (!start.ok())
            co_return;
        ro.defaultValue = 1.0;
        auto step = co_await ctx.getReal("Increment <1>", ro);
        if (!step.ok())
            co_return;
        cmd::StringOptions pre;
        pre.allowEmpty = true;
        auto prefix = co_await ctx.getString("Prefix (Enter for none)", pre);
        if (prefix.status == cmd::Status::Cancel)
            co_return;
        auto suffix = co_await ctx.getString("Suffix (Enter for none)", pre);
        if (suffix.status == cmd::Status::Cancel)
            co_return;
        // As many decimals as the start or the increment have.
        int decimals = 0;
        for (double v : {start.value, step.value})
            while (decimals < 6 && std::abs(v * std::pow(10, decimals) -
                                            std::round(v * std::pow(10, decimals))) > 1e-9)
                ++decimals;

        ctx.setUndoLabel("TCOUNT");
        double n = start.value;
        for (EntityId id : sel.ids) {
            const std::string text = (prefix.ok() ? prefix.text : std::string()) +
                                     numberText(n, decimals) +
                                     (suffix.ok() ? suffix.text : std::string());
            (void)ctx.tx().modify(id, [&](Entity& e) {
                if (auto* t = entityAs<TextData>(e))
                    t->utf8 = text;
            });
            n += step.value;
        }
        ctx.message(std::to_string(sel.ids.size()) + " text objects numbered.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTCountCommand() {
    return std::make_unique<TCountCommand>();
}

}  // namespace tbcad::textcmd
