#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

namespace {

void checkRegular(const PolylineData& pl, Vec2 center, double radius, double firstAngle, int n) {
    REQUIRE(static_cast<int>(pl.v.size()) == n);
    CHECK(pl.closed);
    for (int i = 0; i < n; ++i) {
        const double a = firstAngle + kTwoPi * i / n;
        CHECK_MESSAGE(close(pl.v[static_cast<std::size_t>(i)].p,
                            center + Vec2{std::cos(a), std::sin(a)} * radius),
                      "vertex ", i);
    }
}

}  // namespace

TEST_CASE("POLYGON inscribed (default): a vertex at the radius point") {
    Fixture f;
    CHECK(f.host.run("POL", {"6", "0,0", "", "10,0"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkRegular(as<PolylineData>(es[0]), {0, 0}, 10, 0, 6);
}

TEST_CASE("POLYGON inscribed with a rotated radius point") {
    Fixture f;
    f.host.run("POLYGON", {"3", "5,5", "Inscribed", "5,15"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkRegular(as<PolylineData>(es[0]), {5, 5}, 10, kPi / 2, 3);
}

TEST_CASE("POLYGON circumscribed: the radius point is the middle of an edge") {
    Fixture f;
    f.host.run("POL", {"4", "0,0", "Circumscribed", "10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkVertices(as<PolylineData>(es[0]), {{10, -10}, {10, 10}, {-10, 10}, {-10, -10}});
}

TEST_CASE("POLYGON Edge: the first edge runs between the two points") {
    Fixture f;
    f.host.run("POL", {"4", "Edge", "0,0", "10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkVertices(as<PolylineData>(es[0]), {{0, 0}, {10, 0}, {10, 10}, {0, 10}});
}

TEST_CASE("POLYGON Edge with a hexagon") {
    Fixture f;
    f.host.run("POL", {"6", "E", "0,0", "10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 6);
    // All sides are 10 long and the polygon is above the first edge.
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(close(distance(pl.v[i].p, pl.v[(i + 1) % 6].p), 10));
    CHECK(close(pl.v[0].p, {0, 0}));
    CHECK(close(pl.v[1].p, {10, 0}));
    CHECK(pl.v[2].p.y > 0);
}

TEST_CASE("POLYGON accepts 3 to 1024 sides") {
    Fixture f;
    f.host.run("POL", {"2"});
    auto r = f.host.feed({"1025"});
    CHECK_FALSE(r.finished);
    f.host.feed({"1024", "0,0", "", "100,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    checkRegular(as<PolylineData>(es[0]), {0, 0}, 100, 0, 1024);
}

TEST_CASE("POLYGON remembers the number of sides for the next run") {
    Fixture f;
    f.host.run("POL", {"5", "0,0", "", "10,0"});
    f.host.run("POL", {"", "0,0", "", "10,0"});  // Enter = the default, 5
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(as<PolylineData>(es[1]).v.size() == 5);
    // The first run's default is 4.
    Fixture g;
    g.host.run("POL", {"", "0,0", "", "10,0"});
    CHECK(as<PolylineData>(g.entities()[0]).v.size() == 4);
}

TEST_CASE("POLYGON: a zero radius is refused; Esc draws nothing") {
    Fixture f;
    f.host.run("POL", {"4", "0,0", "", "0,0"});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("radius must not be zero"));
    f.host.run("POL", {"4", "0,0", "", ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("POLYGON shows its outline while the radius point moves") {
    Fixture f;
    f.host.run("POL", {"5", "0,0", ""});
    cmd::RawInput in;
    in.worldCursor = {10, 0};
    f.host.runner().pointerMoved(in);
    REQUIRE(f.host.preview().items.size() == 1);
    const auto* pl = std::get_if<DisplayPolyline>(&f.host.preview().items[0]);
    REQUIRE(pl != nullptr);
    CHECK(pl->pts.size() == 5);
    f.host.feed({ScriptItem::esc()});
}
