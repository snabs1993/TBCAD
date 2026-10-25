#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/standards.h"

using namespace tbcad;
using layers::StandardsFix;
using layers::StandardsKind;

namespace {

const layers::StandardsViolation* find(const std::vector<layers::StandardsViolation>& v,
                                       StandardsKind k, const std::string& name) {
    for (const auto& x : v)
        if (x.kind == k && x.name == name)
            return &x;
    return nullptr;
}

// A standard with layers A-WALL (red) and A-DOOR (green), text style Notes (width 0.8) and
// dimension style Plan (text height 3.5).
void makeStandard(Document& std_) {
    auto tx = std_.begin("std");
    Layer wall;
    wall.name = "A-WALL";
    wall.color = Color::index(1);
    (void)tx.addLayer(wall);
    Layer door;
    door.name = "A-DOOR";
    door.color = Color::index(3);
    (void)tx.addLayer(door);
    TextStyle notes;
    notes.name = "Notes";
    notes.widthFactor = 0.8;
    (void)tx.addTextStyle(notes);
    DimStyle plan;
    plan.name = "Plan";
    plan.textHeight = 3.5;
    (void)tx.addDimStyle(plan);
    tx.commit();
}

}  // namespace

TEST_CASE("checkStandards finds renamed, recoloured, unknown and style differences") {
    Document std_;
    makeStandard(std_);
    Document doc;
    EntityId onWall, onStray;
    {
        auto tx = doc.begin("setup");
        Layer wall;
        wall.name = "a-wall";  // case differs, colour differs
        wall.color = Color::index(2);
        auto wallId = tx.addLayer(wall).value();
        Layer door;
        door.name = "A-DOOR";  // right
        door.color = Color::index(3);
        (void)tx.addLayer(door);
        Layer stray;
        stray.name = "Stray";
        stray.color = Color::index(1);  // looks like A-WALL
        auto strayId = tx.addLayer(stray).value();
        Entity a{{}, LineData{{0, 0}, {5, 0}}};
        a.common.layer = wallId;
        onWall = tx.add(a).value();
        Entity b{{}, LineData{{0, 1}, {5, 1}}};
        b.common.layer = strayId;
        onStray = tx.add(b).value();
        TextStyle notes;
        notes.name = "Notes";
        (void)tx.addTextStyle(notes);  // width 1: differs
        TextStyle other;
        other.name = "Other";
        (void)tx.addTextStyle(other);
        DimStyle plan;
        plan.name = "Plan";  // text height 2.5: differs
        (void)tx.addDimStyle(plan);
        tx.commit();
    }
    const auto found = layers::checkStandards(doc, std_);
    const auto* wall = find(found, StandardsKind::Layer, "a-wall");
    REQUIRE(wall);
    CHECK(wall->fix == StandardsFix::Rename);
    CHECK(wall->target == "A-WALL");
    CHECK(find(found, StandardsKind::Layer, "A-DOOR") == nullptr);
    const auto* stray = find(found, StandardsKind::Layer, "Stray");
    REQUIRE(stray);
    CHECK(stray->fix == StandardsFix::Replace);
    CHECK(stray->target == "A-WALL");
    CHECK(find(found, StandardsKind::TextStyle, "Notes"));
    CHECK(find(found, StandardsKind::TextStyle, "Other")->fix == StandardsFix::Replace);
    CHECK(find(found, StandardsKind::DimStyle, "Plan")->fix == StandardsFix::Update);

    auto tx = doc.begin("fix");
    const auto r = layers::fixStandards(doc, std_, tx, found);
    tx.commit();
    CHECK(r.failed.empty());
    CHECK(r.fixed == static_cast<int>(found.size()));
    const Layer* fixed = doc.layerByName("A-WALL");
    REQUIRE(fixed);
    CHECK(fixed->color == Color::index(1));
    CHECK(doc.find(onWall)->common.layer == fixed->id);
    CHECK(doc.find(onStray)->common.layer == fixed->id);  // merged into the look-alike
    CHECK(doc.layerByName("Stray") == nullptr);
    CHECK(doc.textStyleByName("Notes")->widthFactor == doctest::Approx(0.8));
    CHECK(doc.textStyleByName("Other") == nullptr);
    CHECK(doc.dimStyleByName("Plan")->textHeight == doctest::Approx(3.5));
    CHECK(layers::checkStandards(doc, std_).empty());
}

TEST_CASE("CHECKSTANDARDS lists violations and _FIXSTANDARDS fixes them in one undo step") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_standards";
    fs::create_directories(dir);
    Document std_;
    makeStandard(std_);
    const std::string file = (dir / "office.tbcad").string();
    REQUIRE(io::writeTbcad(std_, file));

    Document doc;
    {
        auto tx = doc.begin("setup");
        Layer stray;
        stray.name = "Stray";
        (void)tx.addLayer(stray);
        tx.commit();
    }
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host{doc, reg};
    CHECK(host.run("CHECKSTANDARDS", {file}).finished);
    CHECK(host.logContains("Stray: Layer is not in the standard"));
    CHECK(host.logContains("1 violation"));
    host.setSetting("tmp.standards.keys", "*");
    CHECK(host.run("_FIXSTANDARDS", {}).finished);
    CHECK(doc.layerByName("Stray") == nullptr);
    CHECK(host.logContains("Fixed 1 violation"));
    CHECK(host.undo());
    CHECK(doc.layerByName("Stray") != nullptr);
    CHECK(host.run("CHECKSTANDARDS", {""}).finished);  // the file is remembered: Enter takes it
    CHECK(host.logContains("1 violation"));
    fs::remove_all(dir);
}
