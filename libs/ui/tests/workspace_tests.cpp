#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>

#include "tbcad/ui/workspace.h"
#include "test_support.h"

using namespace tbcad::ui;
namespace fs = std::filesystem;

TEST_CASE("built-in workspaces are listed first and cannot be changed") {
    WorkspaceStore store(tbcad::ui::test::tempDir("ws-builtin") / "workspaces");
    auto names = store.names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Drafting");
    CHECK(store.isBuiltin("minimal"));
    CHECK_FALSE(store.save(Workspace{"Drafting"}).empty());
    CHECK_FALSE(store.remove("Minimal").empty());
    CHECK_FALSE(store.rename("Annotation", "X").empty());
    auto min = store.load("minimal");
    REQUIRE(min);
    CHECK_FALSE(min->properties);
    CHECK(min->builtin);
}

TEST_CASE("user workspaces save, load, rename and delete") {
    WorkspaceStore store(tbcad::ui::test::tempDir("ws-user") / "workspaces");
    Workspace w;
    w.name = "Site plan: day/night";
    w.panels = {{"layers", true}, {"hatch", false}};
    w.commandLine = false;
    w.dockIni = "[Window][Layers]\nPos=1,2\n";
    REQUIRE(store.save(w).empty());
    auto names = store.names();
    REQUIRE(names.size() == 4);
    CHECK(names.back() == "Site plan: day/night");

    auto got = store.load("Site plan: day/night");
    REQUIRE(got);
    CHECK(got->panels.at("layers"));
    CHECK_FALSE(got->panels.at("hatch"));
    CHECK_FALSE(got->commandLine);
    CHECK(got->properties);
    CHECK(got->dockIni == w.dockIni);
    CHECK_FALSE(got->builtin);

    CHECK(store.rename("Site plan: day/night", "Plan B").empty());
    CHECK_FALSE(store.load("Site plan: day/night"));
    CHECK(store.load("Plan B"));
    CHECK_FALSE(store.rename("Plan B", "Drafting").empty());
    CHECK_FALSE(store.rename("Nope", "Z").empty());

    CHECK(store.remove("Plan B").empty());
    CHECK_FALSE(store.remove("Plan B").empty());
    CHECK(store.names().size() == 3);
}

TEST_CASE("workspaces: empty names, odd characters and damaged files") {
    fs::path dir = tbcad::ui::test::tempDir("ws-odd") / "workspaces";
    WorkspaceStore store(dir);
    CHECK_FALSE(store.save(Workspace{"   "}).empty());
    CHECK(store.save(Workspace{"แบบบ้าน"}).empty());
    CHECK(store.load("แบบบ้าน"));
    CHECK(WorkspaceStore::fileStem("a/b:c") == "a_b_c");
    { std::ofstream(dir / "broken.json") << "{ nope"; }
    auto names = store.names();  // the broken file is listed by its file name but loads as nothing
    CHECK_FALSE(store.load("broken"));
    CHECK(std::find(names.begin(), names.end(), "แบบบ้าน") != names.end());
}

#include "tbcad/ui/app_core.h"

TEST_CASE("WORKSPACE command lists, renames, deletes and hands load/save to the window") {
    fs::path dir = tbcad::ui::test::tempDir("ws-cmd");
    auto reg = tbcad::ui::test::makeRegistry();
    AppCore core(*reg, tbcad::platform::AppPaths{dir / "cfg", dir / "data"});
    WorkspaceStore store(dir / "cfg" / "workspaces");

    core.submitText("WORKSPACE");
    core.submitText("List");
    CHECK(core.history().back().find("Drafting, Annotation, Minimal") != std::string::npos);

    core.submitText("WS");
    core.submitText("Save");
    core.submitText("Mine");
    auto reqs = core.takeUiRequests();
    REQUIRE(reqs.size() == 1);
    CHECK(reqs[0] == "workspace:save:Mine");  // the window writes it, it owns the layout

    Workspace w;
    w.name = "Mine";
    REQUIRE(store.save(w).empty());
    core.submitText("WS");
    core.submitText("Load");
    core.submitText("Mine");
    reqs = core.takeUiRequests();
    REQUIRE(reqs.size() == 1);
    CHECK(reqs[0] == "workspace:load:Mine");

    core.submitText("WS");
    core.submitText("Load");
    core.submitText("Nope");
    CHECK(core.takeUiRequests().empty());
    CHECK(core.history().back().find("No workspace") != std::string::npos);

    core.submitText("WS");
    core.submitText("Save");
    core.submitText("Minimal");
    CHECK(core.takeUiRequests().empty());
    CHECK(core.history().back().find("built in") != std::string::npos);

    core.submitText("WS");
    core.submitText("Rename");
    core.submitText("Mine");
    core.submitText("Yours");
    CHECK(store.load("Yours"));
    core.submitText("WS");
    core.submitText("Delete");
    core.submitText("Yours");
    CHECK_FALSE(store.load("Yours"));
}
