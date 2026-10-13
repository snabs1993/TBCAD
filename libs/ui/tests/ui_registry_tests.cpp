#include <doctest/doctest.h>

#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
using namespace tbcad::ui;

TEST_CASE("panels need a unique id and a draw function") {
    UiRegistry ui;
    int drawn = 0;
    CHECK(ui.addPanel("layers", "Layers", [&](PanelContext&) { ++drawn; }, true));
    CHECK_FALSE(ui.addPanel("layers", "Again", [](PanelContext&) {}));
    CHECK_FALSE(ui.addPanel("", "No id", [](PanelContext&) {}));
    CHECK_FALSE(ui.addPanel("x", "No fn", nullptr));
    REQUIRE(ui.panels().size() == 1);
    PanelContext pc;
    ui.panels()[0].draw(pc);
    CHECK(drawn == 1);
    CHECK(ui.panels()[0].visibleByDefault);
}

TEST_CASE("menu paths need a menu and a label") {
    UiRegistry ui;
    CHECK(ui.addMenuItem("Draw/Line", "LINE", "draw.line"));
    CHECK(ui.addMenuItem("Draw/Circle/Center, Radius", "CIRCLE"));
    CHECK_FALSE(ui.addMenuItem("Line", "LINE"));
    CHECK_FALSE(ui.addMenuItem("Draw/", "LINE"));
    CHECK_FALSE(ui.addMenuItem("/Line", "LINE"));
    CHECK_FALSE(ui.addMenuItem("Draw//Line", "LINE"));
    CHECK_FALSE(ui.addMenuItem("Draw/Line", ""));
    CHECK(ui.menuItems().size() == 2);
}

TEST_CASE("status toggles read and write through callbacks") {
    UiRegistry ui;
    bool on = false;
    CHECK(ui.addStatusToggle("snap", "SNAP", [&] { return on; }, [&](bool v) { on = v; }));
    CHECK_FALSE(ui.addStatusToggle("snap", "SNAP", [] { return false; }, [](bool) {}));
    ui.statusToggles()[0].set(true);
    CHECK(ui.statusToggles()[0].get());
    CHECK(on);
}

TEST_CASE("property sections are found by entity kind in registration order") {
    UiRegistry ui;
    std::string trace;
    ui.addPropertySection(EntityKind::Line, [&](PanelContext&, EntityId) { trace += "a"; });
    ui.addPropertySection(EntityKind::Circle, [&](PanelContext&, EntityId) { trace += "x"; });
    ui.addPropertySection(EntityKind::Line, [&](PanelContext&, EntityId) { trace += "b"; });
    PanelContext pc;
    for (auto* s : ui.sectionsFor(EntityKind::Line))
        s->draw(pc, EntityId{1});
    CHECK(trace == "ab");
    CHECK(ui.sectionsFor(EntityKind::Hatch).empty());
    ui.addOverlayDrawer(nullptr);
    CHECK(ui.overlayDrawers().empty());
}

TEST_CASE("menu items carry an action id and a default chord") {
    UiRegistry ui;
    CHECK(ui.addMenuItem("Draw/Line", "LINE", "draw.line", "L"));
    REQUIRE(ui.menuItems().size() == 1);
    CHECK(ui.menuItems()[0].actionId == "draw.line");
    CHECK(ui.menuItems()[0].defaultChord == "L");
}
