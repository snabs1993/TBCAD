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

// A host that is "on a sheet", as the application is while a layout is shown.
struct SheetHost : cmd::ScriptHost {
    using cmd::ScriptHost::ScriptHost;
    BlockId drawingOwner() const override { return BlockId{1}; }
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
    f.host->run("VPLOCK", {"1", "Lock"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].locked);
    f.host->run("VPSCALE", {"1", "1:100"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].scaleN == doctest::Approx(50));
    CHECK(f.host->logContains("locked"));
    f.host->run("VPLOCK", {"1", "Unlock"});
    f.host->run("VPSCALE", {"1", "1:100"});
    CHECK(layout::findLayout(f.doc, "Plan")->viewports[0].scaleN == doctest::Approx(100));
}

TEST_CASE("a viewport selected on the sheet is used without asking") {
    Document doc;
    cmd::Registry reg;
    layoutcmd::registerCommands(reg);
    SheetHost host(doc, reg);
    host.run("LAYOUT", {"New", "Plan", "A3", ""});
    host.run("MVIEW", {"10,10", "200,200", "1:50", "100,100"});
    host.run("MVIEW", {"210,10", "410,200", "1:50", "900,100"});
    CHECK(layoutcmd::selectedViewport(host, doc) == 0);
    host.setSetting("tmp.layout.viewport", "Plan:2");
    CHECK(layoutcmd::selectedViewport(host, doc) == 2);
    host.run("VPSCALE", {"1:200"});  // no viewport answer: the selected one is taken
    CHECK(layout::findLayout(doc, "Plan")->viewports[0].scaleN == doctest::Approx(50));
    CHECK(layout::findLayout(doc, "Plan")->viewports[1].scaleN == doctest::Approx(200));
    CHECK(host.logContains("selected viewport 2"));
    host.setSetting("tmp.layout.viewport", "Other:2");  // another sheet: not ours
    CHECK(layoutcmd::selectedViewport(host, doc) == 0);
}

TEST_CASE("VPLOCK locks a selected viewport without asking") {
    Document doc;
    cmd::Registry reg;
    layoutcmd::registerCommands(reg);
    SheetHost host(doc, reg);
    host.run("LAYOUT", {"New", "Plan", "A3", ""});
    host.run("MVIEW", {"10,10", "200,200", "1:50", "100,100"});
    host.setSetting("tmp.layout.viewport", "Plan:1");
    host.run("VPLOCK", {});
    CHECK(layout::findLayout(doc, "Plan")->viewports[0].locked);
}

TEST_CASE("CHSPACE moves objects to the sheet at viewport size and back into model space") {
    Document doc;
    cmd::Registry reg;
    layoutcmd::registerCommands(reg);
    cmd::ScriptHost model(doc, reg);
    model.run("LAYOUT", {"New", "Plan", "A3", ""});
    model.run("MVIEW",
              {"10,10", "210,110", "1:50", "1000,500"});  // centre of the sheet rect: 110,60
    EntityId id;
    {
        auto tx = doc.begin("line");
        id = tx.add(Entity{{}, LineData{{1000, 500}, {1500, 500}}}).value();
        tx.commit();
    }
    auto r = model.run("CHSPACE", {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(), "1"});
    CHECK(r.finished);
    CHECK(model.logContains("1 objects moved to the sheet"));
    auto lay = layout::findLayout(doc, "Plan");
    REQUIRE(lay.has_value());
    const BlockDef* sheet = doc.block(lay->content);
    REQUIRE(sheet != nullptr);
    REQUIRE(sheet->entities.size() == 1);
    const Entity* moved = doc.find(sheet->entities[0]);
    const auto* ln = entityAs<LineData>(*moved);
    REQUIRE(ln != nullptr);
    CHECK(ln->a.x == doctest::Approx(110));
    CHECK(ln->a.y == doctest::Approx(60));
    CHECK(ln->b.x == doctest::Approx(120));  // 500 model units at 1:50 = 10 mm
    CHECK(doc.modelSpace().empty());

    SheetHost onSheet(doc, reg);
    onSheet.run("CHSPACE",
                {cmd::ScriptItem::select({sheet->entities[0]}), cmd::ScriptItem::enter(), "1"});
    CHECK(onSheet.logContains("moved into model space"));
    REQUIRE(doc.modelSpace().size() == 1);
    const auto* back = entityAs<LineData>(*doc.find(doc.modelSpace()[0]));
    CHECK(back->a.x == doctest::Approx(1000));
    CHECK(back->b.x == doctest::Approx(1500));
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

TEST_CASE("VPCLIP gives a viewport a polygonal outline, from points or a closed polyline") {
    Fixture f;
    f.host->run("LAYOUT", {"New", "Plan", "A3", ""});
    f.host->run("MVIEW", {"10,10", "110,110", "1:50", "2500,1000"});
    auto vp = [&] { return *layout::findLayout(f.doc, "Plan")->findViewport(1); };

    auto r = f.host->run("VPCLIP",
                         {"1", "Polygonal", "20,20", "100,20", "60,100", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    REQUIRE(vp().clip.size() == 3);
    CHECK(vp().paper.min.x == doctest::Approx(20));
    CHECK(vp().paper.max.y == doctest::Approx(100));
    CHECK(layout::viewportContains(vp(), {60, 50}));
    CHECK_FALSE(layout::viewportContains(vp(), {25, 90}));  // inside the old rectangle only

    f.host->run("VPMOVE", {"1", "0,0", "10,0"});  // the outline moves with the rectangle
    CHECK(vp().clip[0].x == doctest::Approx(30));
    CHECK(vp().paper.min.x == doctest::Approx(30));

    CHECK(f.host->undo());
    CHECK(vp().clip[0].x == doctest::Approx(20));

    f.host->run("VPCLIP", {"1", "Delete"});
    CHECK(vp().clip.empty());

    // A closed polyline on the sheet.
    EntityId poly{};
    {
        auto tx = f.doc.begin("outline");
        PolylineData pl;
        pl.closed = true;
        for (Vec2 v : {Vec2{15, 15}, Vec2{105, 15}, Vec2{105, 60}, Vec2{15, 60}})
            pl.v.push_back({v, 0, 0, 0});
        Entity e;
        e.data = std::move(pl);
        const BlockDef* sheet = f.doc.block(layout::findLayout(f.doc, "Plan")->content);
        REQUIRE(sheet);
        poly = tx.add(std::move(e), sheet->id).value();
        tx.commit();
    }
    f.host->run("VPCLIP", {"1", "Object", cmd::ScriptItem::pick(poly, {15, 15})});
    CHECK(vp().clip.size() == 4);
    CHECK(vp().paper.max.y == doctest::Approx(60));
}
