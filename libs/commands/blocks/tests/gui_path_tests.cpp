#include <doctest/doctest.h>

#include <algorithm>
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
    Vec2 a = core.camera().worldToScreen({200, 600});
    Vec2 b = core.camera().worldToScreen({-200, -100});
    core.dragSelect(a, b, ui::PointerState{b, false, false});  // right to left: crossing
    core.submitText("");                                       // end of the selection
    core.submitText("0,0");                                    // base point
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

TEST_CASE(
    "a window around a whole associative array selects it and MATCHPROP recolours its items") {
    fs::path dir = fs::temp_directory_path() / "tbcad_gui_path_array";
    fs::remove_all(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1000, 800);
    render::Camera2D cam = core.camera();
    cam.center = {200, 100};
    cam.pixelsPerMm = 1.0;
    core.setCamera(cam);

    for (const char* s : {"CIRCLE", "20,20", "10"})
        core.submitText(s);
    for (const char* s : {"CIRCLE", "400,300", "10"})
        core.submitText(s);
    REQUIRE(core.document().modelSpaceCount() == 2);
    core.updateFrame();
    // Array the first circle with every default: 3 x 4, associative.
    core.setSelection({core.document().modelSpace()[0]});
    core.submitText("ARRAY");
    core.submitText("");
    core.submitText("");
    REQUIRE_FALSE(core.runner().active());
    EntityId array;
    for (EntityId id : core.document().modelSpace())
        if (const auto* ins = entityAs<InsertData>(*core.document().find(id)))
            if (ins->array)
                array = id;
    REQUIRE(array);
    core.clearSelection();
    core.updateFrame();

    const BBox2 b = blocks::insertBounds(core.document(), *core.document().find(array));
    Vec2 a = core.camera().worldToScreen({b.min.x - 5, b.min.y - 5});
    Vec2 c = core.camera().worldToScreen({b.max.x + 5, b.max.y + 5});
    core.dragSelect(a, c, ui::PointerState{c, false, false});  // left to right: window
    const auto sel = core.selection();
    CHECK(std::find(sel.begin(), sel.end(), array) != sel.end());
    CHECK(sel.size() == 1);

    // Match the properties of the far circle (colour 1) onto the array.
    core.clearSelection();
    {
        auto tx = core.document().begin("colour");
        for (EntityId id : core.document().modelSpace())
            if (entityAs<CircleData>(*core.document().find(id)))
                (void)tx.modify(id, [](Entity& e) { e.common.color = Color::index(1); });
        tx.commit();
    }
    EntityId far;
    for (EntityId id : core.document().modelSpace())
        if (entityAs<CircleData>(*core.document().find(id)))
            far = id;
    auto clickAt = [&](Vec2 w) {
        core.updateFrame();
        core.click({core.camera().worldToScreen(w), false, false});
    };
    core.submitText("MATCHPROP");
    clickAt({410, 300});
    clickAt({30, 20});  // the edge of the first item
    core.submitText("");
    const auto* ins = entityAs<InsertData>(*core.document().find(array));
    REQUIRE(ins);
    const BlockDef* def = core.document().block(ins->block);
    REQUIRE(def);
    REQUIRE_FALSE(def->entities.empty());
    for (EntityId id : def->entities)
        CHECK(core.document().find(id)->common.color == Color::index(1));
}
