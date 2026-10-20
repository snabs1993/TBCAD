#include <doctest/doctest.h>

#include "tbcad/ui/status_items.h"
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

namespace {
struct NullFilter : cmd::PointFilter {
    std::optional<Vec2> filter(const cmd::PointQuery&, Vec2, cmd::FilterFeedback&) override {
        return std::nullopt;
    }
};
struct NullParser : cmd::CommandLineParser {
    std::optional<Vec2> parsePoint(std::string_view, const cmd::PointParseContext&) override {
        return std::nullopt;
    }
};
}  // namespace

TEST_CASE("point filters and the line parser are stored for AppCore to install") {
    UiRegistry ui;
    ui.addPointFilter(10, std::make_unique<NullFilter>());
    ui.addPointFilter(20, nullptr);  // ignored
    REQUIRE(ui.pointFilters().size() == 1);
    CHECK(ui.pointFilters()[0].order == 10);
    CHECK(ui.lineParser() == nullptr);
    NullParser p;
    ui.setLineParser(&p);
    CHECK(ui.lineParser() == &p);
}

TEST_CASE("underlay drawers are kept apart from overlay drawers") {
    UiRegistry ui;
    ui.addUnderlayDrawer(nullptr);
    CHECK(ui.underlayDrawers().empty());
    ui.addUnderlayDrawer([](PanelContext&, render::Overlay&) {});
    CHECK(ui.underlayDrawers().size() == 1);
    CHECK(ui.overlayDrawers().empty());
}

TEST_CASE("a contextual tab shows for a selection made only of its kinds") {
    UiRegistry ui;
    CHECK(ui.addContextualTab("text", "Text Editor", {EntityKind::Text, EntityKind::MText},
                              [](PanelContext&) {}));
    CHECK(ui.addContextualTab("hatch", "Hatch Editor", {EntityKind::Hatch}, [](PanelContext&) {}));
    CHECK_FALSE(ui.addContextualTab("text", "Again", {EntityKind::Line}, [](PanelContext&) {}));
    CHECK_FALSE(ui.addContextualTab("x", "No kinds", {}, [](PanelContext&) {}));
    CHECK_FALSE(ui.addContextualTab("y", "No draw", {EntityKind::Line}, nullptr));

    Document doc;
    Transaction tx = doc.begin("setup");
    auto add = [&](EntityData d) {
        Entity e;
        e.data = std::move(d);
        return tx.add(std::move(e)).value();
    };
    TextData t;
    t.utf8 = "A";
    t.height = 2.5;
    const EntityId text = add(Indirect<TextData>(t));
    MTextData m;
    m.utf8 = "B";
    m.height = 2.5;
    const EntityId mtext = add(Indirect<MTextData>(m));
    const EntityId line = add(LineData{{0, 0}, {1, 0}});
    tx.commit();

    CHECK(ui.contextualTabFor(doc, {}) == nullptr);
    const ContextualTab* tab = ui.contextualTabFor(doc, {text, mtext});
    REQUIRE(tab);
    CHECK(tab->id == "text");
    CHECK(ui.contextualTabFor(doc, {text, line}) == nullptr);  // mixed
    CHECK(ui.contextualTabFor(doc, {line}) == nullptr);
    CHECK(ui.contextualTabFor(doc, {EntityId{99999}}) == nullptr);  // gone
}

TEST_CASE("status bar items are hidden and shown through one setting") {
    std::string s;
    CHECK(statusItemShown(s, "grid"));
    s = setStatusItemShown(s, "grid", false);
    s = setStatusItemShown(s, "coords", false);
    s = setStatusItemShown(s, "grid", false);  // once
    CHECK(s == "grid,coords");
    CHECK_FALSE(statusItemShown(s, "grid"));
    CHECK(statusItemShown(s, "snap"));
    s = setStatusItemShown(s, "grid", true);
    CHECK(s == "coords");
    CHECK(statusHiddenIds(" a, ,b ") == std::vector<std::string>{"a", "b"});
}
