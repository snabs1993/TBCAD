#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select;
using namespace tbcad::select::test;

TEST_CASE("queryEntities by type") {
    Fixture f;
    auto l = f.addEntity(LineData{{0, 0}, {1, 1}});
    auto c = f.addEntity(CircleData{{0, 0}, 2});
    auto r = queryEntities(f.doc, {Property::Type, "circle", false});
    REQUIRE(r);
    CHECK(r->size() == 1);
    CHECK(has(*r, c));
    auto n = queryEntities(f.doc, {Property::Type, "Circle", true});
    REQUIRE(n);
    CHECK(has(*n, l));
    CHECK_FALSE(queryEntities(f.doc, {Property::Type, "banana", false}));
}

TEST_CASE("queryEntities by layer ignores case") {
    Fixture f;
    LayerId walls = f.addLayer("Walls");
    auto a = f.addEntity(LineData{{0, 0}, {1, 1}}, walls);
    f.addEntity(LineData{{0, 0}, {2, 2}});
    auto r = queryEntities(f.doc, {Property::Layer, "WALLS", false});
    REQUIRE(r);
    CHECK(r->size() == 1);
    CHECK(has(*r, a));
}

TEST_CASE("queryEntities by colour compares what is drawn") {
    Fixture f;
    LayerId red = f.addLayer("Red");
    {
        Transaction tx = f.doc.begin("setup");
        REQUIRE(tx.modifyLayer(red, [](Layer& l) { l.color = Color::index(1); }).ok());
        tx.commit();
    }
    auto byLayer = f.addEntity(LineData{{0, 0}, {1, 0}}, red);  // red through its layer
    auto own = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.modify(own, [](Entity& e) { e.common.color = Color::index(1); });
    f.addEntity(LineData{{0, 2}, {1, 2}});  // layer 0, white
    auto r = queryEntities(f.doc, {Property::Color, "1", false});
    REQUIRE(r);
    CHECK(r->size() == 2);
    CHECK(has(*r, byLayer));
    auto bl = queryEntities(f.doc, {Property::Color, "ByLayer", false});
    REQUIRE(bl);
    CHECK(bl->size() == 2);  // the two that do not set their own colour
    CHECK(queryEntities(f.doc, {Property::Color, "10,20,30", false}));
    CHECK_FALSE(queryEntities(f.doc, {Property::Color, "300", false}));
    CHECK_FALSE(queryEntities(f.doc, {Property::Color, "red", false}));
}

TEST_CASE("queryEntities by linetype and lineweight") {
    Fixture f;
    LinetypeId dashed;
    {
        Transaction tx = f.doc.begin("setup");
        Linetype lt;
        lt.name = "DASHED";
        lt.pattern = {5, -2};
        dashed = tx.addLinetype(lt).value();
        tx.commit();
    }
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    f.modify(a, [&](Entity& e) {
        e.common.linetype = dashed;
        e.common.lineweight = LineWeight::mm(0.5);
    });
    f.addEntity(LineData{{0, 1}, {1, 1}});
    auto lt = queryEntities(f.doc, {Property::Linetype, "dashed", false});
    REQUIRE(lt);
    CHECK(lt->size() == 1);
    CHECK(has(*lt, a));
    auto lw = queryEntities(f.doc, {Property::Lineweight, "0.5", false});
    REQUIRE(lw);
    CHECK(lw->size() == 1);
    CHECK_FALSE(queryEntities(f.doc, {Property::Lineweight, "thick", false}));
}

TEST_CASE("queryEntities skips hidden objects and objects on frozen layers") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.modify(b, [](Entity& e) { e.common.visible = false; });
    LayerId frozen = f.addLayer("Frozen");
    f.addEntity(LineData{{0, 2}, {1, 2}}, frozen);
    {
        Transaction tx = f.doc.begin("setup");
        REQUIRE(tx.modifyLayer(frozen, [](Layer& l) { l.frozen = true; }).ok());
        tx.commit();
    }
    auto r = queryEntities(f.doc, {Property::Type, "line", false});
    REQUIRE(r);
    CHECK(r->size() == 1);
    CHECK(has(*r, a));
}

TEST_CASE("QSELECT replaces the selection and reports bad values") {
    Fixture f;
    auto a = f.addEntity(CircleData{{0, 0}, 1});
    f.addEntity(LineData{{0, 0}, {1, 1}});
    f.host.run("QSELECT", {"Type", "circle", "", ""});
    CHECK(f.host.logContains("1 object(s) found."));
    REQUIRE(f.host.selection().size() == 1);
    CHECK(f.host.selection()[0] == a);
    CHECK_FALSE(f.host.undoStack().canUndo());

    f.host.run("QSEL", {"Type", "banana", "", ""});
    CHECK(f.host.logContains("not a valid Type value"));
}

TEST_CASE("QSELECT Not and Append") {
    Fixture f;
    auto c = f.addEntity(CircleData{{0, 0}, 1});
    auto l = f.addEntity(LineData{{0, 0}, {1, 1}});
    f.host.run("QSELECT", {"Type", "circle", "", ""});
    f.host.run("QSELECT", {"Type", "circle", "Not", "Append"});
    auto sel = f.host.selection();
    CHECK(sel.size() == 2);
    CHECK(has(sel, c));
    CHECK(has(sel, l));
}

TEST_CASE("queryEntities by level also finds the dimmed objects of other levels") {
    Fixture f;
    auto ground = f.addEntity(LineData{{0, 0}, {1, 1}});
    LevelId upper;
    {
        Transaction tx = f.doc.begin("level");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = upper; }).ok());
        tx.commit();
    }
    auto top = f.addEntity(LineData{{0, 0}, {2, 2}});
    auto onUpper = queryEntities(f.doc, {Property::Level, "upper", false});
    REQUIRE(onUpper);
    CHECK(*onUpper == std::vector<EntityId>{top});
    auto onFirst = queryEntities(f.doc, {Property::Level, "Level 1", false});
    REQUIRE(onFirst);
    CHECK(*onFirst == std::vector<EntityId>{ground});  // dimmed, but still found by name
    auto notUpper = queryEntities(f.doc, {Property::Level, "Upper", true});
    REQUIRE(notUpper);
    CHECK(*notUpper == std::vector<EntityId>{ground});
}
