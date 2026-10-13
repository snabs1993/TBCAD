#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

TEST_CASE("RECTANGLE from two corners is a closed 4-vertex polyline") {
    Fixture f;
    CHECK(f.host.run("REC", {"0,0", "10,5"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    checkVertices(pl, {{0, 0}, {10, 0}, {10, 5}, {0, 5}});
    CHECK(pl.closed);
    for (const auto& v : pl.v)
        CHECK(v.bulge == 0);
}

TEST_CASE("RECTANGLE corners in any order") {
    Fixture f;
    f.host.run("RECTANGLE", {"10,5", "0,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkVertices(as<PolylineData>(es[0]), {{10, 5}, {0, 5}, {0, 0}, {10, 0}});
}

TEST_CASE("RECTANGLE Dimensions: the second point only picks the quadrant") {
    Fixture f;
    f.host.run("REC", {"0,0", "Dimensions", "10", "5", "20,20"});
    f.host.run("REC", {"0,0", "D", "10", "5", "-1,-1"});
    f.host.run("REC", {"0,0", "D", "10", "5", "3,-1"});
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    checkVertices(as<PolylineData>(es[0]), {{0, 0}, {10, 0}, {10, 5}, {0, 5}});
    checkVertices(as<PolylineData>(es[1]), {{0, 0}, {-10, 0}, {-10, -5}, {0, -5}});
    checkVertices(as<PolylineData>(es[2]), {{0, 0}, {10, 0}, {10, -5}, {0, -5}});
}

TEST_CASE("RECTANGLE Rotation turns the rectangle about the first corner") {
    Fixture f;
    f.host.run("REC", {"0,0", "Rotation", "90", "-5,10"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkVertices(as<PolylineData>(es[0]), {{0, 0}, {0, 10}, {-5, 10}, {-5, 0}});
}

TEST_CASE("RECTANGLE Rotation at 45 degrees") {
    Fixture f;
    f.host.run("REC", {"0,0", "R", "45", "0,10"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    // u = (1,1)/sqrt2, v = (-1,1)/sqrt2; (0,10) has components (10/sqrt2, 10/sqrt2).
    const double k = 10 / std::sqrt(2.0);
    const double s = 1 / std::sqrt(2.0);
    checkVertices(as<PolylineData>(es[0]), {{0, 0}, {k * s, k * s}, {0, 10}, {-k * s, k * s}});
}

TEST_CASE("RECTANGLE Dimensions together with Rotation") {
    Fixture f;
    f.host.run("REC", {"1,1", "R", "90", "D", "4", "2", "-1,5"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    // Length 4 runs along the rotated x axis (+y), width 2 along the rotated y axis (-x).
    checkVertices(as<PolylineData>(es[0]), {{1, 1}, {1, 5}, {-1, 5}, {-1, 1}});
}

TEST_CASE("RECTANGLE refuses a zero width and asks again") {
    Fixture f;
    f.host.run("REC", {"0,0", "10,0"});
    CHECK(f.host.logContains("zero width or height"));
    f.host.feed({"10,5"});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("RECTANGLE: Esc draws nothing; the preview is the rectangle to the cursor") {
    Fixture f;
    f.host.run("REC", {"0,0"});
    cmd::RawInput in;
    in.worldCursor = {10, 5};
    f.host.runner().pointerMoved(in);
    REQUIRE(f.host.preview().items.size() == 1);
    const auto* pl = std::get_if<DisplayPolyline>(&f.host.preview().items[0]);
    REQUIRE(pl != nullptr);
    CHECK(pl->closed);
    CHECK(pl->pts.size() == 4);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}
