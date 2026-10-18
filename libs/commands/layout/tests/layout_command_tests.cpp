#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/layout/layout.h"
#include "tbcad/layoutcmd/layoutcmd.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    std::unique_ptr<cmd::ScriptHost> host;
    Fixture() {
        layoutcmd::registerCommands(reg);
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
    }
};

}  // namespace

TEST_CASE("LAYOUT New creates a layout, makes it current, and undo removes it") {
    Fixture f;
    auto r = f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    CHECK(r.finished);
    auto l = layout::findLayout(f.doc, "Plan");
    REQUIRE(l.has_value());
    CHECK(l->widthMm == doctest::Approx(420));
    CHECK(layoutcmd::currentLayout(*f.host, f.doc) == "Plan");
    CHECK(f.host->setting("layout.show") == std::optional<std::string>("Plan"));  // window follows
    CHECK(f.host->undoStack().undoLabel() == "LAYOUT");
    CHECK(f.host->undo());
    CHECK(layout::layouts(f.doc).empty());
}

TEST_CASE("MVIEW, VPSCALE and VPLAYER edit the current layout") {
    Fixture f;
    LayerId walls;
    {
        auto tx = f.doc.begin("layer");
        Layer l;
        l.name = "Walls";
        walls = tx.addLayer(l).value();
        tx.commit();
    }
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    auto mv = f.host->run("MVIEW", {"10,10", "410,287", "1:50", "2500,1000"});
    CHECK(mv.finished);
    auto l = layout::findLayout(f.doc, "Plan");
    REQUIRE(l->viewports.size() == 1);
    CHECK(l->viewports[0].scaleN == doctest::Approx(50));
    CHECK(l->viewports[0].center.x == doctest::Approx(2500));
    CHECK(l->viewports[0].paper.width() == doctest::Approx(400));

    f.host->run("VPSCALE", {"1", "1:100"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].scaleN == doctest::Approx(100));
    f.host->run("VPSCALE", {"1", "banana", cmd::ScriptItem::esc()});
    CHECK(f.host->logContains("Not a scale"));

    f.host->run("VPLAYER", {"Freeze", "walls", "1"});
    auto frozen = layout::findLayout(f.doc, "Plan")->viewports[0].frozen;
    REQUIRE(frozen.size() == 1);
    CHECK(frozen[0] == walls);
    f.host->run("VPLAYER", {"Thaw", "Walls", "1"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].frozen.empty());
    f.host->run("VPLAYER", {"Freeze", "Nope", cmd::ScriptItem::esc()});
    CHECK(f.host->logContains("No layer named"));
}

TEST_CASE("VPLOCK stops VPSCALE until it is switched off") {
    Fixture f;
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    f.host->run("MVIEW", {"10,10", "410,287", "1:50", "2500,1000"});
    f.host->run("VPLOCK", {"On", "1"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].locked);
    f.host->run("VPSCALE", {"1", "1:100"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].scaleN == doctest::Approx(50));
    CHECK(f.host->logContains("locked"));
    f.host->run("VPLOCK", {"Off", "1"});
    f.host->run("VPSCALE", {"1", "1:100"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].scaleN == doctest::Approx(100));
}

TEST_CASE("VPLEVEL shows one level in a viewport") {
    Fixture f;
    LevelId upper;
    {
        auto tx = f.doc.begin("level");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        tx.commit();
    }
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    f.host->run("MVIEW", {"10,10", "410,287", "1:50", "2500,1000"});
    f.host->run("VPLEVEL", {"1", "upper"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].level == upper);
    f.host->run("VPLEVEL", {"1", ""});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].level.value == 0);
    f.host->run("VPLEVEL", {"1", "Nope", cmd::ScriptItem::esc()});
    CHECK(f.host->logContains("No level named"));
}

TEST_CASE("MVIEW with Enter at the centre prompt shows the middle of the drawing") {
    Fixture f;
    {
        auto tx = f.doc.begin("plan");
        Entity a;
        a.data = LineData{{0, 0}, {10000, 4000}};
        REQUIRE(tx.add(std::move(a)));
        tx.commit();
    }
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    auto r = f.host->run("MVIEW", {"10,10", "410,287", "1:50", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    auto l = layout::findLayout(f.doc, "Plan");
    REQUIRE(l->viewports.size() == 1);
    CHECK(l->viewports[0].center.x == doctest::Approx(5000));
    CHECK(l->viewports[0].center.y == doctest::Approx(2000));
}

TEST_CASE("viewports can be picked by clicking, moved, resized, re-centred and deleted") {
    Fixture f;
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    f.host->run("MVIEW", {"10,10", "110,110", "1:50", "2500,1000"});
    f.host->run("MVIEW", {"150,10", "250,110", "1:100", "0,0"});
    auto vp = [&](int id) { return *layout::findLayout(f.doc, "Plan")->findViewport(id); };

    f.host->run("VPSCALE", {cmd::ScriptItem::click({200, 50}), "1:20"});  // the second one
    CHECK(vp(2).scaleN == doctest::Approx(20));
    CHECK(vp(1).scaleN == doctest::Approx(50));

    f.host->run("VPMOVE", {"1", "0,0", "5,5"});
    CHECK(vp(1).paper.min.x == doctest::Approx(15));
    CHECK(vp(1).paper.min.y == doctest::Approx(15));
    CHECK(vp(1).paper.width() == doctest::Approx(100));

    f.host->run("VPRESIZE", {"2", "150,10", "280,150"});
    CHECK(vp(2).paper.width() == doctest::Approx(130));
    CHECK(vp(2).paper.height() == doctest::Approx(140));
    f.host->run("VPRESIZE", {"2", "5,5", "5,50"});  // no width
    CHECK(f.host->logContains("needs a size"));

    f.host->run("VPCENTER", {"1", "400,300"});
    CHECK(vp(1).center.x == doctest::Approx(400));

    f.host->run("VPDELETE", {cmd::ScriptItem::click({300, 200})});  // outside every viewport
    CHECK(f.host->logContains("No viewport there"));
    f.host->run("VPDELETE", {"1"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports.size() == 1);
    CHECK(f.host->undo());
    CHECK(layout::findLayout(f.doc, "Plan")->viewports.size() == 2);
}

TEST_CASE("commands that need a layout say so - LAYOUT List, Rename, Paper and Delete work") {
    Fixture f;
    f.host->run("MVIEW", {});
    CHECK(f.host->logContains("no layouts"));
    f.host->run("LAYOUT", {"New", "A", "A4", "Portrait"});
    f.host->run("LAYOUT", {"New", "B", "A3", ""});
    f.host->clearLog();
    f.host->run("LAYOUT", {"List"});
    CHECK(f.host->logContains("* B"));
    f.host->run("LAYOUT", {"Rename", "A", "Alpha"});
    CHECK(layout::findLayout(f.doc, "Alpha").has_value());
    f.host->run("LAYOUT", {"Paper", "Alpha", "A2", "Landscape"});
    CHECK(layout::findLayout(f.doc, "Alpha")->widthMm == doctest::Approx(594));
    f.host->run("LAYOUT", {"Set", "Alpha"});
    CHECK(layoutcmd::currentLayout(*f.host, f.doc) == "Alpha");
    f.host->run("LAYOUT", {"Delete", "Alpha"});
    CHECK(layout::layouts(f.doc).size() == 1);
    CHECK(layoutcmd::currentLayout(*f.host, f.doc) == "B");  // falls back to the first one
}

TEST_CASE("TITLEBLOCK adds objects to the sheet") {
    Fixture f;
    f.host->run("LAYOUT", {"New", "S", "A3", ""});
    auto r = f.host->run("TITLEBLOCK", {"Tower", "Ground floor", "1:100", "A-101", "2026-10-13"});
    CHECK(r.finished);
    auto l = layout::findLayout(f.doc, "S");
    const BlockDef* def = f.doc.block(l->content);
    REQUIRE(def != nullptr);
    // One Insert of the TITLEBLOCK block, whose values are attributes.
    REQUIRE(def->entities.size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(def->entities.front()));
    REQUIRE(ins != nullptr);
    std::string project;
    for (const auto& a : ins->attribs)
        if (a.tag == "PROJECT")
            project = a.value;
    CHECK(project == "Tower");
    CHECK(f.doc.blockByName("TITLEBLOCK") != nullptr);
    CHECK(f.doc.modelSpaceCount() == 0);  // nothing leaked into model space
}
