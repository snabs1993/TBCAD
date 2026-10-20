// VPORTS opens the Viewports dialog; -VPORTS splits the model space drawing area into tiled
// viewports, each with its own view (the window does the work after the command ends).
#include "commands.h"

namespace tbcad::sysvars {

namespace {

class VportsDialogCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPORTS"; }
    std::string_view description() const override {
        return "Open the Viewports dialog: tiled model viewports and saved configurations";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("vports");
        co_return;
    }
};

class VportsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-VPORTS"; }
    std::string_view description() const override {
        return "Split model space into tiled viewports, save and restore the configurations";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Save", "Restore", "Delete", "SIngle", "List", "2", "3", "4"};
        ko.defaultKeyword = "3";
        auto k = co_await ctx.getKeyword(
            "Enter an option [Save/Restore/Delete/SIngle/List/2/3/4] <3>", ko);
        if (!k.ok())
            co_return;
        cmd::IHost& host = ctx.host();
        if (k.keyword == "SIngle") {
            host.requestUi("vports:1");
        } else if (k.keyword == "List") {
            host.requestUi("vports:list");
        } else if (k.keyword == "2") {
            cmd::KeywordOptions o;
            o.keywords = {"Horizontal", "Vertical"};
            o.defaultKeyword = "Vertical";
            auto d = co_await ctx.getKeyword(
                "Enter a configuration option [Horizontal/Vertical] "
                "<Vertical>",
                o);
            if (d.ok())
                host.requestUi(d.keyword == "Horizontal" ? "vports:2H" : "vports:2V");
        } else if (k.keyword == "3") {
            cmd::KeywordOptions o;
            o.keywords = {"Horizontal", "Vertical", "Above", "Below", "Left", "Right"};
            o.defaultKeyword = "Right";
            auto d = co_await ctx.getKeyword(
                "Enter a configuration option [Horizontal/Vertical/Above/Below/Left/Right] "
                "<Right>",
                o);
            if (d.ok())
                host.requestUi(std::string("vports:3") + d.keyword.front());
        } else if (k.keyword == "4") {
            host.requestUi("vports:4");
        } else {
            auto n = co_await ctx.getString(k.keyword == "Save"
                                                ? "Enter name for new viewport configuration"
                                                : "Enter name of viewport configuration");
            if (!n.ok())
                co_return;
            std::string name(cmd::trim(n.text));
            if (name.empty())
                co_return;
            for (char& c : name)
                if (c == '\n' || c == '\t')
                    c = ' ';
            host.requestUi("vports:" +
                           std::string(k.keyword == "Save"      ? "save:"
                                       : k.keyword == "Restore" ? "restore:"
                                                                : "delete:") +
                           name);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeVportsCommand() {
    return std::make_unique<VportsCommand>();
}
std::unique_ptr<cmd::Command> makeVportsDialogCommand() {
    return std::make_unique<VportsDialogCommand>();
}

}  // namespace tbcad::sysvars
