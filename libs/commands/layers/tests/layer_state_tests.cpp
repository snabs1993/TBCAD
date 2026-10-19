#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/layout/layout.h"

using namespace tbcad;
using namespace tbcad::layers;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    Fixture() { registerAllCommands(reg); }
    cmd::ScriptHost host{doc, reg};

    // Walls, S-FLOOR with children edge and hidden.
    void build() {
        auto tx = doc.begin("setup");
        auto walls = createLayer(doc, tx, "Walls");
        auto floor = createLayer(doc, tx, "S-FLOOR");
        REQUIRE(walls.ok());
        REQUIRE(floor.ok());
        REQUIRE(createLayer(doc, tx, "edge", floor.value()).ok());
        REQUIRE(createLayer(doc, tx, "hidden", floor.value()).ok());
        tx.commit();
    }
    LayerId id(const char* path) {
        auto r = resolveLayer(doc, path);
        REQUIRE(r.ok());
        return r.value();
    }
    template <class Fn>
    void edit(Fn fn) {
        auto tx = doc.begin("edit");
        fn(tx);
        tx.commit();
    }
};

}  // namespace

TEST_CASE("wildcards are case-insensitive and support * and ?") {
    CHECK(wildcardMatch("S-*", "s-floor"));
    CHECK(wildcardMatch("*oor", "S-FLOOR"));
    CHECK(wildcardMatch("w?lls", "Walls"));
    CHECK_FALSE(wildcardMatch("w?lls", "Wlls"));
    CHECK(wildcardMatch("*", ""));
    CHECK_FALSE(wildcardMatch("a*b", "acd"));
}

TEST_CASE("filters combine words with AND and understand switches, branches and look") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) {
        REQUIRE(setFrozen(f.doc, tx, f.id("Walls"), true).ok());
        REQUIRE(setLayerColor(f.doc, tx, f.id("S-FLOOR"), Color::index(1)).ok());
    });

    auto names = [&](const char* expr) {
        std::vector<std::string> out;
        auto r = filterLayers(f.doc, expr);
        REQUIRE(r.ok());
        for (LayerId id : r.value())
            out.push_back(layerPath(f.doc, id));
        return out;
    };
    CHECK(names("frozen") == std::vector<std::string>{"Walls"});
    CHECK(names("name:S-*").size() == 1);
    CHECK(names("branch:S-FLOOR").size() == 3);
    CHECK(names("branch:S-FLOOR not:name:S-*").size() == 2);
    CHECK(names("color:red").size() == 3);              // S-FLOOR and its ByParent children
    CHECK(names("thawed unlocked empty").size() == 4);  // 0 and the three thawed layers
    CHECK(names("").size() == 5);
    CHECK(names("*").size() == 5);
    CHECK(names("hid*") == std::vector<std::string>{"S-FLOOR~hidden"});
    CHECK(names("path:S-FLOOR~e*") == std::vector<std::string>{"S-FLOOR~edge"});
    CHECK(names("current") == std::vector<std::string>{"0"});

    auto bad = filterLayers(f.doc, "branch:Nope");
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(filterLayers(f.doc, "color:notacolour").ok());
    CHECK_FALSE(filterLayers(f.doc, "wibble:1").ok());
}

TEST_CASE("a layer state restores switches and look, ignores layers added later") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) { REQUIRE(saveLayerState(f.doc, tx, "Design").ok()); });
    CHECK(layerStateNames(f.doc) == std::vector<std::string>{"design"});

    f.edit([&](Transaction& tx) {
        REQUIRE(setOn(f.doc, tx, f.id("Walls"), false).ok());
        REQUIRE(setLocked(f.doc, tx, f.id("S-FLOOR~edge"), true).ok());
        REQUIRE(setLayerColor(f.doc, tx, f.id("Walls"), Color::index(3)).ok());
        REQUIRE(createLayer(f.doc, tx, "Later").ok());
        REQUIRE(setOn(f.doc, tx, f.id("Later"), false).ok());
    });

    std::size_t skipped = 99;
    f.edit([&](Transaction& tx) {
        REQUIRE(restoreLayerState(f.doc, tx, "design", {}, &skipped).ok());
    });
    CHECK(skipped == 0);
    CHECK(f.doc.layer(f.id("Walls"))->on);
    CHECK_FALSE(f.doc.layer(f.id("S-FLOOR~edge"))->locked);
    CHECK(f.doc.layer(f.id("Walls"))->color == Color::index(7));
    CHECK_FALSE(f.doc.layer(f.id("Later"))->on);  // not in the state: untouched
}

TEST_CASE("a layer state also saves the layers frozen in each layout viewport") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) { REQUIRE(layout::createLayout(tx, "Plan", "A3", true).ok()); });
    int vp1 = 0, vp2 = 0;
    f.edit([&](Transaction& tx) {
        vp1 = layout::addViewport(tx, "Plan", BBox2{{10, 10}, {100, 100}}, {0, 0}, 50).value();
        vp2 = layout::addViewport(tx, "Plan", BBox2{{110, 10}, {200, 100}}, {0, 0}, 50).value();
        REQUIRE(layout::setViewportLayerFrozen(tx, "Plan", vp1, f.id("Walls"), true).ok());
    });
    f.edit([&](Transaction& tx) { REQUIRE(saveLayerState(f.doc, tx, "Sheet").ok()); });
    // Swap the freezing between the two viewports.
    f.edit([&](Transaction& tx) {
        REQUIRE(layout::setViewportLayerFrozen(tx, "Plan", vp1, f.id("Walls"), false).ok());
        REQUIRE(layout::setViewportLayerFrozen(tx, "Plan", vp2, f.id("Walls"), true).ok());
        REQUIRE(layout::setViewportLayerFrozen(tx, "Plan", vp2, f.id("S-FLOOR"), true).ok());
    });
    std::size_t skipped = 99;
    f.edit([&](Transaction& tx) {
        REQUIRE(restoreLayerState(f.doc, tx, "sheet", {}, &skipped).ok());
    });
    CHECK(skipped == 0);
    auto lay = layout::findLayout(f.doc, "Plan");
    REQUIRE(lay);
    CHECK(lay->findViewport(vp1)->frozen == std::vector<LayerId>{f.id("Walls")});
    CHECK(lay->findViewport(vp2)->frozen.empty());
    // Only the Look part leaves the viewports alone.
    f.edit([&](Transaction& tx) {
        REQUIRE(layout::setViewportLayerFrozen(tx, "Plan", vp1, f.id("Walls"), false).ok());
    });
    f.edit([&](Transaction& tx) {
        StateParts look{false, false, false, true, false, false};
        REQUIRE(restoreLayerState(f.doc, tx, "sheet", look).ok());
    });
    CHECK(layout::findLayout(f.doc, "Plan")->findViewport(vp1)->frozen.empty());
}

TEST_CASE("restoring part of a state only touches that part, missing layers are counted") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) { REQUIRE(saveLayerState(f.doc, tx, "A").ok()); });
    f.edit([&](Transaction& tx) {
        REQUIRE(setOn(f.doc, tx, f.id("Walls"), false).ok());
        REQUIRE(setLayerColor(f.doc, tx, f.id("Walls"), Color::index(3)).ok());
        REQUIRE(deleteLayer(f.doc, tx, f.id("S-FLOOR~edge"), DeleteMode::LeafOnly).ok());
    });
    std::size_t skipped = 0;
    StateParts onlyVisibility{true, false, false, false};
    f.edit([&](Transaction& tx) {
        REQUIRE(restoreLayerState(f.doc, tx, "A", onlyVisibility, &skipped).ok());
    });
    CHECK(f.doc.layer(f.id("Walls"))->on);
    CHECK(f.doc.layer(f.id("Walls"))->color == Color::index(3));  // look not restored
    CHECK(skipped == 1);                                          // the deleted sub-layer
}

TEST_CASE("states: rename, delete, errors") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) {
        REQUIRE(saveLayerState(f.doc, tx, "One").ok());
        REQUIRE(saveLayerState(f.doc, tx, "Two").ok());
    });
    f.edit([&](Transaction& tx) {
        CHECK_FALSE(renameLayerState(f.doc, tx, "One", "two").ok());  // exists
        CHECK(renameLayerState(f.doc, tx, "One", "Uno").ok());
        CHECK_FALSE(deleteLayerState(f.doc, tx, "One").ok());
        CHECK_FALSE(saveLayerState(f.doc, tx, "  ").ok());
        CHECK_FALSE(restoreLayerState(f.doc, tx, "nothing").ok());
    });
    CHECK(layerStateNames(f.doc) == std::vector<std::string>{"two", "uno"});
    f.edit([&](Transaction& tx) { CHECK(deleteLayerState(f.doc, tx, "uno").ok()); });
    CHECK(layerStateNames(f.doc) == std::vector<std::string>{"two"});
}

TEST_CASE("states and filters are saved in .tbcad") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) {
        REQUIRE(setFrozen(f.doc, tx, f.id("Walls"), true).ok());
        REQUIRE(saveLayerState(f.doc, tx, "Frozen walls").ok());
        REQUIRE(saveLayerFilter(f.doc, tx, "Sub", "branch:S-FLOOR not:name:S-*").ok());
    });
    CHECK_FALSE(
        saveLayerFilter(f.doc, *std::make_unique<Transaction>(f.doc.begin("x")), "Bad", "wibble:1")
            .ok());

    auto dir = std::filesystem::temp_directory_path() / "tbcad_layer_state_test";
    std::filesystem::create_directories(dir);
    auto file = dir / "a.tbcad";
    REQUIRE(io::writeTbcad(f.doc, file).ok());
    io::IoReport report;
    auto back = io::readTbcad(file, report);
    REQUIRE(back.ok());
    CHECK(layerStateNames(*back.value()) == std::vector<std::string>{"frozen walls"});
    CHECK(layerFilterNames(*back.value()) == std::vector<std::string>{"sub"});
    auto expr = savedFilterExpression(*back.value(), "SUB");
    REQUIRE(expr.ok());
    CHECK(expr.value() == "branch:S-FLOOR not:name:S-*");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("-LAYERSTATE and -LAYERFILTER are registered and work from the command line") {
    Fixture f;
    CHECK(f.reg.find("-LAYERSTATE") != nullptr);
    CHECK(f.reg.find("-LAYERFILTER") != nullptr);
    f.build();

    auto r = f.host.run("-LAYERSTATE", {"Save", "Base", "List", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(layerStateNames(f.doc) == std::vector<std::string>{"base"});
    CHECK(f.host.logContains("base"));
    CHECK(f.host.undo());  // saving a state is one undo step
    CHECK(layerStateNames(f.doc).empty());
    f.host.run("-LAYERSTATE", {"Save", "Base", cmd::ScriptItem::enter()});

    f.edit([&](Transaction& tx) { REQUIRE(setOn(f.doc, tx, f.id("Walls"), false).ok()); });
    f.host.run("-LAYERSTATE", {"Restore", "Base", "All", cmd::ScriptItem::enter()});
    CHECK(f.doc.layer(f.id("Walls"))->on);

    f.host.run("-LAYERFILTER", {"Apply", "branch:S-FLOOR", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("3 layer(s) match"));
    CHECK(f.host.setting("layers.filter") == std::optional<std::string>("branch:S-FLOOR"));
    f.host.run("-LAYERFILTER",
               {"Save", "Floors", "branch:S-FLOOR", "Use", "floors", cmd::ScriptItem::enter()});
    CHECK(layerFilterNames(f.doc) == std::vector<std::string>{"floors"});
    f.host.run("-LAYERFILTER", {"Clear", cmd::ScriptItem::enter()});
    CHECK(f.host.setting("layers.filter") == std::optional<std::string>(""));
    f.host.run("-LAYERFILTER", {"Apply", "wibble:2", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("Unknown filter word"));
}

TEST_CASE(
    "group filters: pick layers by hand, edit, survive a rename or delete, filter by group:") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) {
        REQUIRE(saveLayerGroup(f.doc, tx, "Plan", {f.id("Walls"), f.id("S-FLOOR~edge")}).ok());
    });
    CHECK(layerGroupNames(f.doc) == std::vector<std::string>{"plan"});
    auto m = layerGroupMembers(f.doc, "PLAN");
    REQUIRE(m.ok());
    CHECK(m.value().size() == 2);
    CHECK(layerMatchesFilter(f.doc, f.id("Walls"), "group:Plan"));
    CHECK_FALSE(layerMatchesFilter(f.doc, f.id("S-FLOOR~hidden"), "group:Plan"));
    CHECK(layerMatchesFilter(f.doc, f.id("S-FLOOR~hidden"), "not:group:Plan"));
    std::string why;
    CHECK_FALSE(layerMatchesFilter(f.doc, f.id("Walls"), "group:Nope", &why));
    CHECK(why.find("No layer group") != std::string::npos);

    f.edit([&](Transaction& tx) {
        REQUIRE(addToLayerGroup(f.doc, tx, "Plan", {f.id("S-FLOOR~hidden")}).ok());
        REQUIRE(removeFromLayerGroup(f.doc, tx, "Plan", {f.id("Walls")}).ok());
    });
    CHECK(layerGroupMembers(f.doc, "Plan").value().size() == 2);
    CHECK_FALSE(layerMatchesFilter(f.doc, f.id("Walls"), "group:Plan"));

    // A deleted layer drops out of the group.
    f.edit([&](Transaction& tx) {
        REQUIRE(deleteLayer(f.doc, tx, f.id("S-FLOOR~hidden"), DeleteMode::LeafOnly).ok());
    });
    CHECK(layerGroupMembers(f.doc, "Plan").value().size() == 1);

    f.edit([&](Transaction& tx) { REQUIRE(deleteLayerGroup(f.doc, tx, "plan").ok()); });
    CHECK(layerGroupNames(f.doc).empty());
    f.edit([&](Transaction& tx) { CHECK_FALSE(deleteLayerGroup(f.doc, tx, "plan").ok()); });
}

TEST_CASE("a saved filter can use another saved filter and groups, circles are refused") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) {
        REQUIRE(saveLayerFilter(f.doc, tx, "floors", "branch:S-FLOOR").ok());
        REQUIRE(saveLayerFilter(f.doc, tx, "floorsOn", "filter:floors on").ok());
    });
    f.edit([&](Transaction& tx) { REQUIRE(setOn(f.doc, tx, f.id("S-FLOOR~hidden"), false).ok()); });
    auto ids = filterLayers(f.doc, "filter:floorsOn");
    REQUIRE(ids.ok());
    CHECK(ids.value().size() == 2);
    f.edit([&](Transaction& tx) {
        // Overwrite "floors" with one that points back at "floorsOn": a circle.
        REQUIRE(f.doc.header().custom.count("tbcad.layerfilter.floors") == 1);
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom["tbcad.layerfilter.floors"] = "filter:floorsOn";
                  }).ok());
    });
    CHECK_FALSE(filterLayers(f.doc, "filter:floorsOn").ok());
}

TEST_CASE("restoring a state brings back the current layer and can turn off new layers") {
    Fixture f;
    f.build();
    f.edit([&](Transaction& tx) { REQUIRE(setCurrentLayer(f.doc, tx, f.id("Walls")).ok()); });
    f.edit([&](Transaction& tx) { REQUIRE(saveLayerState(f.doc, tx, "base").ok()); });
    LayerId extra;
    f.edit([&](Transaction& tx) {
        extra = createLayer(f.doc, tx, "Extra").value();
        REQUIRE(setCurrentLayer(f.doc, tx, extra).ok());
    });
    f.edit([&](Transaction& tx) {
        StateParts p;
        REQUIRE(restoreLayerState(f.doc, tx, "base", p).ok());
    });
    CHECK(f.doc.header().currentLayer == f.id("Walls"));
    CHECK(f.doc.layer(extra)->on);  // layers added later are left alone by default
    f.edit([&](Transaction& tx) {
        StateParts p;
        p.hideOthers = true;
        REQUIRE(restoreLayerState(f.doc, tx, "base", p).ok());
    });
    CHECK_FALSE(f.doc.layer(extra)->on);
    CHECK(f.doc.layer(f.id("Walls"))->on);

    // Part restores leave the current layer alone.
    f.edit([&](Transaction& tx) { REQUIRE(setCurrentLayer(f.doc, tx, extra).ok()); });
    f.host.run("-LAYERSTATE", {"Restore", "base", "Lock", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == extra);
    f.host.run("-LAYERSTATE", {"Restore", "base", "HideOthers", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == f.id("Walls"));

    // Custom: any mix of parts, as the Layer States dialog sends them.
    f.edit([&](Transaction& tx) { REQUIRE(setCurrentLayer(f.doc, tx, extra).ok()); });
    f.host.run("-LAYERSTATE",
               {"Restore", "base", "Custom", "Lock, Plot", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == extra);
    f.host.run("-LAYERSTATE", {"Restore", "base", "Custom", "current", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLayer == f.id("Walls"));
}

TEST_CASE(
    "-LAYER Merge moves objects to the target and deletes the merged layers; undoes in one step") {
    Fixture f;
    f.build();
    EntityId a, b;
    f.edit([&](Transaction& tx) {
        Entity e{{}, LineData{{0, 0}, {1, 0}}};
        e.common.layer = f.id("Walls");
        a = tx.add(std::move(e)).value();
        Entity g{{}, LineData{{0, 0}, {2, 0}}};
        g.common.layer = f.id("S-FLOOR~edge");
        b = tx.add(std::move(g)).value();
    });
    f.host.run("-LAYER",
               {"Merge", "Walls,S-FLOOR~edge", "S-FLOOR~hidden", cmd::ScriptItem::enter()});
    CHECK(f.doc.find(a)->common.layer == f.id("S-FLOOR~hidden"));
    CHECK(f.doc.find(b)->common.layer == f.id("S-FLOOR~hidden"));
    CHECK_FALSE(resolveLayer(f.doc, "Walls").ok());
    CHECK(f.host.undo());
    CHECK(resolveLayer(f.doc, "Walls").ok());
    CHECK(f.doc.find(a)->common.layer == f.id("Walls"));
    f.host.clearLog();
    f.host.run("-LAYER", {"Merge", "0", "Walls", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("0"));  // layer 0 cannot be merged away
    CHECK(resolveLayer(f.doc, "0").ok());
}

TEST_CASE("-LAYERFILTER Group / AddTo / RemoveFrom / DeleteGroup from the command line") {
    Fixture f;
    f.build();
    f.host.run("-LAYERFILTER", {"Group", "Mine", "Walls,S-FLOOR", cmd::ScriptItem::enter()});
    CHECK(layerGroupMembers(f.doc, "Mine").value().size() == 2);
    f.host.run("-LAYERFILTER", {"AddTo", "Mine", "S-FLOOR~edge", "RemoveFrom", "Mine", "Walls",
                                cmd::ScriptItem::enter()});
    CHECK(layerGroupMembers(f.doc, "Mine").value().size() == 2);
    f.host.run("-LAYERFILTER", {"Apply", "group:Mine", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("2 layer(s) match"));
    f.host.clearLog();
    f.host.run("-LAYERFILTER", {"List", cmd::ScriptItem::enter()});
    CHECK(f.host.logContains("Layer groups: mine"));
    f.host.run("-LAYERFILTER", {"DeleteGroup", "Mine", cmd::ScriptItem::enter()});
    CHECK(layerGroupNames(f.doc).empty());
}
