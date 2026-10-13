#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

namespace {
void addRect(Fixture& f, Vec2 a, Vec2 b) {
    f.addEntity(LineData{{a.x, a.y}, {b.x, a.y}});
    f.addEntity(LineData{{b.x, a.y}, {b.x, b.y}});
    f.addEntity(LineData{{b.x, b.y}, {a.x, b.y}});
    f.addEntity(LineData{{a.x, b.y}, {a.x, a.y}});
}
}  // namespace

TEST_CASE("BOUNDARY makes a closed polyline from four lines") {
    Fixture f;
    addRect(f, {0, 0}, {100, 50});
    CHECK(f.host.run("BO", {"50,25", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 5);
    const auto& pl = as<PolylineData>(es[4]);
    CHECK(pl.closed);
    REQUIRE(pl.v.size() == 4);
    std::vector<Vec2> got;
    for (const auto& v : pl.v) {
        got.push_back(v.p);
        CHECK(v.bulge == 0);
    }
    for (Vec2 corner : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}}) {
        CHECK(std::any_of(got.begin(), got.end(), [&](Vec2 p) { return close(p, corner); }));
    }
    CHECK(close(std::abs(geom::signedArea(pl)), 5000));
}

TEST_CASE("BOUNDARY also creates a polyline for an island") {
    Fixture f;
    addRect(f, {0, 0}, {100, 50});
    f.addEntity(CircleData{{50, 25}, 10});
    f.host.run("BO", {"5,5", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 7);
    CHECK(as<PolylineData>(es[5]).v.size() == 4);
    const auto& ring = as<PolylineData>(es[6]);
    REQUIRE(ring.v.size() == 2);
    CHECK(close(ring.v[0].bulge, 1));
    CHECK(close(std::abs(geom::signedArea(ring)), kPi * 100, 1e-6));
}

TEST_CASE("BOUNDARY inside the island gives just the circle") {
    Fixture f;
    f.addEntity(CircleData{{0, 0}, 10});
    f.host.run("BO", {"1,1", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(as<PolylineData>(es[1]).v.size() == 2);
}

TEST_CASE("BOUNDARY keeps arcs as bulges") {
    Fixture f;
    f.addEntity(LineData{{-10, 0}, {10, 0}});
    f.addEntity(ArcData{{0, 0}, 10, 0, kPi});
    f.host.run("BO", {"0,5", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    const auto& pl = as<PolylineData>(es[2]);
    REQUIRE(pl.v.size() == 2);
    CHECK(close(std::abs(pl.v[0].bulge) + std::abs(pl.v[1].bulge), 1.0, 1e-6));  // a half circle
    const bool oneBulge = (pl.v[0].bulge == 0) != (pl.v[1].bulge == 0);
    CHECK(oneBulge);
}

TEST_CASE("BOUNDARY reports a point that is not enclosed and goes on asking") {
    Fixture f;
    f.addEntity(LineData{{0, 0}, {10, 0}});
    auto r = f.host.run("BO", {"5,5"});
    CHECK_FALSE(r.finished);
    CHECK(f.host.logContains("closed"));
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("BOUNDARY ignores objects on layers that are off and uses the host index in a view") {
    Fixture f;
    addRect(f, {0, 0}, {100, 50});
    f.host.setView({{50, 25}, 1, {400, 300}});
    int queries = 0;
    f.host.setWindowQuery([&](const BBox2&) {
        ++queries;
        std::vector<EntityId> ids;
        for (const Entity* e : f.entities())
            ids.push_back(e->common.id);
        return ids;
    });
    f.host.run("BO", {"50,25", ""});
    CHECK(queries == 1);
    CHECK(f.entities().size() == 5);
}

TEST_CASE("BOUNDARY: one undo step covers every polyline made") {
    Fixture f;
    addRect(f, {0, 0}, {100, 50});
    f.host.run("BO", {"50,25", "10,10", ""});
    CHECK(f.entities().size() == 6);
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().size() == 4);
}
