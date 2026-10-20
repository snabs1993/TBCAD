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

TEST_CASE("CHPROP leaves objects on locked layers alone") {
    Fixture f;
    const LayerId locked = f.addLayer("Locked");
    {
        Transaction tx = f.doc.begin("lock");
        REQUIRE(tx.modifyLayer(locked, [](Layer& l) { l.locked = true; }).ok());
        tx.commit();
    }
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {10, 1}}, locked);
    f.host.setSelection({a, b});
    f.host.run("CHPROP", {"Color", "1"});
    CHECK(f.doc.find(a)->common.color == Color::index(1));
    CHECK(f.doc.find(b)->common.color == Color::byLayer());
}

TEST_CASE("CHPROP changes transparency and the plot style") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(CircleData{{0, 0}, 5});
    f.host.setSelection({a, b});
    f.host.run("CHPROP", {"Transparency", "40"});
    CHECK(f.doc.find(a)->common.transparency == 40);
    CHECK(f.doc.find(b)->common.transparency == 40);
    f.host.setSelection({a, b});
    f.host.run("CHPROP", {"Transparency", "ByBlock"});
    CHECK(f.doc.find(a)->common.transparency == kTransparencyByBlock);
    f.host.setSelection({a, b});
    f.host.run("CHPROP", {"Transparency", "95"});
    CHECK(f.host.logContains("transparency"));
    f.host.feed({cmd::ScriptItem::esc()});
    CHECK(f.doc.find(a)->common.transparency == kTransparencyByBlock);
    f.host.setSelection({a});
    f.host.run("CHPROP", {"Plotstyle", "Thick"});
    CHECK(entityPlotStyle(f.doc.header(), a) == "Thick");
    CHECK(entityPlotStyle(f.doc.header(), b).empty());
    CHECK(f.host.undo());
    CHECK(entityPlotStyle(f.doc.header(), a).empty());
}

TEST_CASE("CHANGE moves ends and sets radii but not on locked layers") {
    Fixture f;
    const LayerId locked = f.addLayer("Locked");
    {
        Transaction tx = f.doc.begin("lock");
        REQUIRE(tx.modifyLayer(locked, [](Layer& l) { l.locked = true; }).ok());
        tx.commit();
    }
    auto a = f.addEntity(LineData{{0, 0}, {10, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {10, 1}}, locked);
    auto c = f.addEntity(CircleData{{0, 5}, 2});
    f.host.setSelection({a, b, c});
    f.host.run("CHANGE", {"12,3"});
    const auto* la = entityAs<LineData>(*f.doc.find(a));
    REQUIRE(la);
    CHECK(distance(la->b, {12, 3}) < 1e-9);
    CHECK(distance(entityAs<LineData>(*f.doc.find(b))->b, {10, 1}) < 1e-9);
    CHECK(entityAs<CircleData>(*f.doc.find(c))->radius ==
          doctest::Approx(distance({0, 5}, {12, 3})));
}
