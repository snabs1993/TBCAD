// Runs the command behind every menu entry in a headless AppCore, in three situations (empty
// drawing, drawing with a selection, command already running), answers its prompts with a few
// typical inputs and cancels. Nothing may crash or hang; each command must end up cancelled.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

struct Rig {
    fs::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;

    explicit Rig(const std::string& name) {
        dir = fs::temp_directory_path() / ("tbcad_menu_smoke_" + name);
        fs::remove_all(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->applyUiKeybindings(uiRegistry);
        core->setViewportSize(1000, 800);
    }
    ~Rig() {
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void draw() {
        for (const char* line : {"LINE 0,0 100,0 100,100 0,100 c", "CIRCLE 50,50 30",
                                 "LINE 0,0 100,100", "RECTANGLE 10,10 40,40"}) {
            core->submitText(line);
            core->pressEscape();
        }
        core->selectAll();
    }
    // Feeds a handful of plausible answers, then cancels whatever is left.
    void exercise(const std::string& command) {
        core->submitText(command);
        for (const char* answer : {"10,10", "50,0", "", "20,20", "0", "5", "", ""}) {
            if (!core->runner().active())
                break;
            core->submitText(answer);
        }
        for (int i = 0; i < 4 && core->runner().active(); ++i)
            core->pressEscape();
        core->pressEscape();
    }
};

std::vector<std::string> menuCommands() {
    Rig rig("list");
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto& item : rig.uiRegistry.menuItems())
        if (seen.insert(item.command).second)
            out.push_back(item.command);
    return out;
}

}  // namespace

TEST_CASE("every menu command survives an empty drawing") {
    const auto commands = menuCommands();
    REQUIRE(commands.size() > 40);
    for (const auto& command : commands) {
        INFO("command: " << command);
        std::fprintf(stderr, "[empty] %s\n", command.c_str());
        Rig rig("empty");
        rig.exercise(command);
        CHECK_FALSE(rig.core->runner().active());
    }
}

TEST_CASE("every menu command survives a drawing with a selection") {
    for (const auto& command : menuCommands()) {
        INFO("command: " << command);
        std::fprintf(stderr, "[selection] %s\n", command.c_str());
        Rig rig("selection");
        rig.draw();
        rig.exercise(command);
        CHECK_FALSE(rig.core->runner().active());
        // Whatever it did must be undoable without trouble.
        for (int i = 0; i < 5; ++i)
            rig.core->undo();
    }
}

TEST_CASE("every menu command can start while another command is running") {
    for (const auto& command : menuCommands()) {
        INFO("command: " << command);
        std::fprintf(stderr, "[busy] %s\n", command.c_str());
        Rig rig("busy");
        rig.draw();
        rig.core->submitText("LINE");
        rig.exercise(command);
        for (int i = 0; i < 4 && rig.core->runner().active(); ++i)
            rig.core->pressEscape();
        CHECK_FALSE(rig.core->runner().active());
    }
}
