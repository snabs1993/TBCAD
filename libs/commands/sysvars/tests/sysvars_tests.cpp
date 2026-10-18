#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
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
    f.host.run("UNITS", {"Inches"});
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
