#include <doctest/doctest.h>

#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;
namespace fs = std::filesystem;

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

TEST_CASE("an xref whose file is gone is listed as missing when the drawing opens") {
    auto registry = makeRegistry();
    const fs::path dir = tempDir("xrefmissing");
    AppCore core(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1000, 800);
    CHECK_FALSE(core.xrefNoticeVisible());
    {
        auto tx = core.document().begin("xref");
        BlockDef def;
        def.name = "site";
        def.xrefPath = "gone/site.dxf";
        REQUIRE(tx.addBlock(std::move(def)).ok());
        tx.commit();
    }
    REQUIRE(core.saveAs(dir / "host.tbcad").ok());
    core.newDocument();
    REQUIRE(core.openFile(dir / "host.tbcad").ok());
    REQUIRE(core.missingXrefs().size() == 1);
    CHECK(core.missingXrefs()[0].name == "site");
    CHECK(core.missingXrefs()[0].path == "gone/site.dxf");
    CHECK(core.xrefNoticeVisible());
    core.dismissXrefNotice();
    CHECK_FALSE(core.xrefNoticeVisible());
}

TEST_CASE("double click in a viewport goes to model space showing that viewport") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper3") / "cfg", tempDir("paper3") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 170}}, {5000, 3000}, 50).ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    core.zoomExtents();
    // Outside every viewport: stays on the sheet.
    core.doubleClick({core.camera().worldToScreen({300, 250}), false, false});
    CHECK(core.inPaperSpace());
    core.doubleClick({core.camera().worldToScreen({100, 100}), false, false});
    CHECK_FALSE(core.inPaperSpace());
    CHECK(core.camera().center.x == doctest::Approx(5000).epsilon(0.01));
    CHECK(core.camera().center.y == doctest::Approx(3000).epsilon(0.01));
    // 200 mm of sheet at 1:50 is 10 m of model across, so the view shows about that much.
    CHECK(core.camera().visibleWorld().max.x - core.camera().visibleWorld().min.x >= 10000);
}
