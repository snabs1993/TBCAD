#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/groups.h"
#include "tbcad/layers/layer_ops.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};
}  // namespace

TEST_CASE("COLOR and LWEIGHT set what new objects get, with undo") {
    Fixture f;
    f.host.run("COLOR", {"red"});
    CHECK(f.doc.header().currentColor == Color::index(1));
    f.host.run("LWEIGHT", {"0.35"});
    CHECK(f.doc.header().currentLineweight == LineWeight::mm(0.35));
    CHECK(f.host.undo());
    CHECK(f.doc.header().currentLineweight.isByLayer());
    f.host.run("COLOR", {"bylayer"});
    CHECK(f.doc.header().currentColor == Color::byLayer());
    f.host.run("COLOR", {"nonsense"});
    CHECK(f.doc.header().currentColor == Color::byLayer());
    f.host.run("COLOR", {"ByParent"});
    CHECK(f.host.logContains("sub-layers"));
}

TEST_CASE("LTSCALE and UNITS change the drawing header") {
    Fixture f;
    f.host.run("LTSCALE", {"2.5"});
    CHECK(f.doc.header().ltScale == doctest::Approx(2.5));
    f.host.run("LTSCALE", {"-1", "4"});  // out of range: asked again
    CHECK(f.host.logContains("out of range"));
    CHECK(f.doc.header().ltScale == doctest::Approx(4));
    f.host.run("-UNITS", {"Inches"});
    CHECK(f.doc.header().units == LengthUnit::Inch);
    CHECK(f.host.undo());
    CHECK(f.doc.header().units == LengthUnit::Millimeter);
}

TEST_CASE("STATUS lists the settings and REGEN does nothing harmful") {
    Fixture f;
    f.host.run("STATUS", {});
    CHECK(f.host.logContains("Objects: 0"));
    CHECK(f.host.logContains("Linetype scale"));
    f.host.run("REGEN", {});
    f.host.run("REDRAW", {});
    CHECK(f.host.logContains("up to date"));
}

TEST_CASE("RENAME renames layers and styles and refuses clashes") {
    Fixture f;
    f.host.run("-LAYER", {"New", "Walls", ""});
    CHECK(f.doc.layerByName("Walls") != nullptr);
    f.host.run("RENAME", {"Layer", "Walls", "Muros"});
    CHECK(f.doc.layerByName("Walls") == nullptr);
    CHECK(f.doc.layerByName("Muros") != nullptr);
    CHECK(f.host.undo());
    CHECK(f.doc.layerByName("Walls") != nullptr);

    f.host.run("RENAME", {"Layer", "0", "Zero"});  // layer 0 is protected
    CHECK(f.doc.layerByName("0") != nullptr);

    f.host.run("RENAME", {"Textstyle", "Standard", "X"});
    CHECK(f.host.logContains("cannot be renamed"));
    f.host.run("RENAME", {"Dimstyle", "Standard", "X"});
    CHECK(f.doc.dimStyleByName("Standard") != nullptr);
    f.host.run("RENAME", {"Linetype", "Continuous", "Solid"});
    CHECK(f.doc.linetypeByName("Continuous") != nullptr);
    f.host.run("RENAME", {"Block", "Nope", "B"});
    CHECK(f.host.logContains("There is no block"));
}

TEST_CASE("SETVAR reads and changes drawing variables by name") {
    Fixture f;
    f.host.run("SETVAR", {"ltscale", "3"});
    CHECK(f.doc.header().ltScale == doctest::Approx(3));
    f.host.run("SETVAR", {"CECOLOR", "red"});
    CHECK(f.doc.header().currentColor == Color::index(1));
    f.host.run("SETVAR", {"INSUNITS", "meters"});
    CHECK(f.doc.header().units == LengthUnit::Meter);
    CHECK(f.host.undo());
    CHECK(f.doc.header().units == LengthUnit::Millimeter);
    f.host.run("SETVAR", {"CLAYER", "NoSuchLayer"});
    CHECK(f.host.logContains("no layer"));
    f.host.run("SETVAR", {"NOPE"});
    CHECK(f.host.logContains("Unknown variable"));
    f.host.run("SETVAR", {"?"});
    CHECK(f.host.logContains("CELWEIGHT"));
}

TEST_CASE("ADDSELECTED copies the layer and properties of an object to the current ones") {
    Fixture f;
    LayerId walls{};
    EntityId line;
    {
        auto tx = f.doc.begin("setup");
        Layer l;
        l.name = "Walls";
        walls = tx.addLayer(std::move(l)).value();
        Entity e;
        e.data = LineData{{0, 0}, {10, 0}};
        e.common.layer = walls;
        e.common.color = Color::index(3);
        e.common.lineweight = LineWeight::mm(0.5);
        line = tx.add(std::move(e)).value();
        tx.commit();
    }
    f.host.run("ADDSELECTED", {cmd::ScriptItem::pick(line, {5, 0})});
    CHECK(f.doc.header().currentLayer == walls);
    CHECK(f.doc.header().currentColor == Color::index(3));
    CHECK(f.doc.header().currentLineweight == LineWeight::mm(0.5));
    CHECK(f.host.undo());
    CHECK(f.doc.header().currentLayer != walls);
}

namespace {
struct ViewHost : cmd::ScriptHost {
    using cmd::ScriptHost::ScriptHost;
    cmd::ViewState state{{10, 20}, 0.5, {800, 600}};
    cmd::ViewState view() const override { return state; }
    void setView(const cmd::ViewState& v) override { state = v; }
};
}  // namespace

TEST_CASE("VIEW saves, restores, lists and deletes named views") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    ViewHost host(doc, reg);
    host.run("-VIEW", {"Save", "Plan"});
    CHECK(doc.header().custom.count("tbcad.view.Plan") == 1);
    host.state.center = {999, 999};
    host.state.scale = 7;
    host.run("-VIEW", {"Restore", "Plan"});
    CHECK(host.state.center.x == doctest::Approx(10));
    CHECK(host.state.center.y == doctest::Approx(20));
    CHECK(host.state.scale == doctest::Approx(0.5));
    host.run("NAMEDVIEW", {"List"});
    CHECK(host.logContains("Plan"));
    host.run("-VIEW", {"Restore", "Nope"});
    CHECK(host.logContains("no view named"));
    host.run("-VIEW", {"Delete", "Plan"});
    CHECK(doc.header().custom.count("tbcad.view.Plan") == 0);
    CHECK(host.undo());
    CHECK(doc.header().custom.count("tbcad.view.Plan") == 1);
}

TEST_CASE("TIME shows the current date and time") {
    Fixture f;
    f.host.run("TIME", {});
    CHECK(f.host.logContains("Current time: 20"));
}

TEST_CASE("DWGUNITS is UNITS, and FLATTEN says the drawing is flat") {
    Fixture f;
    f.host.run("DWGUNITS", {"Feet"});
    CHECK(f.doc.header().units == LengthUnit::Foot);
    EntityId a, b;
    LevelId upper;
    {
        Transaction tx = f.doc.begin("lines");
        a = tx.add(Entity{{}, LineData{{0, 0}, {1, 0}}}).value();
        b = tx.add(Entity{{}, LineData{{0, 1}, {1, 1}}}).value();
        Level up;
        up.name = "Roof";
        up.elevation = 3000;
        upper = tx.addLevel(up).value();
        REQUIRE(tx.modify(b, [&](Entity& e) { e.common.level = upper; }));
        tx.commit();
    }
    f.host.run("FLATTEN", {cmd::ScriptItem::select({a}), cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("nothing to flatten"));
    f.host.run("FLATTEN", {cmd::ScriptItem::select({a, b}), cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("1 object moved to the default level"));
    CHECK(f.doc.find(b)->common.level == kLevelDefault);
    f.host.undo();
    CHECK(f.doc.find(b)->common.level == upper);
}

TEST_CASE("HYPERLINK sets, lists, opens and removes a link, the link is saved in the drawing") {
    Fixture f;
    EntityId line;
    {
        Transaction tx = f.doc.begin("line");
        line = tx.add(Entity{{}, LineData{{0, 0}, {10, 0}}}).value();
        tx.commit();
    }
    f.host.run("HYPERLINK", {"Set", cmd::ScriptItem::pick(line, {5, 0}), "https://example.org/spec",
                             "Spec sheet"});
    REQUIRE(f.doc.header().custom.count("tbcad.link." + std::to_string(line.value)) == 1);
    f.host.run("HYPERLINK", {"List"});
    CHECK(f.host.logContains("https://example.org/spec"));
    CHECK(f.host.logContains("Spec sheet"));
    f.host.run("HYPERLINK", {"Open", cmd::ScriptItem::pick(line, {5, 0})});
    CHECK(f.host.logContains("Opening https://example.org/spec"));
    f.host.run("HYPERLINK", {"Remove", cmd::ScriptItem::pick(line, {5, 0})});
    CHECK(f.doc.header().custom.count("tbcad.link." + std::to_string(line.value)) == 0);
    CHECK(f.host.undo());
    CHECK(f.doc.header().custom.count("tbcad.link." + std::to_string(line.value)) == 1);
}

TEST_CASE("SPACETRANS converts between paper and model lengths, DELAY checks its range") {
    Fixture f;
    f.host.run("SPACETRANS", {"Paper", "2.5", "50"});
    CHECK(f.host.logContains("125 in model units"));
    f.host.run("SPACETRANS", {"Model", "5000", "100"});
    CHECK(f.host.logContains("50 in paper units"));
    f.host.run("SPACETRANS", {"Paper", "1", "0"});
    CHECK(f.host.logContains("above zero"));
    CHECK(f.host.run("DELAY", {"5"}).finished);
    f.host.run("DELAY", {"99999"});
    CHECK(f.host.logContains("0 to 10000"));
}

TEST_CASE("CELTSCALE, PSLTSCALE, MSLTSCALE and PLINEGEN are commands and SETVAR names") {
    Fixture f;
    CHECK(ceLtScale(f.doc.header()) == 1);
    CHECK(psLtScale(f.doc.header()));
    f.host.run("CELTSCALE", {"0.25"});
    CHECK(ceLtScale(f.doc.header()) == doctest::Approx(0.25));
    f.host.run("PSLTSCALE", {"0"});
    CHECK_FALSE(psLtScale(f.doc.header()));
    f.host.run("MSLTSCALE", {"Off"});
    CHECK_FALSE(msLtScale(f.doc.header()));
    f.host.run("PLINEGEN", {"0"});
    CHECK_FALSE(plineGen(f.doc.header()));
    f.host.run("PLINEGEN", {"maybe"});
    CHECK(f.host.logContains("PLINEGEN is 0 or 1"));
    CHECK_FALSE(plineGen(f.doc.header()));
    f.host.run("SETVAR", {"CELTSCALE", "3"});
    CHECK(ceLtScale(f.doc.header()) == doctest::Approx(3));
    CHECK(f.host.undo());
    CHECK(ceLtScale(f.doc.header()) == doctest::Approx(0.25));
    // New objects take the current linetype scale.
    f.host.run("LINE", {"0,0", "10,0", ""});
    bool found = false;
    f.doc.forEachEntity([&](const Entity& e) {
        found = true;
        CHECK(e.common.linetypeScale == doctest::Approx(0.25));
    });
    CHECK(found);
}

TEST_CASE("LWDISPLAY shows or hides lineweights on screen") {
    Fixture f;
    f.host.run("LWDISPLAY", {"0"});
    CHECK(f.host.setting("view.lineweights") == "0");
    f.host.run("LWDISPLAY", {"1"});
    CHECK(f.host.setting("view.lineweights") == "1");
    f.host.run("LWDISPLAY", {"maybe"});
    CHECK(f.host.logContains("LWDISPLAY is 0 or 1"));
}

TEST_CASE("QPMODE and ROLLOVERTIPS switch Quick Properties and hover tooltips") {
    Fixture f;
    f.host.run("QPMODE", {"1"});
    CHECK(f.host.setting("ui.quickProps") == "1");
    f.host.run("QPMODE", {"0"});
    CHECK(f.host.setting("ui.quickProps") == "0");
    f.host.run("ROLLOVERTIPS", {""});  // Enter keeps the default: on
    CHECK(f.host.setting("hover.tooltip") == "1");
    f.host.run("ROLLOVERTIPS", {"0"});
    CHECK(f.host.setting("hover.tooltip") == "0");
    f.host.run("QPMODE", {"2"});
    CHECK(f.host.logContains("QPMODE is 0 or 1"));
}

TEST_CASE("the Options dialog's variables read and write its settings") {
    Fixture f;
    f.host.run("PICKBOX", {""});  // Enter keeps the default
    CHECK(f.host.logContains("PICKBOX = 6"));
    f.host.run("PICKBOX", {"9"});
    CHECK(f.host.setting("pick.aperturePx") == "9");
    f.host.run("PICKBOX", {"80"});
    CHECK(f.host.logContains("PICKBOX is a whole number from 1 to 50"));
    CHECK(f.host.setting("pick.aperturePx") == "9");
    f.host.run("CURSORSIZE", {"5"});
    CHECK(f.host.setting("view.crosshairSize") == "5");
    f.host.run("APERTURE", {"15"});
    CHECK(f.host.setting("drafting.osnap.aperture") == "15");
    f.host.run("GRIPSIZE", {"7"});
    CHECK(f.host.setting("grips.size") == "7");
    f.host.run("GRIPCOLOR", {"3"});
    CHECK(f.host.setting("grips.color") == "3");
    f.host.run("GRIPHOT", {"256"});
    CHECK(f.host.logContains("GRIPHOT is a whole number from 1 to 255"));
    f.host.run("LAYLOCKFADECTL", {"-40"});
    CHECK(f.host.setting("view.lockFade") == "-40");
    f.host.run("ISAVEBAK", {"0"});
    CHECK(f.host.setting("files.backup") == "0");
    f.host.run("ISAVEPERCENT", {"0"});
    CHECK(f.host.setting("files.isavePercent") == "0");
    f.host.run("ISAVEPERCENT", {"101"});
    CHECK(f.host.setting("files.isavePercent") == "0");
    f.host.run("APBOX", {"1"});
    CHECK(f.host.setting("osnap.apbox") == "1");

    f.host.run("SAVETIME", {"25"});
    CHECK(f.host.setting("autosave.enabled") == "1");
    CHECK(f.host.setting("autosave.minutes") == "25");
    f.host.run("SAVETIME", {"0"});
    CHECK(f.host.setting("autosave.enabled") == "0");
    CHECK(f.host.setting("autosave.minutes") == "25");  // kept for when it is switched back on
    f.host.run("SAVETIME", {""});
    CHECK(f.host.logContains("SAVETIME = 0"));

    f.host.run("SHORTCUTMENU", {""});
    CHECK(f.host.logContains("SHORTCUTMENU = 7"));
    f.host.run("SHORTCUTMENU", {"11"});
    CHECK(f.host.setting("ui.rightClick.default") == "menu");
    CHECK(f.host.setting("ui.rightClick.edit") == "menu");
    CHECK(f.host.setting("ui.rightClick.command") == "options");
    f.host.run("SHORTCUTMENU", {"0"});
    CHECK(f.host.setting("ui.rightClick.default") == "repeat");
    CHECK(f.host.setting("ui.rightClick.command") == "enter");
}

TEST_CASE("SCRIPT types each line of the file, RSCRIPT repeats it, Esc stops") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_script_test";
    fs::create_directories(dir);
    const fs::path file = dir / "draw.scr";
    {
        std::ofstream o(file);
        o << "; two lines and a circle\r\nLINE 0,0 100,0 \r\nLINE\r\n0,10\r\n100,10\r\n\r\n"
             "CIRCLE 50,50 20\r\n";
    }
    auto count = [](Document& d) {
        std::size_t n = 0;
        d.forEachEntity([&](const Entity&) { ++n; });
        return n;
    };
    {
        Fixture f;
        f.host.run("SCRIPT", {file.string()});
        CHECK(count(f.doc) == 3);
        CHECK_FALSE(f.host.runner().active());
        CHECK(f.host.undoStack().undoCount() == 3);
    }
    {
        Fixture f;
        f.host.run("RSCRIPT", {file.string(), "4"});
        CHECK(count(f.doc) == 12);
    }
    {
        // a script that runs another one
        const fs::path outer = dir / "outer.scr";
        {
            std::ofstream o(outer);
            o << "SCRIPT " << file.string() << "\nCIRCLE 0,0 5\n";
        }
        Fixture f;
        f.host.run("SCRIPT", {outer.string()});
        CHECK(count(f.doc) == 4);
    }
    {
        Fixture f;
        f.host.run("SCRIPT", {(dir / "missing").string()});
        CHECK(f.host.logContains("Cannot open"));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("RESUME continues a script that Esc interrupted") {
    Fixture f;
    f.host.run("RESUME", {});
    CHECK(f.host.logContains("No interrupted script"));
    f.host.runner().queueScript({"CIRCLE 0,0 5", "CIRCLE 10,0 5"});
    f.host.runner().submit(cmd::Input::cancel());  // Esc before the first line ran
    CHECK(f.host.logContains("RESUME"));
    std::size_t n = 0;
    f.doc.forEachEntity([&](const Entity&) { ++n; });
    CHECK(n == 0);
    f.host.run("RESUME", {});
    n = 0;
    f.doc.forEachEntity([&](const Entity&) { ++n; });
    CHECK(n == 2);
}

TEST_CASE("LIMITS stores the drawing limits, keeps them on Enter and undoes") {
    Fixture f;
    CHECK(drawingLimits(f.doc.header()).empty());
    f.host.run("LIMITS", {"0,0", "1000,500"});
    BBox2 b = drawingLimits(f.doc.header());
    REQUIRE_FALSE(b.empty());
    CHECK(b.max.x == doctest::Approx(1000));
    CHECK(b.max.y == doctest::Approx(500));
    f.host.run("LIMITS", {"-10,-20", ""});  // Enter keeps the upper right corner
    b = drawingLimits(f.doc.header());
    CHECK(b.min.x == doctest::Approx(-10));
    CHECK(b.max.x == doctest::Approx(1000));
    f.host.run("LIMITS", {"50,50", "10,10"});  // inverted: refused
    CHECK(drawingLimits(f.doc.header()).min.x == doctest::Approx(-10));
    f.host.run("LIMITS", {"ON"});
    CHECK(limitsCheck(f.doc.header()));
    f.host.run("LIMITS", {"OFF"});
    CHECK_FALSE(limitsCheck(f.doc.header()));
    CHECK(f.host.undo());
    CHECK(limitsCheck(f.doc.header()));
}

TEST_CASE("REGENAUTO is accepted") {
    Fixture f;
    CHECK(f.host.run("REGENAUTO", {}).finished);
}

TEST_CASE("FILL and ATTDISP set the display modes in the header, with undo") {
    Fixture f;
    CHECK(fillMode(f.doc.header()));
    CHECK(attDisplay(f.doc.header()) == 0);
    f.host.run("FILL", {"OFF"});
    CHECK_FALSE(fillMode(f.doc.header()));
    f.host.run("ATTDISP", {"ON"});
    CHECK(attDisplay(f.doc.header()) == 1);
    f.host.run("ATTDISP", {"OFF"});
    CHECK(attDisplay(f.doc.header()) == 2);
    f.host.run("SETVAR", {"ATTMODE", "1"});
    CHECK(attDisplay(f.doc.header()) == 0);
    CHECK(f.host.undo());
    CHECK(attDisplay(f.doc.header()) == 2);
    f.host.run("FILL", {"ON"});
    CHECK(fillMode(f.doc.header()));
}

TEST_CASE("QTEXT sets quick text in the header, CONVERTPOLY only reports") {
    Fixture f;
    CHECK_FALSE(quickText(f.doc.header()));
    f.host.run("QTEXT", {"ON"});
    CHECK(quickText(f.doc.header()));
    f.host.run("SETVAR", {"QTEXTMODE", "0"});
    CHECK_FALSE(quickText(f.doc.header()));
    CHECK(f.host.run("CONVERTPOLY", {}).finished);
    CHECK(f.host.logContains("optimized"));
}

TEST_CASE("MULTIPLE repeats a command until Esc") {
    Fixture f;
    f.host.run("MULTIPLE", {"LINE"});
    CHECK(f.host.runner().active());  // LINE waits for its first point
    f.host.feed({"0,0", "10,0", "", "0,5", "10,5", ""});
    int lines = 0;
    f.doc.forEachEntity([&](const Entity& e) { lines += kind(e) == EntityKind::Line ? 1 : 0; });
    CHECK(lines == 2);
    CHECK(f.host.runner().active());  // a third LINE waits for its first point
    f.host.feed({cmd::ScriptItem::esc()});
    CHECK_FALSE(f.host.runner().active());
    f.host.run("LINE", {"0,9", "10,9", ""});  // no longer repeating
    CHECK_FALSE(f.host.runner().active());
}

TEST_CASE("MULTIPLE refuses unknown names and itself") {
    Fixture f;
    f.host.run("MULTIPLE", {"NOSUCH", "MULTIPLE"});
    CHECK(f.host.logContains("Unknown command"));
    CHECK(f.host.logContains("cannot repeat itself"));
}

TEST_CASE("PICKSTYLE is a command and a SETVAR name, 0 to 3") {
    Fixture f;
    CHECK(pickStyle(f.doc.header()) == 1);
    f.host.run("PICKSTYLE", {"3"});
    CHECK(pickStyle(f.doc.header()) == 3);
    f.host.run("SETVAR", {"PICKSTYLE", "7"});
    CHECK(f.host.logContains("PICKSTYLE is 0, 1, 2 or 3."));
    CHECK(pickStyle(f.doc.header()) == 3);
    f.host.run("SETVAR", {"PICKSTYLE", "0"});
    CHECK(pickStyle(f.doc.header()) == 0);
}

TEST_CASE("HPORIGIN is a command and a SETVAR name, a point") {
    Fixture f;
    CHECK(hatchOrigin(f.doc.header()).x == 0);
    f.host.run("HPORIGIN", {"12.5,-3"});
    CHECK(hatchOrigin(f.doc.header()).x == doctest::Approx(12.5));
    CHECK(hatchOrigin(f.doc.header()).y == doctest::Approx(-3));
    f.host.run("SETVAR", {"HPORIGIN", "bad"});
    CHECK(f.host.logContains("HPORIGIN is a point: x,y."));
    CHECK(hatchOrigin(f.doc.header()).x == doctest::Approx(12.5));
}
