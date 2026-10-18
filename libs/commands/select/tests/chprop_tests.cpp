#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select::test;

TEST_CASE("CHPROP changes the properties of the selection in one undo step") {
    Fixture f;
    const LayerId walls = f.addLayer("Walls");
    const LayerId north = f.addLayer("North", walls);
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    f.host.setSelection({a, b});
    f.host.run("CHPROP", {"Layer", "Walls"});
    CHECK(f.doc.find(a)->common.layer == walls);
    CHECK(f.doc.find(b)->common.layer == walls);
    f.host.setSelection({a});
    f.host.run("CH", {"Layer", "Walls/North"});
    CHECK(f.doc.find(a)->common.layer == north);
    CHECK(f.doc.find(b)->common.layer == walls);
    f.host.setSelection({a, b});
    f.host.run("CH", {"Color", "1"});
    CHECK(f.doc.find(b)->common.color == Color::index(1));
    f.host.run("CH", {"Lineweight", "0.5"});
    CHECK(f.doc.find(a)->common.lineweight == LineWeight::mm(0.5));
    f.host.run("CH", {"Scale", "2.5"});
    CHECK(f.doc.find(a)->common.linetypeScale == 2.5);
    f.host.undo();
    CHECK(f.doc.find(a)->common.linetypeScale == 1);
}

TEST_CASE("CHPROP refuses values that do not exist") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("CHPROP", {"Layer", "Nope"});
    CHECK(f.host.logContains("no layer named"));
    f.host.feed({cmd::ScriptItem::esc()});
    f.host.run("CHPROP", {"Color", "banana"});
    CHECK(f.host.logContains("not a colour"));
    f.host.feed({cmd::ScriptItem::esc()});
    f.host.run("CHPROP", {"Scale", "-1"});
    CHECK(f.host.logContains("positive"));
    f.host.feed({cmd::ScriptItem::esc()});
    CHECK(f.doc.find(a)->common.layer != LayerId{});
    CHECK(f.doc.find(a)->common.color == Color::byLayer());
}

TEST_CASE("QUICKPROPERTIES toggles the setting the application reads") {
    Fixture f;
    f.host.run("QP", {});
    CHECK(f.host.setting("ui.quickProps") == "1");
    f.host.run("QUICKPROPERTIES", {});
    CHECK(f.host.setting("ui.quickProps") == "0");
}

TEST_CASE("CHANGE moves the nearest line end, sets a radius, and can change properties") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(CircleData{{0, 20}, 5});
    f.host.setSelection({a});
    f.host.run("CHANGE", {"9,5"});
    const auto* l = entityAs<LineData>(*f.doc.find(a));
    CHECK(l->a.x == doctest::Approx(0));
    CHECK(l->b.x == doctest::Approx(9));
    CHECK(l->b.y == doctest::Approx(5));
    f.host.setSelection({b});
    f.host.run("CHANGE", {"0,28"});
    CHECK(entityAs<CircleData>(*f.doc.find(b))->radius == doctest::Approx(8));
    CHECK(f.host.undo());
    CHECK(entityAs<CircleData>(*f.doc.find(b))->radius == doctest::Approx(5));
    f.host.setSelection({a, b});
    f.host.run("CHANGE", {"Properties", "Color", "3"});
    CHECK(f.doc.find(b)->common.color == Color::index(3));
}

TEST_CASE("SELECTALL selects shown objects and skips those on off or locked layers") {
    Fixture f;
    const LayerId off = f.addLayer("Off");
    const LayerId locked = f.addLayer("Locked");
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    auto c = f.addEntity(LineData{{0, 1}, {10, 1}});
    auto d = f.addEntity(LineData{{0, 2}, {10, 2}});
    f.host.setSelection({a});
    f.host.run("CH", {"Layer", "Off"});  // a goes to Off
    f.host.setSelection({c});
    f.host.run("CH", {"Layer", "Locked"});
    {
        Transaction tx = f.doc.begin("layers");
        REQUIRE(tx.modifyLayer(off, [](Layer& l) { l.on = false; }));
        REQUIRE(tx.modifyLayer(locked, [](Layer& l) { l.locked = true; }));
        tx.commit();
    }
    f.host.setSelection({});
    REQUIRE(f.host.run("SELECTALL", {}).finished);
    const auto sel = f.host.selection();
    CHECK(sel.size() == 2);
    CHECK(std::find(sel.begin(), sel.end(), b) != sel.end());
    CHECK(std::find(sel.begin(), sel.end(), d) != sel.end());
}

TEST_CASE("SELECT leaves the picked objects selected for the next command") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    f.host.setSelection({});
    auto r = f.host.run("SELECT", {cmd::ScriptItem::pick(a), cmd::ScriptItem::enter()});
    CHECK(r.finished);
    const auto sel = f.host.selection();
    REQUIRE(sel.size() == 1);
    CHECK(sel[0] == a);
    (void)b;
}
