// VIEW: named views. A view is the centre and scale of the display, saved in the drawing
// (Header::custom "tbcad.view.<name>").
#include <cstdio>

#include "commands.h"

namespace tbcad::sysvars {

namespace {

constexpr const char* kPrefix = "tbcad.view.";

std::string viewKey(const std::string& name) {
    return kPrefix + name;
}

class ViewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"NAMEDVIEW"}; }
    std::string_view description() const override {
        return "Save the current view under a name, restore a saved view, delete or list them";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Restore", "Save", "Delete", "List"};
        ko.defaultKeyword = "Restore";
        auto k = co_await ctx.getKeyword("View [Restore/Save/Delete/List] <Restore>", ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "List") {
            std::size_t n = 0;
            for (const auto& [key, value] : ctx.doc().header().custom)
                if (key.rfind(kPrefix, 0) == 0) {
                    ctx.message(key.substr(std::string_view(kPrefix).size()) + "  (" + value + ")");
                    ++n;
                }
            if (n == 0)
                ctx.message("There are no saved views.");
            co_return;
        }
        auto nameIn = co_await ctx.getString("View name");
        if (!nameIn.ok())
            co_return;
        const std::string name(cmd::trim(nameIn.text));
        if (name.empty())
            co_return;
        const std::string key = viewKey(name);
        const auto& custom = ctx.doc().header().custom;
        const auto it = custom.find(key);

        if (k.keyword == "Save") {
            const cmd::ViewState v = ctx.host().view();
            char buf[96];
            std::snprintf(buf, sizeof buf, "%.9g %.9g %.9g", v.center.x, v.center.y, v.scale);
            const std::string text = buf;
            ctx.setUndoLabel("VIEW Save");
            if (auto r = ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = text; }); !r)
                ctx.message(r.error().message);
            else
                ctx.message("View \"" + name + "\" saved.");
        } else if (k.keyword == "Delete") {
            if (it == custom.end()) {
                ctx.message("There is no view named \"" + name + "\".");
                co_return;
            }
            ctx.setUndoLabel("VIEW Delete");
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom.erase(key); });
            ctx.message("View \"" + name + "\" deleted.");
        } else {
            if (it == custom.end()) {
                ctx.message("There is no view named \"" + name + "\".");
                co_return;
            }
            double x = 0, y = 0, s = 0;
            if (std::sscanf(it->second.c_str(), "%lf %lf %lf", &x, &y, &s) != 3 || !(s > 0)) {
                ctx.message("The saved view \"" + name + "\" is damaged.");
                co_return;
            }
            cmd::ViewState v = ctx.host().view();
            v.center = {x, y};
            v.scale = s;
            ctx.host().setView(v);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeViewCommand() {
    return std::make_unique<ViewCommand>();
}

}  // namespace tbcad::sysvars
