#include <doctest/doctest.h>

#include "tbcad/core/groups.h"
#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;
namespace fs = std::filesystem;

namespace {

// Three horizontal lines at y = 0, 20 and 40 (x 0..10) and a long one at y = 60 (x -50..50).
struct SelFixture {
    fs::path dir;
    std::unique_ptr<cmd::Registry> registry = makeRegistry();
    std::unique_ptr<AppCore> core;
    explicit SelFixture(const std::string& name) : dir(tempDir(name)) {
        core = std::make_unique<AppCore>(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1000, 800);
        render::Camera2D cam = core->camera();
        cam.center = {0, 30};
        cam.pixelsPerMm = 4.0;
        core->setCamera(cam);
        for (double y : {0.0, 20.0, 40.0})
            addLine(core->document(), {0, y}, {10, y});
        addLine(core->document(), {-50, 60}, {50, 60});
    }
    PointerState at(Vec2 world, bool shift = false) {
        return PointerState{core->camera().worldToScreen(world), shift, false};
    }
    std::size_t count() { return core->document().modelSpaceCount(); }
    bool lastLineHas(const std::string& s) {
        for (const std::string& h : core->history())
            if (h.find(s) != std::string::npos)
                return true;
        return false;
    }
};

}  // namespace

TEST_CASE("typed Window at Select objects takes only what lies inside") {
    SelFixture f("selwin");
    f.core->submitText("TESTERASE");
    f.core->submitText("W");
    CHECK(f.core->runner().selectShapePending());
    f.core->submitText("-5,15");
    f.core->submitText("12,45");  // lines at 20 and 40 lie inside
    CHECK_FALSE(f.core->runner().selectShapePending());
    CHECK(f.core->runner().collected().size() == 2);
    f.core->submitText("");
    CHECK(f.count() == 2);
}

TEST_CASE("typed Crossing takes what the box touches, and corners can be clicked") {
    SelFixture f("selcross");
    f.core->submitText("TESTERASE");
    f.core->submitText("C");
    f.core->click(f.at({-1, 50}));
    f.core->click(f.at({1, 70}));  // touches only the long line at y = 60
    CHECK(f.core->runner().collected().size() == 1);
    f.core->submitText("");
    CHECK(f.count() == 3);
}

TEST_CASE("WPolygon and CPolygon finish on Enter, a fence takes what it crosses") {
    SelFixture f("selpoly");
    f.core->submitText("TESTERASE");
    f.core->submitText("WP");
    for (const char* p : {"-5,-5", "12,-5", "12,25", "-5,25"})
        f.core->submitText(p);
    CHECK(f.core->runner().selectShapePending());
    f.core->submitText("");
    CHECK(f.core->runner().collected().size() == 2);  // y = 0 and 20 lie inside
    f.core->submitText("CP");
    for (const char* p : {"5,35", "5,70", "8,70"})
        f.core->submitText(p);
    f.core->submitText("");
    CHECK(f.core->runner().collected().size() == 4);  // + y = 40 and the long line
    f.core->submitText("");
    CHECK(f.count() == 0);

    SelFixture g("selfence");
    g.core->submitText("TESTERASE");
    g.core->submitText("F");
    g.core->submitText("5,-10");
    g.core->submitText("5,30");  // crosses y = 0 and y = 20
    g.core->submitText("");
    CHECK(g.core->runner().collected().size() == 2);
    g.core->submitText("");
    CHECK(g.count() == 2);
}

TEST_CASE("Enter with too few points cancels the shape, not the command") {
    SelFixture f("selshort");
    f.core->submitText("TESTERASE");
    f.core->submitText("F");
    f.core->submitText("5,0");
    f.core->submitText("");
    CHECK(f.core->runner().active());
    CHECK_FALSE(f.core->runner().selectShapePending());
    CHECK(f.core->runner().collected().empty());
}

TEST_CASE("Remove, Add and Undo at Select objects") {
    SelFixture f("selremove");
    f.core->submitText("TESTERASE");
    f.core->submitText("ALL");
    CHECK(f.core->runner().collected().size() == 4);
    f.core->submitText("R");
    f.core->submitText("W");
    f.core->submitText("-5,-5");
    f.core->submitText("12,25");
    CHECK(f.core->runner().collected().size() == 2);
    f.core->submitText("U");  // back to all four
    CHECK(f.core->runner().collected().size() == 4);
    f.core->submitText("A");  // Add mode again
    f.core->submitText("P");  // nothing was selected before: no objects
    CHECK(f.core->runner().collected().size() == 4);
    f.core->submitText("");
    CHECK(f.count() == 0);
}

TEST_CASE("Shift-click and Shift-window remove from the selection being built") {
    SelFixture f("selshift");
    f.core->submitText("TESTERASE");
    f.core->click(f.at({5, 0}));
    f.core->click(f.at({5, 20}));
    f.core->click(f.at({5, 40}));
    CHECK(f.core->runner().collected().size() == 3);
    f.core->click(f.at({5, 20}, true));
    CHECK(f.core->runner().collected().size() == 2);
    Vec2 a = f.core->camera().worldToScreen({-5, 35});
    Vec2 b = f.core->camera().worldToScreen({15, 45});
    f.core->dragSelect(a, b, PointerState{b, true, false});
    CHECK(f.core->runner().collected().size() == 1);
    f.core->submitText("");
    CHECK(f.count() == 3);
}

TEST_CASE("Last and Previous at Select objects") {
    SelFixture f("sellast");
    f.core->submitText("TESTERASE");
    f.core->submitText("L");
    f.core->submitText("");
    CHECK(f.count() == 3);
    f.core->submitText("TESTERASE");
    f.core->submitText("P");  // the long line again, already gone: nothing
    CHECK(f.core->runner().collected().empty());
    f.core->submitText("");
    CHECK_FALSE(f.core->runner().active());
}

TEST_CASE("dragging right selects a window, dragging left a crossing") {
    SelFixture f("seldrag");
    Vec2 a = f.core->camera().worldToScreen({-5, 15});
    Vec2 b = f.core->camera().worldToScreen({5, 45});
    f.core->dragSelect(a, b, f.at({5, 45}));  // left to right: window, half-covered lines stay out
    CHECK(f.core->selection().empty());
    f.core->dragSelect(b, a, f.at({-5, 15}));  // right to left: crossing
    CHECK(f.core->selection().size() == 2);
}

TEST_CASE("PICKADD 0: a pick replaces the selection and Shift toggles") {
    SelFixture f("selpickadd");
    f.core->submitText("PICKADD 0");
    f.core->click(f.at({5, 0}));
    f.core->click(f.at({5, 20}));
    CHECK(f.core->selection().size() == 1);
    f.core->click(f.at({5, 40}, true));
    CHECK(f.core->selection().size() == 2);
    f.core->click(f.at({5, 40}, true));
    CHECK(f.core->selection().size() == 1);
    f.core->submitText("PICKADD 2");
    f.core->click(f.at({5, 0}));
    CHECK(f.core->selection().size() == 2);
}

TEST_CASE("PICKFIRST 0: a command ignores the objects selected before it") {
    SelFixture f("selpickfirst");
    f.core->click(f.at({5, 0}));
    f.core->submitText("PICKFIRST 0");
    f.core->submitText("TESTERASE");
    CHECK(f.core->runner().active());  // still asking
    f.core->submitText("");
    CHECK(f.count() == 4);
    f.core->submitText("PICKFIRST 1");
    f.core->submitText("TESTERASE");
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.count() == 3);
}

TEST_CASE("PICKDRAG 0: two clicks make a selection window") {
    SelFixture f("selpickdrag");
    CHECK(f.core->dragPicksWindow());
    CHECK_FALSE(f.core->windowClick(f.at({-20, -10})));
    f.core->submitText("PICKDRAG 0");
    CHECK_FALSE(f.core->dragPicksWindow());
    CHECK(f.core->windowClick(f.at({-5, -5})));  // first corner, empty space
    CHECK(f.core->selection().empty());
    CHECK(f.core->windowClick(f.at({15, 25})));  // second corner: lines at y=0 and y=20
    CHECK(f.core->selection().size() == 2);
    CHECK_FALSE(f.core->windowClick(f.at({5, 0})));  // on an object: a normal click
    f.core->submitText("PICKDRAG 1");
    CHECK_FALSE(f.core->windowClick(f.at({-5, -5})));
}

TEST_CASE("an unknown word at Select objects names the options") {
    SelFixture f("selbad");
    f.core->submitText("TESTERASE");
    f.core->submitText("BANANA");
    CHECK(f.lastLineHas("Fence"));
    CHECK(f.core->runner().active());
}

#include <chrono>
#include <cstdio>

TEST_CASE("perf: a crossing window at a Select objects prompt on 393k lines") {
    auto registry = makeRegistry();
    auto dir = tempDir("selcross393k");
    AppCore core(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    const int n = 393000;
    {
        auto tx = core.document().begin("gen");
        for (int i = 0; i < n; ++i)
            (void)tx.add(Entity{{},
                                LineData{{(i % 700) * 30.0, (i / 700) * 30.0},
                                         {(i % 700) * 30.0 + 20, (i / 700) * 30.0 + 10}}});
        tx.commit();
    }
    core.zoomExtents();
    core.updateFrame();
    auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
            .count();
    };
    core.submitText("TESTERASE");
    core.submitText("C");
    core.submitText("-10,-10");
    auto t = std::chrono::steady_clock::now();
    core.submitText("21000,17000");  // everything
    const double crossing = ms(t);
    CHECK(core.runner().collected().size() == static_cast<std::size_t>(n));
    core.submitText("R");
    core.submitText("W");
    core.submitText("-10,-10");
    t = std::chrono::steady_clock::now();
    core.submitText("10500,17000");  // about half of it leaves again
    const double removeWin = ms(t);
    CHECK(core.runner().collected().size() < static_cast<std::size_t>(n) / 2 + 3000);
    std::printf("[select perf] 393k crossing at prompt %.0f ms, remove window %.0f ms\n", crossing,
                removeWin);
#ifdef NDEBUG
    CHECK(crossing < 500);
#endif
}

TEST_CASE("Group, BOX, SIngle and ? at Select objects") {
    SelFixture f("selgroup");
    {
        // The lines at 0 and 20 (the first two added).
        std::vector<EntityId> ids;
        f.core->document().forEachEntity([&](const Entity& e) {
            if (ids.size() < 2)
                ids.push_back(e.common.id);
        });
        Transaction tx = f.core->document().begin("group");
        REQUIRE(writeGroups(tx, {Group{.name = "Pair", .members = ids}}).ok());
        tx.commit();
    }

    f.core->submitText("TESTERASE");
    f.core->submitText("G");
    f.core->submitText("nosuch");
    CHECK(f.lastLineHas("There is no group named"));
    f.core->submitText("G");
    f.core->submitText("pair");  // names are not case sensitive
    CHECK(f.core->runner().collected().size() == 2);
    f.core->submitText("?");
    CHECK(f.lastLineHas("Window/Last/Crossing/BOX"));
    f.core->submitText("");
    CHECK(f.count() == 2);

    // BOX from right to left is a crossing: the half-covered line at 40 is taken.
    f.core->submitText("TESTERASE");
    f.core->submitText("BOX");
    f.core->submitText("5,45");
    f.core->submitText("-5,35");
    CHECK(f.core->runner().collected().size() == 1);
    f.core->pressEscape();

    // SIngle: the first pick ends the prompt.
    f.core->submitText("TESTERASE");
    f.core->submitText("SI");
    f.core->click(f.at({5, 40}));
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.count() == 1);
}

TEST_CASE("the selection cycling list: the objects under the cursor, and picking one of them") {
    SelFixture f("selcyclist");
    addLine(f.core->document(), {0, 0}, {10, 0.2});  // crosses the line at y = 0
    const auto under = f.core->objectsAt(f.at({5, 0.05}));
    REQUIRE(under.size() == 2);
    f.core->pickFromList(under[1], f.at({5, 0.05}));
    REQUIRE(f.core->selection().size() == 1);
    CHECK(f.core->selection()[0] == under[1]);
    // While a command waits for a point the list is empty: a click is a point there.
    f.core->submitText("TESTLINE");
    REQUIRE(f.core->runner().active());
    CHECK(f.core->objectsAt(f.at({5, 0.05})).empty());
    f.core->pressEscape();
}
