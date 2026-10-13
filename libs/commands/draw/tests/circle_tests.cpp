#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

TEST_CASE("CIRCLE centre and radius") {
    Fixture f;
    CHECK(f.host.run("C", {"10,20", "5"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).center, {10, 20}));
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE radius from a picked point") {
    Fixture f;
    f.host.run("CIRCLE", {"0,0", "3,4"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE Diameter option") {
    Fixture f;
    f.host.run("CIRCLE", {"1,1", "D", "10"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).center, {1, 1}));
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE 2P") {
    Fixture f;
    f.host.run("CIRCLE", {"2P", "0,0", "10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).center, {5, 0}));
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE 3P") {
    Fixture f;
    f.host.run("CIRCLE", {"3P", "0,0", "10,0", "5,5"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).center, {5, 0}));
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE 3P through an off-axis triple") {
    Fixture f;
    f.host.run("CIRCLE", {"3P", "3,4", "-5,0", "0,-5"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<CircleData>(es[0]).center, {0, 0}));
    CHECK(close(as<CircleData>(es[0]).radius, 5));
}

TEST_CASE("CIRCLE 3P with collinear points draws nothing") {
    Fixture f;
    auto r = f.host.run("CIRCLE", {"3P", "0,0", "5,0", "10,0"});
    CHECK(r.finished);
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("collinear"));
}

TEST_CASE("CIRCLE Ttr between two lines picks the solution nearest the picks") {
    Fixture f;
    auto xAxis = f.addEntity(LineData{{-100, 0}, {100, 0}});
    auto yAxis = f.addEntity(LineData{{0, -100}, {0, 100}});
    SUBCASE("first quadrant") {
        f.host.run("CIRCLE", {"Ttr", ScriptItem::pick(xAxis, {50, 0}),
                              ScriptItem::pick(yAxis, {0, 50}), "5"});
        auto es = f.entities();
        REQUIRE(es.size() == 3);
        CHECK(close(as<CircleData>(es[2]).center, {5, 5}));
        CHECK(close(as<CircleData>(es[2]).radius, 5));
    }
    SUBCASE("second quadrant") {
        f.host.run("CIRCLE", {"Ttr", ScriptItem::pick(xAxis, {-50, 0}),
                              ScriptItem::pick(yAxis, {0, 50}), "5"});
        auto es = f.entities();
        REQUIRE(es.size() == 3);
        CHECK(close(as<CircleData>(es[2]).center, {-5, 5}));
    }
    SUBCASE("third quadrant") {
        f.host.run("CIRCLE", {"Ttr", ScriptItem::pick(xAxis, {-50, 0}),
                              ScriptItem::pick(yAxis, {0, -50}), "5"});
        auto es = f.entities();
        REQUIRE(es.size() == 3);
        CHECK(close(as<CircleData>(es[2]).center, {-5, -5}));
    }
}

TEST_CASE("CIRCLE Ttr between a line and a circle") {
    Fixture f;
    auto line = f.addEntity(LineData{{-100, 0}, {100, 0}});
    auto circ = f.addEntity(CircleData{{30, 12}, 10});
    f.host.run("CIRCLE",
               {"Ttr", ScriptItem::pick(line, {0, 0}), ScriptItem::pick(circ, {20, 12}), "5"});
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& c = as<CircleData>(es[2]);
    CHECK(close(c.radius, 5));
    CHECK(close(c.center.y, 5));                     // tangent to the line
    CHECK(close(distance(c.center, {30, 12}), 15));  // externally tangent to the circle
    CHECK(c.center.x < 30);                          // the solution on the picked side
}

TEST_CASE("CIRCLE Ttr between two circles and with an arc") {
    Fixture f;
    auto a = f.addEntity(CircleData{{0, 0}, 10});
    auto b = f.addEntity(ArcData{{30, 0}, 10, 0, kPi});
    f.host.run("CIRCLE", {"Ttr", ScriptItem::pick(a, {10, 0}), ScriptItem::pick(b, {20, 0}), "5"});
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& c = as<CircleData>(es[2]);
    CHECK(close(distance(c.center, {0, 0}), 15));
    CHECK(close(distance(c.center, {30, 0}), 15));
    CHECK(close(c.center.x, 15));
}

TEST_CASE("CIRCLE Ttr with no solution reports it") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {100, 0}});
    auto b = f.addEntity(LineData{{0, 100}, {100, 100}});
    f.host.run("CIRCLE",
               {"Ttr", ScriptItem::pick(a, {10, 0}), ScriptItem::pick(b, {10, 100}), "5"});
    CHECK(f.entities().size() == 2);
    CHECK(f.host.logContains("does not exist"));
}

TEST_CASE("CIRCLE Ttr only accepts lines, circles and arcs") {
    Fixture f;
    auto pt = f.addEntity(PointData{{1, 1}});
    f.host.run("CIRCLE", {"Ttr"});
    auto r = f.host.feed({ScriptItem::pick(pt, {1, 1})});
    CHECK(r.rejected == 1);
    f.host.feed({ScriptItem::esc()});
}

TEST_CASE("CIRCLE: Esc leaves the drawing untouched and the preview follows the cursor") {
    Fixture f;
    f.host.run("CIRCLE", {"0,0"});
    cmd::RawInput in;
    in.worldCursor = {0, 7};
    f.host.runner().pointerMoved(in);
    REQUIRE_FALSE(f.host.preview().items.empty());
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("CIRCLE is one undo step") {
    Fixture f;
    f.host.run("CIRCLE", {"0,0", "5"});
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}
