#include <doctest/doctest.h>

#include "tbcad/hatch/hatch.h"
#include "tbcad/layout/layout.h"
#include "tbcad/text/text.h"
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
    CHECK(core.settings().getString("plot.layout", "?") == "Sheet 1");  // PLOT plots this sheet
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

    // Clicking an empty sheet selects nothing.
    core.click({core.camera().worldToScreen({100, 100}), false, false});
    CHECK(core.selection().empty());

    CHECK(core.setActiveLayout(""));
    CHECK_FALSE(core.inPaperSpace());
    CHECK(core.settings().getString("plot.layout", "?").empty());
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

TEST_CASE("a hatch on the sheet over a viewport does not stop a double click entering it") {
    hatch::registerHatchDisplay();
    text::registerDisplay();
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper5") / "cfg", tempDir("paper5") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 170}}, {5000, 3000}, 50).ok());
        tx.commit();
    }
    const BlockId sheet = layout::findLayout(core.document(), "A")->content;
    {
        // A solid fill drawn on the sheet across the whole viewport, and a note inside it.
        HatchData h;
        HatchLoop loop;
        HatchPolylineEdge pe;
        for (Vec2 q : {Vec2{10, 10}, Vec2{230, 10}, Vec2{230, 180}, Vec2{10, 180}})
            pe.v.push_back(PolyVertex{q});
        pe.closed = true;
        loop.edges.push_back(pe);
        h.loops.push_back(loop);
        TextData t;
        t.position = {150, 150};
        t.height = 5;
        t.utf8 = "NOTE";
        auto tx = core.document().begin("sheet objects");
        REQUIRE(tx.add(Entity{{}, h}, sheet).ok());
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        REQUIRE(tx.add(std::move(e), sheet).ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    core.zoomExtents();
    core.updateFrame();
    // On the note (its outline): the note keeps the double click (it opens its editor), the sheet stays.
    core.doubleClick({core.camera().worldToScreen({150, 152}), false, false});
    CHECK(core.inPaperSpace());
    core.pressEscape();
    core.pressEscape();
    // On the fill: the viewport is entered.
    core.doubleClick({core.camera().worldToScreen({100, 100}), false, false});
    CHECK_FALSE(core.inPaperSpace());
    CHECK(core.editingViewport() == 1);
}

TEST_CASE("MSPACE enters the model through a viewport and PSPACE returns to the sheet") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper4") / "cfg", tempDir("paper4") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 170}}, {5000, 3000}, 50).ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    core.submitText("MSPACE");
    CHECK_FALSE(core.inPaperSpace());
    CHECK(core.camera().center.x == doctest::Approx(5000).epsilon(0.01));
    core.submitText("PSPACE");
    CHECK(core.inPaperSpace());
}

TEST_CASE("zooming and panning inside a viewport changes its view, unless it is locked") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper_vz") / "cfg", tempDir("paper_vz") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 170}}, {5000, 3000}, 50).ok());
        tx.commit();
    }
    const auto vp = [&] { return layout::findLayout(core.document(), "A")->viewports.front(); };
    REQUIRE(core.setActiveLayout("A"));
    core.submitText("MSPACE");
    REQUIRE_FALSE(core.inPaperSpace());
    core.submitText("ZOOM");
    core.submitText("2X");
    render::Camera2D cam = core.camera();
    cam.center.x += 400;  // a pan of 400 model units
    core.setCamera(cam);
    core.setViewportSize(800, 800);  // a panel opening is not a pan
    core.submitText("PSPACE");
    core.updateFrame(std::chrono::steady_clock::now());
    REQUIRE(core.inPaperSpace());
    CHECK(vp().scaleN == doctest::Approx(25));
    CHECK(vp().center.x == doctest::Approx(5400));
    CHECK(vp().center.y == doctest::Approx(3000));
    // One undo step puts the view back.
    CHECK(core.undo());
    CHECK(vp().scaleN == doctest::Approx(50));
    CHECK(vp().center.x == doctest::Approx(5000));

    // Entering and leaving without touching the view changes nothing.
    const std::size_t steps = core.undoStack().undoCount();
    core.submitText("MSPACE");
    core.submitText("PSPACE");
    core.updateFrame(std::chrono::steady_clock::now());
    CHECK(core.undoStack().undoCount() == steps);

    // A locked viewport keeps its view.
    {
        auto tx = core.document().begin("lock");
        REQUIRE(layout::setViewportLocked(tx, "A", vp().id, true).ok());
        tx.commit();
    }
    core.submitText("MSPACE");
    core.submitText("ZOOM");
    core.submitText("2X");
    core.submitText("PSPACE");
    core.updateFrame(std::chrono::steady_clock::now());
    CHECK(vp().scaleN == doctest::Approx(50));
}

TEST_CASE("ZOOM nXP sets the viewport scale, from the sheet and from model space") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper_xp") / "cfg", tempDir("paper_xp") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        REQUIRE(layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 170}}, {5000, 3000}, 50).ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    // On the sheet the only viewport is the target; 1/100XP makes it 1:100.
    core.submitText("ZOOM");
    core.submitText("1/100XP");
    core.updateFrame(std::chrono::steady_clock::now());
    REQUIRE(layout::findLayout(core.document(), "A")->viewports.front().scaleN ==
            doctest::Approx(100));
    // From model space the view follows the new scale: 200 mm at 1:200 is 40 m across.
    core.submitText("MSPACE");
    core.submitText("ZOOM");
    core.submitText("0.005XP");
    core.updateFrame(std::chrono::steady_clock::now());
    CHECK(layout::findLayout(core.document(), "A")->viewports.front().scaleN ==
          doctest::Approx(200));
    CHECK(core.camera().visibleWorld().max.x - core.camera().visibleWorld().min.x >= 40000);
    // nX is relative to the current view and changes no viewport.
    const double before = core.camera().pixelsPerMm;
    core.submitText("ZOOM");
    core.submitText("2X");
    CHECK(core.camera().pixelsPerMm == doctest::Approx(before * 2));
    CHECK(layout::findLayout(core.document(), "A")->viewports.front().scaleN ==
          doctest::Approx(200));
}

TEST_CASE("objects can be drawn on, picked and erased from a layout sheet") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper4") / "cfg", tempDir("paper4") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    core.zoomExtents();
    core.submitText("TESTLINE");
    core.submitText("10,10");
    core.submitText("200,10");
    // The line is on the sheet, not in model space.
    CHECK(core.document().modelSpace().empty());
    const auto lay = layout::findLayout(core.document(), "A");
    REQUIRE(lay);
    const BlockDef* sheet = core.document().block(lay->content);
    REQUIRE(sheet);
    REQUIRE(sheet->entities.size() == 1);
    core.updateFrame();

    // Clicking it selects it; ERASE removes it.
    core.click({core.camera().worldToScreen({100, 10}), false, false});
    CHECK(core.selection().size() == 1);
    core.submitText("TESTERASE");
    core.updateFrame();
    CHECK(core.document().block(lay->content)->entities.empty());

    // Back in model space nothing of that is left over.
    CHECK(core.setActiveLayout(""));
    CHECK(core.selection().empty());
    core.submitText("TESTLINE");
    core.submitText("0,0");
    core.submitText("50,0");
    CHECK(core.document().modelSpace().size() == 1);
}

TEST_CASE("a layout asked for by a command is shown once the command is over") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper6") / "cfg", tempDir("paper6") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "Plan", "A3").ok());
        tx.commit();
    }
    CHECK_FALSE(core.inPaperSpace());
    core.setSetting("layout.show", "Plan");
    core.updateFrame(std::chrono::steady_clock::now());
    CHECK(core.inPaperSpace());
    CHECK(core.settings().getString("plot.layout", "?") == "Plan");
}

TEST_CASE("ZOOM Extents and ZOOM All frame the sheet while a layout is active") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("paper5") / "cfg", tempDir("paper5") / "data"});
    core.setViewportSize(1000, 800);
    {
        auto tx = core.document().begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        tx.commit();
    }
    REQUIRE(core.setActiveLayout("A"));
    for (const char* kw : {"E", "A"}) {
        core.setCamera([&] {
            render::Camera2D c = core.camera();
            c.pixelsPerMm *= 8;
            return c;
        }());
        core.submitText("ZOOM");
        core.submitText(kw);
        CHECK(core.camera().screenToWorld({0, 0}).x <= 1.0);
        CHECK(core.camera().pixelsPerMm < 5);
    }
}
