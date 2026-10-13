#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/plugin/plugin_host.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    plugin::PluginHost plugins;
    std::unique_ptr<cmd::ScriptHost> host;
    Fixture() {
        registerAllCommands(reg);
        plugins.loadFolder(TBCAD_TEST_PLUGIN_DIR, reg);
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
    }
    std::size_t count() const {
        std::size_t n = 0;
        doc.forEachEntity([&](const Entity&) { ++n; });
        return n;
    }
};

}  // namespace

TEST_CASE("the sample plugin loads, describes itself and registers its commands") {
    Fixture f;
    REQUIRE(f.plugins.plugins().size() == 1);
    const auto& info = f.plugins.plugins()[0];
    CHECK(info.error.empty());
    CHECK(info.name == "Hello");
    CHECK(info.version == "1.0");
    CHECK(info.commands.size() == 2);
    CHECK(f.reg.find("HELLOLINE") != nullptr);
    CHECK(f.reg.find("helloGrid") != nullptr);
    bool logged = false;
    for (const std::string& l : f.plugins.takeLog())
        logged = logged || l.find("hello plugin ready") != std::string::npos;
    CHECK(logged);

    ui::UiRegistry ui;
    f.plugins.registerUi(ui);
    bool menu = false;
    for (const auto& item : ui.menuItems())
        menu = menu || (item.path == "Plugins/Hello line" && item.command == "HELLOLINE");
    CHECK(menu);
}

TEST_CASE("a plugin command prompts like a built-in one and is one undo step") {
    Fixture f;
    auto r = f.host->run("HELLOLINE", {"0,0", "10,5"});
    CHECK(r.finished);
    REQUIRE(f.count() == 1);
    f.doc.forEachEntity([&](const Entity& e) {
        const auto* line = entityAs<LineData>(e);
        REQUIRE(line);
        CHECK(distance(line->a, {0, 0}) < 1e-9);
        CHECK(distance(line->b, {10, 5}) < 1e-9);
    });
    CHECK(f.host->undoStack().undoLabel() == "HELLOLINE");
    CHECK(f.host->undo());
    CHECK(f.count() == 0);
}

TEST_CASE("Esc during a plugin prompt cancels the command and leaves the drawing alone") {
    Fixture f;
    auto r = f.host->run("HELLOLINE", {"0,0", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.count() == 0);
    CHECK_FALSE(f.host->undoStack().canUndo());
    r = f.host->run("HELLOLINE", {ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.count() == 0);
}

TEST_CASE("integer and real prompts reach the plugin and it can add many entities") {
    Fixture f;
    auto r = f.host->run("HELLOGRID", {"0,0", "3", "10"});
    CHECK(r.finished);
    CHECK(f.count() == 9);
    CHECK(f.host->undo());
    CHECK(f.count() == 0);
}

TEST_CASE("a file that is not a plugin is reported and skipped") {
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_plugin_test_bad";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / (std::string("bad") + (TBCAD_TEST_LIB_EXT)), std::ios::binary);
        out << "this is not a shared library";
    }
    cmd::Registry reg;
    plugin::PluginHost host;
    host.loadFolder(dir, reg);
    REQUIRE(host.plugins().size() == 1);
    CHECK_FALSE(host.plugins()[0].error.empty());
    CHECK_FALSE(host.takeLog().empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("an empty or missing folder loads nothing") {
    cmd::Registry reg;
    plugin::PluginHost host;
    host.loadFolder("/this/folder/does/not/exist", reg);
    CHECK(host.plugins().empty());
}
