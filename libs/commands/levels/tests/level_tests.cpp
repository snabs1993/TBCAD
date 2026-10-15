#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/levels/level_ops.h"

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
    // A line on the current level, added outside any command.
    EntityId addLine(double y = 0) {
        auto tx = doc.begin("line");
        auto id = tx.add(Entity{{}, LineData{{0, y}, {10, y}}});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    LevelId level(const char* name) {
        auto r = levels::resolveLevel(doc, name);
        REQUIRE(r.ok());
        return r.value();
    }
};

}  // namespace

TEST_CASE("a new document has one level and objects land on the current level") {
    Fixture f;
    REQUIRE(f.doc.levels().size() == 1);
    CHECK(f.doc.header().currentLevel == kLevelDefault);
    EntityId a = f.addLine();
    CHECK(f.doc.find(a)->common.level == kLevelDefault);
}

TEST_CASE("-LEVEL New makes the level current - new objects go there, one undo step") {
    Fixture f;
    EntityId a = f.addLine();
    f.run("-LEVEL", {"New", "Level 2", "3000", cmd::ScriptItem::enter()});
    REQUIRE(f.doc.levels().size() == 2);
    LevelId l2 = f.level("level 2");  // names are case-insensitive
    CHECK(f.doc.level(l2)->elevation == 3000);
    CHECK(f.doc.header().currentLevel == l2);
    EntityId b = f.addLine();
    CHECK(f.doc.find(b)->common.level == l2);
    CHECK(f.doc.find(a)->common.level == kLevelDefault);
    CHECK(f.host.undoStack().undo(f.doc));
    CHECK(f.doc.levels().size() == 1);
    CHECK(f.doc.header().currentLevel == kLevelDefault);
}

TEST_CASE("objects on different levels may share the same position without interfering") {
    Fixture f;
    EntityId a = f.addLine();  // level 1
    auto tx = f.doc.begin("l2");
    LevelId l2 = levels::createLevel(f.doc, tx, "Level 2", 3000).value();
    REQUIRE(levels::setCurrentLevel(f.doc, tx, l2).ok());
    tx.commit();
    EntityId b = f.addLine();  // identical geometry, level 2

    // Dimmed view: the other level is locked + flagged, never selectable or snappable.
    EffectiveLayer ea = f.doc.effective(f.doc.find(a)->common);
    EffectiveLayer eb = f.doc.effective(f.doc.find(b)->common);
    CHECK(ea.visible());
    CHECK(ea.locked);
    CHECK(ea.otherLevel);
    CHECK_FALSE(ea.live());
    CHECK(eb.live());
    CHECK_FALSE(eb.locked);

    auto setView = [&](LevelView v) {
        auto t = f.doc.begin("view");
        REQUIRE(levels::setLevelView(f.doc, t, v).ok());
        t.commit();
    };
    setView(LevelView::HideOthers);
    CHECK_FALSE(f.doc.effective(f.doc.find(a)->common).visible());
    CHECK(f.doc.effective(f.doc.find(b)->common).visible());
    setView(LevelView::ShowAll);
    EffectiveLayer all = f.doc.effective(f.doc.find(a)->common);
    CHECK(all.visible());
    CHECK_FALSE(all.locked);
    CHECK(all.live());

    // A hidden level is hidden whatever the view; a locked level is locked.
    setView(LevelView::DimOthers);
    {
        auto t = f.doc.begin("hide");
        REQUIRE(levels::setVisible(f.doc, t, kLevelDefault, false).ok());
        t.commit();
    }
    CHECK_FALSE(f.doc.effective(f.doc.find(a)->common).visible());
    {
        auto t = f.doc.begin("show+lock");
        REQUIRE(levels::setVisible(f.doc, t, kLevelDefault, true).ok());
        REQUIRE(levels::setLocked(f.doc, t, l2, true).ok());
        t.commit();
    }
    CHECK(f.doc.effective(f.doc.find(b)->common).locked);
    CHECK_FALSE(f.doc.effective(f.doc.find(b)->common).otherLevel);
}

TEST_CASE("the current level cannot be hidden - setting a hidden level current shows it") {
    Fixture f;
    auto tx = f.doc.begin("t");
    LevelId l2 = levels::createLevel(f.doc, tx, "Roof", 6000).value();
    CHECK_FALSE(levels::setVisible(f.doc, tx, kLevelDefault, false).ok());
    REQUIRE(levels::setVisible(f.doc, tx, l2, false).ok());
    REQUIRE(levels::setCurrentLevel(f.doc, tx, l2).ok());
    tx.commit();
    CHECK(f.doc.level(l2)->visible);
}

TEST_CASE("level names are unique and the first level cannot be deleted") {
    Fixture f;
    auto tx = f.doc.begin("t");
    REQUIRE(levels::createLevel(f.doc, tx, "Roof", 6000).ok());
    CHECK_FALSE(levels::createLevel(f.doc, tx, "ROOF", 0).ok());
    CHECK_FALSE(levels::createLevel(f.doc, tx, "", 0).ok());
    CHECK_FALSE(levels::deleteLevel(f.doc, tx, kLevelDefault).ok());
    CHECK(levels::renameLevel(f.doc, tx, kLevelDefault, "Ground").ok());
    tx.commit();
    CHECK(f.doc.level(kLevelDefault)->name == "Ground");
}

TEST_CASE("-LEVEL Move and CHLEVEL move objects between levels - Delete moves them back") {
    Fixture f;
    EntityId a = f.addLine();
    EntityId b = f.addLine(5);
    f.run("-LEVEL", {"New", "Upper", "3000", cmd::ScriptItem::enter()});
    LevelId up = f.level("Upper");
    f.host.setSelection({a});
    auto r = f.run("CHLEVEL", {"Upper"});
    CHECK(f.doc.find(a)->common.level == up);
    CHECK(f.doc.find(b)->common.level == kLevelDefault);
    // Erasing a level that has objects directly is refused by the core.
    {
        auto tx = f.doc.begin("x");
        CHECK_FALSE(tx.eraseLevel(up).ok());
    }
    f.run("-LEVEL", {"Delete", "Upper", cmd::ScriptItem::enter()});
    CHECK(f.doc.level(up) == nullptr);
    CHECK(f.doc.find(a)->common.level == kLevelDefault);
    CHECK(f.doc.header().currentLevel == kLevelDefault);
    // One undo brings the level and the assignment back.
    CHECK(f.host.undoStack().undo(f.doc));
    CHECK(f.doc.level(up) != nullptr);
    CHECK(f.doc.find(a)->common.level == up);
    CHECK(f.doc.header().currentLevel == up);
}

TEST_CASE("LEVELUP and LEVELDOWN walk the levels by elevation") {
    Fixture f;
    f.run("-LEVEL", {"New", "L3", "6000", "New", "L2", "3000", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().currentLevel == f.level("L2"));
    f.run("LEVELUP", {});
    CHECK(f.doc.header().currentLevel == f.level("L3"));
    f.run("LEVELUP", {});  // already on top
    CHECK(f.doc.header().currentLevel == f.level("L3"));
    f.run("LEVELDOWN", {});
    f.run("LEVELDOWN", {});
    CHECK(f.doc.header().currentLevel == kLevelDefault);
}

TEST_CASE("LEVEL asks the application to show the panel - view option") {
    Fixture f;
    f.run("LEVEL", {});
    CHECK(f.host.setting("ui.showPanel") == "levels");
    f.run("-LEVEL", {"View", "hide", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().levelView == LevelView::HideOthers);
    CHECK(levels::parseLevelView("nonsense").ok() == false);
}

TEST_CASE("undo and redo of level edits keep the table and header in step") {
    Fixture f;
    f.run("-LEVEL", {"New", "A", "0", "New", "B", "3000", cmd::ScriptItem::enter()});
    CHECK(f.doc.levels().size() == 3);
    CHECK(f.host.undoStack().undo(f.doc));
    CHECK(f.doc.levels().size() == 1);
    CHECK(f.host.undoStack().redo(f.doc));
    CHECK(f.doc.levels().size() == 3);
    CHECK(f.doc.header().currentLevel == f.level("B"));
}

TEST_CASE("COPYTOLEVEL copies objects to another level at the same position") {
    Fixture f;
    EntityId a = f.addLine();
    f.run("-LEVEL", {"New", "Upper", "3000", cmd::ScriptItem::enter()});
    LevelId up = f.level("Upper");
    f.host.setSelection({a});
    f.run("COPYTOLEVEL", {"Upper"});
    int onUpper = 0, onFirst = 0;
    f.doc.forEachEntity([&](const Entity& e) { (e.common.level == up ? onUpper : onFirst)++; });
    CHECK(onUpper == 1);
    CHECK(onFirst == 1);
    CHECK(f.host.undoStack().undo(f.doc));
    CHECK(f.doc.find(a) != nullptr);
    onUpper = 0;
    f.doc.forEachEntity([&](const Entity& e) { onUpper += e.common.level == up; });
    CHECK(onUpper == 0);
}
