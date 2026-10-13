#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"

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
