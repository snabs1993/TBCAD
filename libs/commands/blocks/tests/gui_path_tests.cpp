#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layout/layout.h"
#include "tbcad/platform/paths.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
namespace fs = std::filesystem;

TEST_CASE("attributes made with -ATTDEF and caught by a crossing window both reach the block") {
    fs::path dir = fs::temp_directory_path() / "tbcad_gui_path_attdef";
    fs::remove_all(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1000, 800);
    render::Camera2D cam = core.camera();
    cam.center = {0, 250};
    cam.pixelsPerMm = 1.0;
    core.setCamera(cam);

    for (const char* s : {"-ATTDEF", "", "NAME", "Name", "Anon", "0,500", "2", "0"})
        core.submitText(s);
    for (const char* s : {"-ATTDEF", "", "AREA", "Area m2", "0", "0,0", "2", "0"})
        core.submitText(s);
    REQUIRE(core.document().modelSpaceCount() == 2);

    core.submitText("BLOCK");
    core.submitText("RT4");
    core.submitText("0,0");
    Vec2 a = core.camera().worldToScreen({200, 600});
    Vec2 b = core.camera().worldToScreen({-200, -100});
    core.dragSelect(a, b, ui::PointerState{b, false, false});  // right to left: crossing
    core.submitText("");                                       // end of the selection
    core.submitText("");                                       // Convert
    const BlockDef* def = core.document().blockByName("RT4");
    REQUIRE(def);
    CHECK(def->entities.size() == 2);
}

TEST_CASE("VPCLIP takes its outline points from mouse clicks on the sheet") {
    fs::path dir = fs::temp_directory_path() / "tbcad_gui_path_vpclip";
    fs::remove_all(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", {{20, 20}, {220, 170}}, {0, 0}, 1).ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    core.zoomExtents();
    core.updateFrame();
    core.submitText("VPCLIP");
    if (core.runner().active() &&
        core.runner().prompt()->text.find("viewport") != std::string::npos)
        core.submitText("1");
    core.submitText("Polygonal");
    REQUIRE(core.runner().active());
    auto clickAt = [&](Vec2 w) {
        core.updateFrame();
        core.click({core.camera().worldToScreen(w), false, false});
    };
    clickAt({30, 30});
    REQUIRE(core.runner().active());
    CHECK(core.runner().prompt()->text.find("point 2") != std::string::npos);
    clickAt({200, 40});
    CHECK(core.runner().prompt()->text.find("point 3") != std::string::npos);
    clickAt({100, 160});
    REQUIRE(core.runner().active());
    CHECK(core.runner().prompt()->text.find("Close") != std::string::npos);
    core.submitText("");
    const auto lay = layout::findLayout(core.document(), "A");
    REQUIRE(lay);
    REQUIRE(lay->findViewport(1));
    CHECK(lay->findViewport(1)->clip.size() == 3);
}
