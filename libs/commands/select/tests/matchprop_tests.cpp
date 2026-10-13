#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select::test;
using cmd::ScriptItem;

TEST_CASE("MATCHPROP copies layer, colour, linetype and lineweight") {
    Fixture f;
    const LayerId red = f.addLayer("Red");
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}}, red);
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    f.modify(a, [](Entity& e) {
        e.common.color = Color::index(1);
        e.common.lineweight = LineWeight::hundredths(50);
        e.common.linetypeScale = 2;
    });
    f.host.run("MA", {ScriptItem::pick(a, {1, 0}), ScriptItem::pick(b, {5, 0}), ScriptItem::esc()});
    const Entity* nb = f.doc.find(b);
    CHECK(nb->common.layer == red);
    CHECK(nb->common.color == Color::index(1));
    CHECK(nb->common.lineweight == LineWeight::hundredths(50));
    CHECK(nb->common.linetypeScale == 2);
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.doc.find(b)->common.layer != red);
}

TEST_CASE("MATCHPROP Settings turns properties off") {
    Fixture f;
    const LayerId red = f.addLayer("Red");
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}}, red);
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    f.modify(a, [](Entity& e) { e.common.color = Color::index(1); });
    f.host.run("MA", {ScriptItem::pick(a, {1, 0}), "Settings", "Layer", "Done",
                      ScriptItem::pick(b, {5, 0}), ScriptItem::esc()});
    CHECK(f.doc.find(b)->common.layer != red);
    CHECK(f.doc.find(b)->common.color == Color::index(1));
}
