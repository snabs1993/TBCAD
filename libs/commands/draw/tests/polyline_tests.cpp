#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

TEST_CASE("PLINE draws one polyline with a vertex per point") {
    Fixture f;
    CHECK(f.host.run("PL", {"0,0", "100,0", "100,50", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {100, 0}, {100, 50}});
    CHECK_FALSE(pl.closed);
    for (const auto& v : pl.v) {
        CHECK(v.bulge == 0);
        CHECK(v.startWidth == 0);
        CHECK(v.endWidth == 0);
    }
}

TEST_CASE("PLINE Close in line mode") {
    Fixture f;
    f.host.run("POLYLINE", {"0,0", "100,0", "100,50", "Close"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {100, 0}, {100, 50}});
    CHECK(pl.closed);
}

TEST_CASE("PLINE Undo removes the last vertex") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "100,50", "Undo", "0,50", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkVertices(as<PolylineData>(es[0]), {{0, 0}, {100, 0}, {0, 50}});
}

TEST_CASE("PLINE Width sets the widths of the following segments") {
    Fixture f;
    f.host.run("PL", {"0,0", "Width", "2", "4", "100,0", "100,50", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 3);
    CHECK(close(pl.v[0].startWidth, 2));
    CHECK(close(pl.v[0].endWidth, 4));
    CHECK(close(pl.v[1].startWidth, 2));
    CHECK(close(pl.v[1].endWidth, 4));
    CHECK(pl.v[2].startWidth == 0);
}

TEST_CASE("PLINE Width: Enter at the ending width keeps the starting width") {
    Fixture f;
    f.host.run("PL", {"0,0", "W", "3", "", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(close(pl.v[0].startWidth, 3));
    CHECK(close(pl.v[0].endWidth, 3));
}

TEST_CASE("PLINE Halfwidth doubles the entered values") {
    Fixture f;
    f.host.run("PL", {"0,0", "H", "1", "2", "100,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(close(pl.v[0].startWidth, 2));
    CHECK(close(pl.v[0].endWidth, 4));
}

TEST_CASE("PLINE widths can change per segment and Undo clears the undone one") {
    Fixture f;
    f.host.run("PL", {"0,0", "W", "1", "1", "10,0", "W", "5", "5", "20,0", "U", "30,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 3);
    CHECK(close(pl.v[0].startWidth, 1));
    // The segment re-entered after Undo uses the widths in force at that point (5).
    CHECK(close(pl.v[1].startWidth, 5));
    CHECK(close(pl.v[1].endWidth, 5));
    CHECK(close(pl.v[2].p.x, 30));
}

TEST_CASE("PLINE Arc mode continues tangent (first segment: along +x)") {
    Fixture f;
    f.host.run("PL", {"0,0", "Arc", "10,10", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {10, 10}});
    CHECK(close(pl.v[0].bulge, std::tan(kPi / 8)));
}

TEST_CASE("PLINE Arc after a line leaves tangent to that line") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "A", "100,100", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {100, 0}, {100, 100}});
    CHECK(pl.v[0].bulge == 0);
    CHECK(close(pl.v[1].bulge, 1));  // a half circle, counter-clockwise
}

TEST_CASE("PLINE Line returns from Arc mode") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "A", "100,100", "L", "0,100", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {100, 0}, {100, 100}, {0, 100}});
    CHECK(pl.v[2].bulge == 0);
}

TEST_CASE("PLINE Arc chains tangentially") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "A", "100,100", "50,150", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 4);
    CHECK(close(pl.v[1].bulge, 1));
    // The first arc ends heading west; the next one turns north-west and back towards the east.
    CHECK(close(pl.v[2].bulge, -std::tan(kPi / 8)));
}

TEST_CASE("PLINE Arc straight ahead along the tangent is a straight segment") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "A", "100,100", "0,100", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 4);
    CHECK(pl.v[2].bulge == 0);
}

TEST_CASE("PLINE Arc refuses an end point straight behind the tangent") {
    Fixture f;
    f.host.run("PL", {"0,0", "100,0", "A", "100,100", "200,100"});
    CHECK(f.host.logContains("cannot be reached"));
    f.host.feed({""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(as<PolylineData>(es[0]).v.size() == 3);
}

TEST_CASE("PLINE Arc Second point") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "Second", "5,5", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {10, 0}});
    CHECK(close(pl.v[0].bulge, -1));  // clockwise half circle through (5,5)
}

TEST_CASE("PLINE Arc Angle") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "Angle", "90", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).v[0].bulge, std::tan(kPi / 8)));
}

TEST_CASE("PLINE Arc CEnter") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "CEnter", "5,0", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    // From angle 180 deg to 0 around (5,0), counter-clockwise: a half circle below the chord.
    CHECK(close(as<PolylineData>(es[0]).v[0].bulge, 1));
}

TEST_CASE("PLINE Arc Direction") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "Direction", "90", "10,10", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).v[0].bulge, -std::tan(kPi / 8)));
}

TEST_CASE("PLINE Arc Radius") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "Radius", "5", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).v[0].bulge, 1));
}

TEST_CASE("PLINE Arc Radius too small is refused and the polyline goes on") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "R", "2", "10,0"});
    CHECK(f.host.logContains("No arc matches"));
    f.host.feed({"5,5", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(as<PolylineData>(es[0]).v.size() == 2);
}

TEST_CASE("PLINE Close in arc mode closes with a tangent arc") {
    Fixture f;
    f.host.run("PL", {"0,0", "A", "10,10", "Close"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {10, 10}});
    CHECK(pl.closed);
    CHECK(close(pl.v[1].bulge, std::tan(3 * kPi / 8)));
}

TEST_CASE("PLINE: Esc discards everything, one point makes nothing") {
    Fixture f;
    f.host.run("PL", {"0,0", "10,0", "10,10", ScriptItem::esc()});
    CHECK(f.entities().empty());
    f.host.run("PL", {"0,0", ""});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("PLINE is one undo step") {
    Fixture f;
    f.host.run("PL", {"0,0", "10,0", "10,10", "20,10", ""});
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("PLINE previews the whole polyline plus the segment to the cursor") {
    Fixture f;
    f.host.run("PL", {"0,0", "10,0"});
    cmd::RawInput in;
    in.worldCursor = {10, 10};
    f.host.runner().pointerMoved(in);
    REQUIRE(f.host.preview().items.size() == 1);
    const auto* pl = std::get_if<DisplayPolyline>(&f.host.preview().items[0]);
    REQUIRE(pl != nullptr);
    CHECK(pl->pts.size() == 3);
    f.host.feed({ScriptItem::esc()});
}
