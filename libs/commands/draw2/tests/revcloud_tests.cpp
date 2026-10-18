#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

namespace {
const double kBulge = std::tan(kPi / 6);
}

TEST_CASE("REVCLOUD from points divides each edge into arcs no longer than the arc length") {
    Fixture f;
    CHECK(f.host.run("REVC", {"Arclength", "25", "0,0", "100,0", "100,50", "0,50", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    REQUIRE(pl.v.size() == 12);
    CHECK(close(pl.v[0].p, {0, 0}));
    CHECK(close(pl.v[1].p, {25, 0}));
    CHECK(close(pl.v[4].p, {100, 0}));
    CHECK(close(pl.v[6].p, {100, 50}));
    CHECK(close(pl.v[7].p, {75, 50}));
    CHECK(close(pl.v[10].p, {0, 50}));
    CHECK(close(pl.v[11].p, {0, 25}));
    for (const auto& v : pl.v)
        CHECK(close(v.bulge, kBulge));  // counter-clockwise input: positive bulge is outwards
}

TEST_CASE("REVCLOUD bulges outwards for clockwise input too") {
    Fixture f;
    f.host.run("REVCLOUD", {"A", "50", "0,0", "0,50", "100,50", "100,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    for (const auto& v : as<PolylineData>(es[0]).v)
        CHECK(close(v.bulge, -kBulge));
}

TEST_CASE("REVCLOUD remembers the arc length") {
    Fixture f;
    f.host.run("REVC", {"A", "50", ScriptItem::esc()});
    f.host.run("REVC", {"0,0", "100,0", "100,50", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    // Edges 100, 50 and 111.8 long at 50: 2 + 1 + 3 pieces.
    CHECK(as<PolylineData>(es[0]).v.size() == 6);
}

TEST_CASE("REVCLOUD from a rectangle object") {
    Fixture f;
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}}) {
        PolyVertex v;
        v.p = p;
        box.v.push_back(v);
    }
    auto rect = f.addEntity(box);
    f.host.run("REVC", {"A", "25", "Object", ScriptItem::pick(rect, {0, 0}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);  // the rectangle stays
    const auto& pl = as<PolylineData>(es[1]);
    REQUIRE(pl.v.size() == 12);
    CHECK(close(pl.v[1].p, {25, 0}));
    CHECK(close(pl.v[0].bulge, kBulge));
}

TEST_CASE("REVCLOUD Reverse turns the arcs inwards") {
    Fixture f;
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}}) {
        PolyVertex v;
        v.p = p;
        box.v.push_back(v);
    }
    auto rect = f.addEntity(box);
    f.host.run("REVC", {"A", "50", "O", ScriptItem::pick(rect, {0, 0}), "Yes"});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(close(as<PolylineData>(es[1]).v[0].bulge, -kBulge));
}

TEST_CASE("REVCLOUD from a circle spaces the vertices evenly on it") {
    Fixture f;
    auto circle = f.addEntity(CircleData{{5, 5}, 10});
    f.host.run("REVC", {"A", "5", "O", ScriptItem::pick(circle, {15, 5}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& pl = as<PolylineData>(es[1]);
    REQUIRE(pl.v.size() == 13);  // ceil(62.83 / 5)
    for (const auto& v : pl.v)
        CHECK(close(distance(v.p, {5, 5}), 10, 0.05));
}

TEST_CASE("REVCLOUD object mode refuses open or unsuitable objects") {
    Fixture f;
    auto line = f.addEntity(LineData{{0, 0}, {10, 0}});
    f.host.run("REVC", {"O"});
    auto r = f.host.feed({ScriptItem::pick(line, {5, 0})});
    CHECK(r.rejected == 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("REVCLOUD with fewer than three points or no area draws nothing") {
    Fixture f;
    f.host.run("REVC", {"0,0", "10,0", ""});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("at least three points"));
    f.host.run("REVC", {"0,0", "10,0", "20,0", ""});
    CHECK(f.entities().empty());
}

TEST_CASE("REVCLOUD: Esc draws nothing - Undo drops the last point - one undo step") {
    Fixture f;
    f.host.run("REVC", {"0,0", "100,0", "100,50", ScriptItem::esc()});
    CHECK(f.entities().empty());
    f.host.run("REVC", {"0,0", "100,0", "100,50", "5,5", "Undo", ""});
    REQUIRE(f.entities().size() == 1);
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE(
    "REVCLOUD Rectangular draws a cloud around two corners and Style Calligraphy tapers the arcs") {
    Fixture f;
    CHECK(f.host.run("REVCLOUD", {"Rectangular", "0,0", "40,20"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    CHECK(pl.v.size() >= 12);
    CHECK(close(pl.v[0].startWidth, 0));

    CHECK(f.host.run("REVCLOUD", {"Style", "Calligraphy", "Rectangular", "0,0", "40,20"}).finished);
    es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& cal = as<PolylineData>(es[1]);
    CHECK(cal.v[0].startWidth > cal.v[0].endWidth);
    CHECK(cal.v[0].endWidth > 0);
}
