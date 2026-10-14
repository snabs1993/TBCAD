#include <doctest/doctest.h>

#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

TEST_CASE("switching to a layout shows the sheet and goes back to the model view") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper") / "cfg", tempDir("paper") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "Sheet 1", "A3").ok());
        tx.commit();
    }
    CHECK(core.layoutNames() == std::vector<std::string>{"Sheet 1"});
    const render::Camera2D model = core.camera();

    CHECK(core.setActiveLayout("Sheet 1"));
    CHECK(core.inPaperSpace());
    CHECK(&core.renderScene() != &core.scene());
    CHECK(core.renderScene().entityCount() > 0);  // at least the white sheet

    // Zoom Extents shows the whole sheet.
    core.setCamera([&] {
        render::Camera2D c = core.camera();
        c.pixelsPerMm *= 8;
        return c;
    }());
    core.zoomExtents();
    CHECK(core.camera().screenToWorld({0, 0}).x <= 1.0);  // the sheet starts at x = 0
    CHECK(core.camera().pixelsPerMm < 5);

    // Clicks do nothing in a read-only layout view.
    core.click({core.camera().worldToScreen({100, 100}), false, false});
    CHECK(core.selection().empty());

    CHECK(core.setActiveLayout(""));
    CHECK_FALSE(core.inPaperSpace());
    CHECK(core.camera().center.x == doctest::Approx(model.center.x));
    CHECK(core.camera().pixelsPerMm == doctest::Approx(model.pixelsPerMm));

    CHECK_FALSE(core.setActiveLayout("No such sheet"));
    CHECK_FALSE(core.inPaperSpace());
}

TEST_CASE("deleting the shown layout returns to the model") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper2") / "cfg", tempDir("paper2") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A4").ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    {
        auto tx = core.document().begin("delete");
        REQUIRE(layout::deleteLayout(tx, "A").ok());
        tx.commit();
    }
    core.updateFrame();
    CHECK_FALSE(core.inPaperSpace());
}
