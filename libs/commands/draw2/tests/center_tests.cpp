#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

TEST_CASE("CENTERMARK draws a cross that reaches past the circle") {
    Fixture f;
    auto circle = f.addEntity(CircleData{{10, 20}, 5});
    CHECK(f.host.run("CM", {ScriptItem::pick(circle, {15, 20}), ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& h = as<LineData>(es[1]);
    const auto& v = as<LineData>(es[2]);
    CHECK(close(h.a, {2.5, 20}));
    CHECK(close(h.b, {17.5, 20}));
    CHECK(close(v.a, {10, 12.5}));
    CHECK(close(v.b, {10, 27.5}));
}

TEST_CASE("CENTERMARK on an arc and a custom extension, several picks, one undo step") {
    Fixture f;
    auto arc = f.addEntity(ArcData{{0, 0}, 10, 0, kPi / 2});
    auto circle = f.addEntity(CircleData{{50, 0}, 4});
    f.host.run("CENTERMARK", {"Extension", "1", ScriptItem::pick(arc, {10, 0}),
                              ScriptItem::pick(circle, {54, 0}), ScriptItem::esc()});
    auto es = f.entities();
    REQUIRE(es.size() == 6);
    CHECK(close(as<LineData>(es[2]).a, {-11, 0}));
    CHECK(close(as<LineData>(es[2]).b, {11, 0}));
    CHECK(close(as<LineData>(es[4]).a, {45, 0}));
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE("CENTERMARK only accepts circles and arcs") {
    Fixture f;
    auto line = f.addEntity(LineData{{0, 0}, {1, 0}});
    f.host.run("CM", {});
    auto r = f.host.feed({ScriptItem::pick(line, {0, 0})});
    CHECK(r.rejected == 1);
    f.host.feed({ScriptItem::esc()});
}

TEST_CASE("CENTERLINE runs midway between two parallel lines over their overlap") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {100, 0}});
    auto b = f.addEntity(LineData{{20, 10}, {120, 10}});
    CHECK(f.host.run("CL", {ScriptItem::pick(a, {50, 0}), ScriptItem::pick(b, {50, 10})}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& l = as<LineData>(es[2]);
    CHECK(close(l.a, {17.5, 5}));
    CHECK(close(l.b, {102.5, 5}));
}

TEST_CASE("CENTERLINE between vertical lines with the line order reversed") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {0, 50}});
    auto b = f.addEntity(LineData{{-8, 50}, {-8, 0}});
    f.host.run("CL",
               {"Extension", "0", ScriptItem::pick(b, {-8, 10}), ScriptItem::pick(a, {0, 10})});
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& l = as<LineData>(es[2]);
    // Direction follows the first picked line (downwards).
    CHECK(close(l.a, {-4, 50}));
    CHECK(close(l.b, {-4, 0}));
}

TEST_CASE("CENTERLINE refuses lines that are not parallel or do not overlap") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {100, 0}});
    auto b = f.addEntity(LineData{{0, 10}, {100, 20}});
    auto c = f.addEntity(LineData{{200, 10}, {300, 10}});
    f.host.run("CL", {ScriptItem::pick(a, {50, 0}), ScriptItem::pick(b, {50, 15})});
    CHECK(f.host.logContains("must be parallel"));
    f.host.run("CL", {ScriptItem::pick(a, {50, 0}), ScriptItem::pick(c, {250, 10})});
    CHECK(f.host.logContains("do not overlap"));
    CHECK(f.entities().size() == 3);
}
