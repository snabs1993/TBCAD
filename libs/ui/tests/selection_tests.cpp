#include <doctest/doctest.h>

#include "tbcad/hatch/hatch.h"
#include "tbcad/ui/selection.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

TEST_CASE("pick returns the nearest entity within the aperture") {
    Indexed d;
    EntityId near = addLine(d.doc, {0, 0}, {100, 0});
    EntityId far = addLine(d.doc, {0, 10}, {100, 10});
    CHECK(pickEntity(d.doc, d.idx, {50, 2}, 5) == near);
    CHECK(pickEntity(d.doc, d.idx, {50, 8}, 5) == far);
    CHECK_FALSE(pickEntity(d.doc, d.idx, {50, 40}, 5).has_value());
}

TEST_CASE("pick prefers the entity drawn on top on a tie") {
    Indexed d;
    addLine(d.doc, {0, 0}, {100, 0});
    EntityId top = addLine(d.doc, {0, 0}, {100, 0});
    CHECK(pickEntity(d.doc, d.idx, {50, 0}, 3) == top);
}

TEST_CASE("window selects what is fully inside, crossing what touches") {
    Indexed d;
    EntityId inside = addLine(d.doc, {10, 10}, {20, 20});
    EntityId crossing = addLine(d.doc, {50, 50}, {150, 50});
    EntityId outside = addLine(d.doc, {200, 200}, {300, 300});
    BBox2 box;
    box.expand({0, 0});
    box.expand({100, 100});
    CHECK(selectInBox(d.doc, d.idx, box, false) == std::vector<EntityId>{inside});
    CHECK(selectInBox(d.doc, d.idx, box, true) == std::vector<EntityId>{inside, crossing});
    (void)outside;
    CHECK(dragIsCrossing(100, 20));
    CHECK_FALSE(dragIsCrossing(20, 100));
}

TEST_CASE("crossing catches a line whose box overlaps the window but whose body does not") {
    Indexed d;
    addLine(d.doc, {0, 100}, {100, 0});  // diagonal; its box covers the whole square
    BBox2 box;
    box.expand({0, 0});
    box.expand({20, 20});  // bottom-left corner, far from the diagonal
    CHECK(selectInBox(d.doc, d.idx, box, true).empty());
}

TEST_CASE("off, frozen and locked layers are not selectable") {
    Indexed d;
    LayerId on = addLayer(d.doc, "on");
    LayerId off = addLayer(d.doc, "off");
    LayerId frozen = addLayer(d.doc, "frozen");
    LayerId locked = addLayer(d.doc, "locked");
    modifyLayer(d.doc, off, [](Layer& l) { l.on = false; });
    modifyLayer(d.doc, frozen, [](Layer& l) { l.frozen = true; });
    modifyLayer(d.doc, locked, [](Layer& l) { l.locked = true; });
    EntityId a = addLine(d.doc, {0, 0}, {10, 0}, on);
    EntityId b = addLine(d.doc, {0, 1}, {10, 1}, off);
    EntityId c = addLine(d.doc, {0, 2}, {10, 2}, frozen);
    EntityId e = addLine(d.doc, {0, 3}, {10, 3}, locked);
    BBox2 all;
    all.expand({-5, -5});
    all.expand({20, 20});
    CHECK(selectInBox(d.doc, d.idx, all, true) == std::vector<EntityId>{a});
    CHECK_FALSE(pickEntity(d.doc, d.idx, {5, 1}, 0.4).has_value());
    CHECK_FALSE(pickEntity(d.doc, d.idx, {5, 3}, 0.4).has_value());
    // Locked entities are still drawn; off and frozen ones are not.
    CHECK(isVisible(d.doc, *d.doc.find(e)));
    CHECK_FALSE(isVisible(d.doc, *d.doc.find(b)));
    CHECK_FALSE(isVisible(d.doc, *d.doc.find(c)));
}

TEST_CASE("sub-layers follow the effective state of their parents") {
    Indexed d;
    LayerId parent = addLayer(d.doc, "S-FLOOR");
    LayerId child = addLayer(d.doc, "edge", parent);
    EntityId e = addLine(d.doc, {0, 0}, {10, 0}, child);
    CHECK(pickEntity(d.doc, d.idx, {5, 0}, 1) == e);

    modifyLayer(d.doc, parent, [](Layer& l) { l.frozen = true; });
    CHECK_FALSE(d.doc.layer(child)->frozen);  // the child's own flag is untouched ...
    CHECK_FALSE(pickEntity(d.doc, d.idx, {5, 0}, 1).has_value());  // ... but it is frozen in effect
    CHECK_FALSE(isVisible(d.doc, *d.doc.find(e)));

    modifyLayer(d.doc, parent, [](Layer& l) {
        l.frozen = false;
        l.locked = true;
    });
    CHECK(isVisible(d.doc, *d.doc.find(e)));
    CHECK_FALSE(pickEntity(d.doc, d.idx, {5, 0}, 1).has_value());

    modifyLayer(d.doc, parent, [](Layer& l) {
        l.locked = false;
        l.on = false;
    });
    CHECK_FALSE(isVisible(d.doc, *d.doc.find(e)));
}

TEST_CASE("entities with the visible flag off are not selectable") {
    Indexed d;
    EntityId e = addLine(d.doc, {0, 0}, {10, 0});
    {
        auto tx = d.doc.begin("hide");
        (void)tx.modify(e, [](Entity& en) { en.common.visible = false; });
        tx.commit();
    }
    CHECK_FALSE(pickEntity(d.doc, d.idx, {5, 0}, 1).has_value());
}

TEST_CASE("objects on another level cannot be picked, even at the same position") {
    Indexed d;
    EntityId ground = addLine(d.doc, {0, 0}, {100, 0});
    LevelId upper;
    {
        auto tx = d.doc.begin("level");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = upper; }).ok());
        tx.commit();
    }
    EntityId top = addLine(d.doc, {0, 0}, {100, 0});  // same position, current level
    CHECK(d.doc.find(top)->common.level == upper);
    CHECK(pickEntity(d.doc, d.idx, {50, 0}, 3) == top);
    CHECK_FALSE(isSelectable(d.doc, *d.doc.find(ground)));
    BBox2 box;
    box.expand({-10, -10});
    box.expand({110, 10});
    CHECK(selectInBox(d.doc, d.idx, box, true) == std::vector<EntityId>{top});

    // Switch back: now only the first level answers.
    {
        auto tx = d.doc.begin("back");
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = kLevelDefault; }).ok());
        tx.commit();
    }
    CHECK(pickEntity(d.doc, d.idx, {50, 0}, 3) == ground);
    // Showing all levels makes both pickable again (the one on top wins the tie).
    {
        auto tx = d.doc.begin("all");
        REQUIRE(tx.modifyHeader([&](Header& h) { h.levelView = LevelView::ShowAll; }).ok());
        tx.commit();
    }
    CHECK(pickEntity(d.doc, d.idx, {50, 0}, 3) == top);
}

TEST_CASE("pickAllAt lists the overlapping entities nearest first and cycling walks them") {
    Indexed d;
    EntityId a = addLine(d.doc, {0, 0}, {100, 0});
    EntityId b = addLine(d.doc, {0, 2}, {100, 2});
    EntityId c = addLine(d.doc, {0, 0}, {100, 0});
    auto stack = pickAllAt(d.doc, d.idx, {50, 0.5}, 5);
    CHECK(stack == std::vector<EntityId>{c, a, b});  // a and c tie: the one on top comes first
    CHECK(pickAllAt(d.doc, d.idx, {50, 40}, 5).empty());
    CHECK(cycleNext(stack, std::nullopt) == c);
    CHECK(cycleNext(stack, c) == a);
    CHECK(cycleNext(stack, b) == c);  // wraps
    CHECK(cycleNext(stack, EntityId{9999}) == c);
    CHECK_FALSE(cycleNext({}, c).has_value());
}

TEST_CASE("lasso selects inside a concave polygon - crossing also takes what it touches") {
    Indexed d;
    EntityId inside = addLine(d.doc, {10, 10}, {20, 20});
    EntityId crossing = addLine(d.doc, {50, 50}, {150, 50});
    EntityId notch = addLine(d.doc, {60, 80}, {90, 80});  // in the bounding box, outside the lasso
    EntityId outside = addLine(d.doc, {200, 200}, {300, 300});
    // An L shape: the upper right quadrant is cut away.
    std::vector<Vec2> lasso{{0, 0}, {100, 0}, {100, 60}, {50, 60}, {50, 100}, {0, 100}};
    CHECK(selectInPolygon(d.doc, d.idx, lasso, false) == std::vector<EntityId>{inside});
    CHECK(selectInPolygon(d.doc, d.idx, lasso, true) == std::vector<EntityId>{inside, crossing});
    CHECK(selectInPolygon(d.doc, d.idx, {{0, 0}, {10, 10}}, true).empty());
    (void)notch;
    (void)outside;
}

TEST_CASE("a click inside a hatch picks it, but a line under the pointer wins") {
    hatch::registerHatchDisplay();
    Indexed d;
    HatchData h;
    HatchLoop loop;
    HatchPolylineEdge pe;
    for (Vec2 q : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 100}, Vec2{0, 100}})
        pe.v.push_back(PolyVertex{q});
    pe.closed = true;
    loop.edges.push_back(pe);
    h.loops.push_back(loop);
    h.solid = false;
    h.pattern = "ANSI31";
    EntityId hatch;
    {
        auto tx = d.doc.begin("hatch");
        hatch = tx.add(Entity{{}, h}).value();
        tx.commit();
    }
    EntityId line = addLine(d.doc, {0, 50}, {100, 50});
    CHECK(pickEntity(d.doc, d.idx, {30, 20}, 1) == hatch);
    CHECK(pickEntity(d.doc, d.idx, {30, 50}, 1) == line);
    CHECK_FALSE(pickEntity(d.doc, d.idx, {130, 20}, 1).has_value());
}
