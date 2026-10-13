#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layers/layer_ops.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    Fixture() { registerAllCommands(reg); }
    cmd::ScriptHost host{doc, reg};

    cmd::ScriptResult run(const char* command, std::vector<cmd::ScriptItem> items) {
        return host.run(command, items);
    }
    // A line on the current layer, added outside any command.
    EntityId addLine() {
        auto tx = doc.begin("line");
        auto id = tx.add(Entity{{}, LineData{{0, 0}, {10, 0}}});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    const Layer* layer(const char* path) {
        auto r = layers::resolveLayer(doc, path);
        return r ? doc.layer(r.value()) : nullptr;
    }
};

}  // namespace

TEST_CASE("the module registers LAYER, -LAYER and friends with aliases") {
    Fixture f;
    CHECK(f.reg.find("LAYER") != nullptr);
    CHECK(f.reg.find("-LAYER") != nullptr);
    CHECK(f.reg.find("LAYERCURRENT") != nullptr);
    CHECK(f.reg.find("-PROP") != nullptr);
    CHECK(f.reg.find("-LINETYPE") != nullptr);
}

TEST_CASE("LAYER asks the application to show the panel") {
    Fixture f;
    auto r = f.run("LAYER", {});
    CHECK(r.finished);
    CHECK(f.host.setting("ui.showPanel") == std::optional<std::string>("layers"));
}

TEST_CASE("-LAYER New and NewSub create layers and sub-layers, one undo step") {
    Fixture f;
    auto r = f.run("-LAYER", {"New", "S-FLOOR,Walls", "NewSub", "S-FLOOR", "Floor hidden,edge",
                              cmd::ScriptItem::enter()});
    CHECK(r.finished);
    REQUIRE(f.layer("S-FLOOR") != nullptr);
    REQUIRE(f.layer("Walls") != nullptr);
    const Layer* hidden = f.layer("S-FLOOR~Floor hidden");
    REQUIRE(hidden != nullptr);
    CHECK(hidden->color.kind == ColorKind::ByParent);
    CHECK(f.layer("S-FLOOR~edge") != nullptr);
    CHECK(f.doc.layers().size() == 5);

    CHECK(f.host.undo());
    CHECK(f.doc.layers().size() == 1);
    CHECK(f.host.redo());
    CHECK(f.doc.layers().size() == 5);
}

TEST_CASE("-LAYER: On, Off, Freeze, Thaw, Lock, Unlock, Plot with lists and *") {
    Fixture f;
    f.run("-LAYER", {"New", "A,B,C", cmd::ScriptItem::enter()});
    f.run("-LAYER",
          {"Off", "A,B", "Freeze", "C", "Lock", "A", "NoPlot", "B", cmd::ScriptItem::enter()});
    CHECK_FALSE(f.layer("A")->on);
    CHECK_FALSE(f.layer("B")->on);
    CHECK(f.layer("C")->frozen);
    CHECK(f.layer("A")->locked);
    CHECK_FALSE(f.layer("B")->plot);

    f.run("-LAYER", {"On", "*", "Thaw", "C", "Unlock", "A", "Plot", "B", cmd::ScriptItem::enter()});
    CHECK(f.layer("A")->on);
    CHECK(f.layer("B")->on);
    CHECK_FALSE(f.layer("C")->frozen);
    CHECK_FALSE(f.layer("A")->locked);
    CHECK(f.layer("B")->plot);
}

TEST_CASE("-LAYER: Set makes a layer current and refuses a frozen one") {
    Fixture f;
    f.run("-LAYER", {"New", "A,B", "Set", "A", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == f.layer("A")->id);
    f.host.clearLog();
    f.run("-LAYER", {"Freeze", "B", "Set", "B", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == f.layer("A")->id);
    CHECK(f.host.logContains("frozen"));
    f.host.clearLog();
    f.run("-LAYER", {"Freeze", "A", cmd::ScriptItem::enter()});  // the current layer
    CHECK(f.host.logContains("current layer"));
    CHECK_FALSE(f.layer("A")->frozen);
}

TEST_CASE("-LAYER: Color, Linetype and Lineweight, including ByParent") {
    Fixture f;
    f.run("-LAYER", {"New", "P", "NewSub", "P", "C", cmd::ScriptItem::enter()});
    f.run("-LAYER", {"Color", "red", "P", "Lineweight", "0.5", "P", cmd::ScriptItem::enter()});
    CHECK(f.layer("P")->color == Color::index(1));
    CHECK(f.layer("P")->lineweight == LineWeight::mm(0.5));
    CHECK(f.doc.effective(f.layer("P~C")->id).color == Color::index(1));
    f.run("-LAYER", {"Color", "0,128,255", "P~C", cmd::ScriptItem::enter()});
    CHECK(f.doc.effective(f.layer("P~C")->id).color == Color::rgb(0, 128, 255));
    f.run("-LAYER", {"Color", "ByParent", "P~C", cmd::ScriptItem::enter()});
    CHECK(f.doc.effective(f.layer("P~C")->id).color == Color::index(1));

    // A linetype must be loaded first.
    f.host.clearLog();
    f.run("-LAYER", {"Linetype", "Dashed", "P", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("not loaded"));
    f.run("-LINETYPE", {"Load", "Dashed", cmd::ScriptItem::enter()});
    f.run("-LAYER", {"Linetype", "Dashed", "P", cmd::ScriptItem::enter()});
    CHECK(f.doc.linetype(f.layer("P")->linetype)->name == "Dashed");
    CHECK(f.doc.effective(f.layer("P~C")->id).linetype == f.layer("P")->linetype);

    // ByParent on a top-level layer is refused with a message.
    f.host.clearLog();
    f.run("-LAYER", {"Color", "ByParent", "P", cmd::ScriptItem::enter()});
    CHECK(f.host.log().size() >= 1);
    CHECK(f.layer("P")->color == Color::index(1));
}

TEST_CASE("-LAYER: Parent re-parents, refuses cycles, None moves to top level") {
    Fixture f;
    f.run("-LAYER", {"New", "A,B", "NewSub", "A", "A1", cmd::ScriptItem::enter()});
    f.run("-LAYER", {"Parent", "B", "A", cmd::ScriptItem::enter()});
    CHECK(f.layer("A~B") != nullptr);
    f.host.clearLog();
    f.run("-LAYER", {"Parent", "A", "A~B", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("cycle"));
    CHECK(f.layer("A") != nullptr);
    f.run("-LAYER", {"Parent", "A~B", "None", cmd::ScriptItem::enter()});
    CHECK(f.layer("B") != nullptr);
    CHECK(f.layer("B")->parent == LayerId{});
}

TEST_CASE("-LAYER: Rename, and Delete with the Branch / MoveUp choice") {
    Fixture f;
    f.run("-LAYER",
          {"New", "A", "NewSub", "A", "A1,A2", "NewSub", "A~A1", "deep", cmd::ScriptItem::enter()});
    f.run("-LAYER", {"Rename", "A", "Z", cmd::ScriptItem::enter()});
    CHECK(f.layer("Z~A1") != nullptr);

    // Layer with sub-layers: the command asks.
    auto r = f.run("-LAYER", {"Delete", "Z~A1", "MoveUp", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.layer("Z~A1") == nullptr);
    CHECK(f.layer("Z~deep") != nullptr);

    r = f.run("-LAYER", {"Delete", "Z", "Branch", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.layer("Z") == nullptr);
    CHECK(f.layer("deep") == nullptr);
    CHECK(f.doc.layers().size() == 1);
}

TEST_CASE("-LAYER: deleting a layer in use or layer 0 fails with a message") {
    Fixture f;
    f.run("-LAYER", {"New", "Walls", "Set", "Walls", cmd::ScriptItem::enter()});
    f.addLine();  // lands on the current layer
    f.run("-LAYER", {"Set", "0", cmd::ScriptItem::enter()});
    f.host.clearLog();
    f.run("-LAYER", {"Delete", "Walls", cmd::ScriptItem::enter()});
    CHECK(f.layer("Walls") != nullptr);
    CHECK(f.host.logContains("1 object"));
    f.host.clearLog();
    f.run("-LAYER", {"Delete", "0", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("Layer 0"));
}

TEST_CASE("-LAYER: Isolate / ShowAll and List") {
    Fixture f;
    f.run("-LAYER", {"New", "A,B", "Isolate", "A", cmd::ScriptItem::enter()});
    CHECK(f.layer("A")->on);
    CHECK_FALSE(f.layer("B")->on);
    f.run("-LAYER", {"ShowAll", cmd::ScriptItem::enter()});
    CHECK(f.layer("B")->on);
    f.host.clearLog();
    f.run("-LAYER", {"Off", "B", "List", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("B"));
    CHECK(f.host.logContains("off"));
}

TEST_CASE("-LAYER: Esc keeps what was done; a bad layer name only prints an error") {
    Fixture f;
    f.run("-LAYER", {"New", "A", "Off", "nope"});
    f.host.clearLog();
    f.host.feed({cmd::ScriptItem::esc()});
    CHECK(f.layer("A") != nullptr);
}

TEST_CASE("LAYERCURRENT takes the layer of a picked object") {
    Fixture f;
    f.run("-LAYER", {"New", "Walls", "Set", "Walls", cmd::ScriptItem::enter()});
    EntityId line = f.addLine();
    f.run("-LAYER", {"Set", "0", cmd::ScriptItem::enter()});
    REQUIRE(f.doc.header().currentLayer == kLayerZero);
    f.run("LAYERCURRENT", {cmd::ScriptItem::pick(line)});
    CHECK(f.doc.header().currentLayer == f.layer("Walls")->id);
}

namespace {

// Does what the application does for a panel edit (see ui/panel_util.h): the first line is split
// into words, the command name starts it and the rest answer prompts; later lines are one answer
// each; an empty line ends the command.
void panelExec(cmd::ScriptHost& host, const std::vector<std::string>& lines) {
    std::vector<std::string> words;
    std::string w;
    for (char c : lines.at(0)) {
        if (c == ' ') {
            if (!w.empty())
                words.push_back(w);
            w.clear();
        } else {
            w += c;
        }
    }
    if (!w.empty())
        words.push_back(w);
    std::vector<cmd::ScriptItem> items;
    for (std::size_t i = 1; i < words.size(); ++i)
        items.emplace_back(words[i]);
    for (std::size_t i = 1; i < lines.size(); ++i)
        items.emplace_back(lines[i]);
    items.push_back(cmd::ScriptItem::enter());
    auto r = host.run(words[0], items);
    REQUIRE(r.finished);
}

}  // namespace

TEST_CASE("the exact command sequences the layer panel sends work, names with spaces included") {
    Fixture f;
    panelExec(f.host, {"-LAYER New", "Walls and doors"});
    const Layer* walls = f.layer("Walls and doors");
    REQUIRE(walls != nullptr);
    std::string wallsRef = "#" + std::to_string(walls->id.value);

    panelExec(f.host, {"-LAYER NewSub " + wallsRef, "Floor hidden"});
    const Layer* sub = f.layer("Walls and doors~Floor hidden");
    REQUIRE(sub != nullptr);
    std::string subRef = "#" + std::to_string(sub->id.value);

    panelExec(f.host, {"-LAYER Rename " + subRef, "Hidden floor"});
    CHECK(f.layer("Walls and doors~Hidden floor") != nullptr);

    panelExec(f.host, {"-LAYER Off " + wallsRef});
    CHECK_FALSE(f.layer("Walls and doors")->on);
    CHECK_FALSE(f.doc.effective(sub->id).on);
    panelExec(f.host, {"-LAYER On " + wallsRef});

    panelExec(f.host, {"-LAYER Color 12,34,56", subRef});
    CHECK(f.doc.effective(sub->id).color == Color::rgb(12, 34, 56));
    panelExec(f.host, {"-LAYER Lineweight 0.35", subRef});
    CHECK(f.doc.layer(sub->id)->lineweight == LineWeight::mm(0.35));

    // Drag the sub-layer out to the top level, then back under its old parent.
    panelExec(f.host, {"-LAYER Parent " + subRef, "None"});
    CHECK(f.layer("Hidden floor") != nullptr);
    panelExec(f.host, {"-LAYER Parent " + subRef, wallsRef});
    CHECK(f.layer("Walls and doors~Hidden floor") != nullptr);

    panelExec(f.host, {"-LAYER Delete " + wallsRef, "MoveUp"});
    CHECK(f.layer("Walls and doors") == nullptr);
    CHECK(f.layer("Hidden floor") != nullptr);
    CHECK(f.doc.layer(sub->id)->parent == LayerId{});
}

TEST_CASE("the exact command sequences the properties panel sends work") {
    Fixture f;
    EntityId c1, c2;
    {
        auto tx = f.doc.begin("make");
        c1 = tx.add(Entity{{}, CircleData{{0, 0}, 1}}).value();
        c2 = tx.add(Entity{{}, LineData{{0, 0}, {1, 0}}}).value();
        tx.commit();
    }
    f.host.setSelection({c1, c2});
    panelExec(f.host, {"-PROP Only Circle LTScale", "2 * 1.5"});
    CHECK(f.doc.find(c1)->common.linetypeScale == doctest::Approx(3));
    CHECK(f.doc.find(c2)->common.linetypeScale == doctest::Approx(1));
    panelExec(f.host, {"-PROP Only Circle Set radius", "10 / 4"});
    CHECK(entityAs<CircleData>(*f.doc.find(c1))->radius == doctest::Approx(2.5));
    panelExec(f.host, {"-PROP Color", "ByBlock"});
    CHECK(f.doc.find(c2)->common.color == Color::byBlock());
    CHECK(f.host.undo());
    CHECK(f.doc.find(c2)->common.color == Color::byLayer());
}
