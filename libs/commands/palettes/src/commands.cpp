#include "commands.h"

#include <memory>

#include "tbcad/palettes/palettes.h"

namespace tbcad::palettes {

namespace {

std::filesystem::path storeFile(cmd::CommandContext& ctx) {
    auto folder = ctx.host().settingsFolder();
    return folder.empty() ? std::filesystem::path{} : folder / "palettes.json";
}

class PalettesPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PALETTES"; }
    std::vector<std::string> defaultAliases() const override { return {"TP", "TOOLPALETTES"}; }
    std::string_view description() const override {
        return "Open the tool palettes (one-click blocks, hatches and commands)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("palettes");
        co_return;
    }
};

class PaletteCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-PALETTE"; }
    std::string_view description() const override {
        return "Create, rename, delete and fill tool palettes from the command line";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const auto file = storeFile(ctx);
        if (file.empty()) {
            ctx.message("Palettes need a settings folder.");
            co_return;
        }
        PaletteStore store(file);
        store.load();
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"List",     "New",      "Rename", "Delete", "AddCommand",
                           "AddBlock", "AddHatch", "Remove", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Palette option", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            if (k.keyword == "List") {
                for (const Palette& p : store.palettes()) {
                    ctx.message(p.name + ":");
                    for (const Tool& t : p.tools)
                        ctx.message("  " + t.name);
                }
                continue;
            }
            auto ask = [&](const std::string& prompt, std::string& out) -> cmd::Task {
                cmd::StringOptions so;
                auto r = co_await ctx.getString(prompt, so);
                out = r.ok() ? cmd::trim(r.text) : std::string();
                co_return;
            };
            std::string palette;
            co_await ask("Palette name", palette);
            if (palette.empty())
                continue;
            std::string err;
            if (k.keyword == "New") {
                err = store.addPalette(palette);
            } else if (k.keyword == "Delete") {
                err = store.removePalette(palette);
            } else if (k.keyword == "Rename") {
                std::string to;
                co_await ask("New name", to);
                err = store.renamePalette(palette, to);
            } else if (k.keyword == "Remove") {
                std::string tool;
                co_await ask("Tool name", tool);
                err = store.removeTool(palette, tool);
            } else {
                Tool t;
                co_await ask("Tool name", t.name);
                if (k.keyword == "AddCommand") {
                    t.kind = ToolKind::Command;
                    co_await ask("Command line (several lines separated by |)", t.command);
                } else if (k.keyword == "AddBlock") {
                    t.kind = ToolKind::Block;
                    co_await ask("Block name", t.block);
                    std::string scale;
                    co_await ask("Scale <1>", scale);
                    if (!scale.empty())
                        t.scale = std::atof(scale.c_str());
                } else {
                    t.kind = ToolKind::Hatch;
                    co_await ask("Pattern name", t.pattern);
                    std::string scale;
                    co_await ask("Pattern scale <1>", scale);
                    if (!scale.empty())
                        t.scale = std::atof(scale.c_str());
                }
                if (t.scale <= 0)
                    t.scale = 1;
                err = store.addTool(palette, std::move(t));
            }
            if (!err.empty()) {
                ctx.message(err);
                continue;
            }
            if (std::string saved = store.save(); !saved.empty())
                ctx.message(saved);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePalettesPanelCommand() {
    return std::make_unique<PalettesPanelCommand>();
}

std::unique_ptr<cmd::Command> makePaletteCommand() {
    return std::make_unique<PaletteCommand>();
}

}  // namespace tbcad::palettes
