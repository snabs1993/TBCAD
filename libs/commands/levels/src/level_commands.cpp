#include <charconv>
#include <cmath>
#include <sstream>

#include "commands.h"
#include "tbcad/levels/level_ops.h"

namespace tbcad::levels {

namespace {

using cmd::CommandContext;
using cmd::Task;

Task askText(CommandContext& ctx, std::string prompt, std::string& out, bool& ok) {
    auto r = co_await ctx.getString(std::move(prompt));
    ok = r.ok() && !r.text.empty();
    if (ok)
        out = std::move(r.text);
}

Task askLevel(CommandContext& ctx, std::string prompt, LevelId& out, bool& ok) {
    std::string text;
    co_await askText(ctx, std::move(prompt), text, ok);
    if (!ok)
        co_return;
    auto r = resolveLevel(ctx.doc(), text);
    if (!r) {
        ctx.message(r.error().message);
        ok = false;
        co_return;
    }
    out = r.value();
}

bool parseNumber(const std::string& text, double& out) {
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    std::size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos)
        return false;
    t = t.substr(b);
    auto r = std::from_chars(t.data(), t.data() + t.size(), out);
    return r.ec == std::errc() && r.ptr == t.data() + t.size() && std::isfinite(out);
}

void report(CommandContext& ctx, const Result<void>& r) {
    if (!r)
        ctx.message(r.error().message);
}

class LevelPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LEVEL"; }
    std::vector<std::string> defaultAliases() const override { return {"LV"}; }
    std::string_view description() const override { return "Open the level panel"; }
    Task run(CommandContext& ctx) override {
        ctx.host().setSetting("ui.showPanel", "levels");
        ctx.message("Level panel. Use -LEVEL to change levels from the command line.");
        co_return;
    }
};

class LevelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LEVEL"; }
    std::vector<std::string> defaultAliases() const override { return {"-LV"}; }
    std::string_view description() const override {
        return "Create levels, switch the level you work on and change how others show";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("LEVEL");
        cmd::KeywordOptions opts;
        opts.keywords = {"New",  "Set",    "Rename", "Elevation", "Show",   "Hide",
                         "Lock", "Unlock", "View",   "Move",      "Delete", "List"};
        while (true) {
            auto k = co_await ctx.getKeyword("Level option", opts);
            if (!k.ok())
                co_return;
            co_await runOption(ctx, k.keyword);
        }
    }

private:
    static Task runOption(CommandContext& ctx, std::string option) {
        Document& doc = ctx.doc();
        bool ok = false;
        if (option == "New") {
            std::string name;
            co_await askText(ctx, "Name for the new level", name, ok);
            if (!ok)
                co_return;
            double elevation = 0;
            std::string e;
            co_await askText(ctx, "Elevation in mm (a number)", e, ok);
            if (ok && !parseNumber(e, elevation)) {
                ctx.message("Not a number: " + e);
                co_return;
            }
            auto id = createLevel(doc, ctx.tx(), name, elevation);
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
            report(ctx, setCurrentLevel(doc, ctx.tx(), id.value()));
            ctx.message("Created level " + describe(doc, id.value()) + " and made it current.");
        } else if (option == "Set") {
            LevelId id;
            co_await askLevel(ctx, "Level to work on", id, ok);
            if (!ok)
                co_return;
            report(ctx, setCurrentLevel(doc, ctx.tx(), id));
            ctx.message("Current level: " + describe(doc, id) + ".");
        } else if (option == "Rename") {
            LevelId id;
            co_await askLevel(ctx, "Level to rename", id, ok);
            if (!ok)
                co_return;
            std::string name;
            co_await askText(ctx, "New name", name, ok);
            if (!ok)
                co_return;
            report(ctx, renameLevel(doc, ctx.tx(), id, name));
        } else if (option == "Elevation") {
            LevelId id;
            co_await askLevel(ctx, "Level", id, ok);
            if (!ok)
                co_return;
            std::string e;
            double v = 0;
            co_await askText(ctx, "Elevation in mm", e, ok);
            if (!ok)
                co_return;
            if (!parseNumber(e, v)) {
                ctx.message("Not a number: " + e);
                co_return;
            }
            report(ctx, setElevation(doc, ctx.tx(), id, v));
        } else if (option == "Show" || option == "Hide" || option == "Lock" || option == "Unlock") {
            LevelId id;
            co_await askLevel(ctx, "Level name", id, ok);
            if (!ok)
                co_return;
            if (option == "Show")
                report(ctx, setVisible(doc, ctx.tx(), id, true));
            else if (option == "Hide")
                report(ctx, setVisible(doc, ctx.tx(), id, false));
            else
                report(ctx, setLocked(doc, ctx.tx(), id, option == "Lock"));
        } else if (option == "View") {
            std::string text;
            co_await askText(ctx, "Show other levels: Dim, Hide or All", text, ok);
            if (!ok)
                co_return;
            auto v = parseLevelView(text);
            if (!v) {
                ctx.message(v.error().message);
                co_return;
            }
            report(ctx, setLevelView(doc, ctx.tx(), v.value()));
            ctx.message(std::string("Other levels: ") + levelViewName(v.value()) + ".");
        } else if (option == "Move") {
            auto sel = co_await ctx.getSelection("Select objects to move to another level");
            if (!sel.ok() || sel.ids.empty())
                co_return;
            LevelId id;
            co_await askLevel(ctx, "Move to level", id, ok);
            if (!ok)
                co_return;
            auto n = moveToLevel(doc, ctx.tx(), sel.ids, id);
            if (n)
                ctx.message(std::to_string(n.value()) + " object(s) moved to " + describe(doc, id) +
                            ".");
            else
                ctx.message(n.error().message);
        } else if (option == "Delete") {
            LevelId id;
            co_await askLevel(ctx, "Level to delete (its objects move to the first level)", id, ok);
            if (!ok)
                co_return;
            std::string label = describe(doc, id);
            std::size_t n = entityCount(doc, id);
            auto r = deleteLevel(doc, ctx.tx(), id);
            if (r)
                ctx.message("Deleted level " + label + "; " + std::to_string(n) +
                            " object(s) moved to the first level.");
            else
                ctx.message(r.error().message);
        } else if (option == "List") {
            for (LevelId id : levelOrder(doc)) {
                const Level* l = doc.level(id);
                std::ostringstream line;
                line << (id == doc.header().currentLevel ? "* " : "  ") << describe(doc, id) << ", "
                     << entityCount(doc, id) << " object(s)" << (l->visible ? "" : ", hidden")
                     << (l->locked ? ", locked" : "");
                ctx.message(line.str());
            }
        }
    }
};

class ChangeLevelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CHLEVEL"; }
    std::vector<std::string> defaultAliases() const override { return {"CHLV"}; }
    std::string_view description() const override {
        return "Move the selected objects to another level";
    }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects to move to another level");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        bool ok = false;
        LevelId id;
        co_await askLevel(ctx, "Move to level", id, ok);
        if (!ok)
            co_return;
        auto n = moveToLevel(ctx.doc(), ctx.tx(), sel.ids, id);
        if (n)
            ctx.message(std::to_string(n.value()) + " object(s) moved to " +
                        describe(ctx.doc(), id) + ".");
        else
            ctx.message(n.error().message);
    }
};

// Switch the working level with the previous / next level by elevation.
class LevelStepCommand final : public cmd::Command {
public:
    LevelStepCommand(const char* name, const char* alias, int dir)
        : name_(name), alias_(alias), dir_(dir) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override {
        return dir_ > 0 ? "Work on the next level up" : "Work on the next level down";
    }
    Task run(CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto order = levelOrder(doc);
        auto it = std::find(order.begin(), order.end(), doc.header().currentLevel);
        if (it == order.end())
            co_return;
        auto idx = it - order.begin() + dir_;
        if (idx < 0 || idx >= static_cast<std::ptrdiff_t>(order.size())) {
            ctx.message(dir_ > 0 ? "Already on the top level." : "Already on the lowest level.");
            co_return;
        }
        report(ctx, setCurrentLevel(doc, ctx.tx(), order[static_cast<std::size_t>(idx)]));
        ctx.message("Current level: " + describe(doc, order[static_cast<std::size_t>(idx)]) + ".");
    }

private:
    const char* name_;
    const char* alias_;
    int dir_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeLevelPanelCommand() {
    return std::make_unique<LevelPanelCommand>();
}
std::unique_ptr<cmd::Command> makeLevelCommand() {
    return std::make_unique<LevelCommand>();
}
std::unique_ptr<cmd::Command> makeChangeLevelCommand() {
    return std::make_unique<ChangeLevelCommand>();
}
std::unique_ptr<cmd::Command> makeLevelUpCommand() {
    return std::make_unique<LevelStepCommand>("LEVELUP", "LVUP", +1);
}
std::unique_ptr<cmd::Command> makeLevelDownCommand() {
    return std::make_unique<LevelStepCommand>("LEVELDOWN", "LVDN", -1);
}

}  // namespace tbcad::levels
