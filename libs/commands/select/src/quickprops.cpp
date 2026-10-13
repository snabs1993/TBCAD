#include "common.h"

namespace tbcad::select::detail {

namespace {

// QUICKPROPERTIES [QP]: show or hide the small properties box over the viewport (the application
// reads the setting "ui.quickProps").
class QuickProperties final : public cmd::Command {
public:
    std::string_view name() const override { return "QUICKPROPERTIES"; }
    std::vector<std::string> defaultAliases() const override { return {"QP"}; }
    std::string_view description() const override {
        return "Show or hide the quick properties box for the selection";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool on = ctx.host().setting("ui.quickProps").value_or("0") != "1";
        ctx.host().setSetting("ui.quickProps", on ? "1" : "0");
        ctx.message(on ? "Quick properties on." : "Quick properties off.");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeQuickProperties() {
    return std::make_unique<QuickProperties>();
}

}  // namespace tbcad::select::detail
