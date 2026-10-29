#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {

const double kBulge = std::tan(kPi / 6);

bool near(Vec2 a, Vec2 b) {
    return distance(a, b) < 1e-9;
}

// A 100 x 50 rectangle cloud with arcs of at most `len`, marked as REVCLOUD marks it.
Entity rectCloud(double len, bool inward = false) {
    Entity e;
    e.data = cloudClosed({{0, 0}, {100, 0}, {100, 50}, {0, 50}}, true, len, inward, false);
    markCloud(e, len, true);
    return e;
}

const PolylineData& pl(const Entity& e) {
    return *entityAs<PolylineData>(e);
}

}  // namespace

TEST_CASE("a marked cloud gives back its outline corners and arc length") {
    const Entity e = rectCloud(25);
    REQUIRE(pl(e).v.size() == 12);
    const auto s = cloudShape(e);
    REQUIRE(s);
    CHECK(s->closed);
    CHECK(s->corners);
    CHECK_FALSE(s->inward);
    CHECK_FALSE(s->calligraphy);
    CHECK(s->arcLength == doctest::Approx(25));
    REQUIRE(s->outline.size() == 4);
    CHECK(near(s->outline[0], {0, 0}));
    CHECK(near(s->outline[1], {100, 0}));
    CHECK(near(s->outline[2], {100, 50}));
    CHECK(near(s->outline[3], {0, 50}));
    CHECK(cloudShape(rectCloud(25, true))->inward);
}

TEST_CASE("a new arc length lays the arcs again along the same outline") {
    const Entity e = rectCloud(25);
    auto s = *cloudShape(e);
    s.arcLength = 10;
    auto r = reshapeCloud(e, s);
    REQUIRE(r);
    const auto& p = pl(r.value());
    CHECK(p.closed);
    CHECK(p.v.size() == 30);  // 10 + 5 + 10 + 5
    for (const PolyVertex& v : p.v)
        CHECK(v.bulge == doctest::Approx(kBulge));
    const auto again = cloudShape(r.value());
    REQUIRE(again);
    CHECK(again->arcLength == doctest::Approx(10));
    CHECK(again->outline.size() == 4);

    s.arcLength = 120;  // longer than every edge: one arc per edge
    auto big = reshapeCloud(e, s);
    REQUIRE(big);
    CHECK(pl(big.value()).v.size() == 4);
    CHECK(cloudShape(big.value())->arcLength == doctest::Approx(120));

    s.arcLength = 0.001;  // far too many arcs
    CHECK_FALSE(reshapeCloud(e, s));
    s.arcLength = 0;
    CHECK_FALSE(reshapeCloud(e, s));
}

TEST_CASE("cloud grips are the outline corners and edges, dragging one lays the arcs again") {
    const Entity e = rectCloud(25);
    const auto grips = gripPoints(e);
    REQUIRE(grips.size() == 8);
    CHECK(near(grips[2], {100, 50}));
    CHECK(near(grips[4], {50, 0}));  // middle of the first edge

    // Pull the top right corner out to 200,50: the bottom edge grows from 4 arcs to more.
    auto moved = moveGrip(e, 2, {200, 50});
    REQUIRE(moved);
    const auto s = cloudShape(moved.value());
    REQUIRE(s);
    REQUIRE(s->outline.size() == 4);
    CHECK(near(s->outline[2], {200, 50}));
    CHECK(s->arcLength == doctest::Approx(25));
    for (std::size_t i = 0; i < pl(moved.value()).v.size(); ++i) {
        const auto& p = pl(moved.value());
        CHECK(distance(p.v[i].p, p.v[(i + 1) % p.v.size()].p) <= 25 + 1e-9);
        CHECK(p.v[i].bulge == doctest::Approx(kBulge));
    }

    // The edge grip moves the whole edge.
    auto edge = moveGrip(e, 4, {50, -20});
    REQUIRE(edge);
    const auto es = cloudShape(edge.value());
    REQUIRE(es);
    CHECK(near(es->outline[0], {0, -20}));
    CHECK(near(es->outline[1], {100, -20}));
    CHECK_FALSE(moveGrip(e, 8, {0, 0}));
}

TEST_CASE("the arc length of a scaled cloud follows the scale") {
    const Entity e = rectCloud(25);
    auto big = transformed(e, Transform2::scale(2, 2));
    REQUIRE(big);
    const auto s = cloudShape(big.value());
    REQUIRE(s);
    CHECK(s->arcLength == doctest::Approx(50));
    CHECK(near(s->outline[2], {200, 100}));
}

TEST_CASE("a mirrored cloud keeps its arcs outwards") {
    const Entity e = rectCloud(25);
    auto m = transformed(e, Transform2::mirror({0, 0}, {0, 1}).value());
    REQUIRE(m);
    const auto s = cloudShape(m.value());
    REQUIRE(s);
    CHECK_FALSE(s->inward);
}

TEST_CASE("only clouds are clouds") {
    Entity line;
    line.data = LineData{{0, 0}, {1, 0}};
    CHECK_FALSE(cloudShape(line));

    // A plain polyline of arcs is not a cloud...
    Entity arcs;
    PolylineData p;
    p.closed = true;
    for (Vec2 q : {Vec2{0, 0}, Vec2{10, 0}, Vec2{10, 10}})
        p.v.push_back({q, 0.3});
    arcs.data = p;
    CHECK_FALSE(cloudShape(arcs));
    // ...but one REVCLOUD drew before clouds were marked is (120-degree arcs, all one way).
    for (auto& v : p.v)
        v.bulge = kBulge;
    arcs.data = p;
    CHECK(cloudShape(arcs));

    // A marked cloud whose arcs were turned into lines is no longer one.
    Entity e = rectCloud(25);
    entityAs<PolylineData>(e)->v[3].bulge = 0;
    CHECK_FALSE(cloudShape(e));
}

TEST_CASE("a cloud from a circle keeps equal arcs when its arc length changes") {
    std::vector<Vec2> ring;
    for (int i = 0; i < 72; ++i)
        ring.push_back({50 * std::cos(i * kTwoPi / 72), 50 * std::sin(i * kTwoPi / 72)});
    Entity e;
    e.data = cloudClosed(ring, false, 20, false, false);
    markCloud(e, 20, false);
    const std::size_t before = pl(e).v.size();
    auto s = *cloudShape(e);
    CHECK_FALSE(s.corners);
    s.arcLength = 40;
    auto r = reshapeCloud(e, s);
    REQUIRE(r);
    CHECK(pl(r.value()).v.size() < before);
    CHECK(pl(r.value()).v.size() >= 7);
}

TEST_CASE("an open cloud keeps its arcs on the same side when a grip moves") {
    Entity e;
    e.data = cloudOpen({{0, 0}, {100, 0}, {100, 60}}, 20, false, true);
    markCloud(e, 20, true);
    const auto s = cloudShape(e);
    REQUIRE(s);
    CHECK_FALSE(s->closed);
    CHECK(s->calligraphy);
    REQUIRE(s->outline.size() == 3);
    const double side = pl(e).v.front().bulge;
    // Swing the end across, so the path now turns the other way.
    auto r = moveGrip(e, 2, {100, -60});
    REQUIRE(r);
    const auto& p = pl(r.value());
    CHECK_FALSE(p.closed);
    CHECK(p.v.back().bulge == 0);
    for (std::size_t i = 0; i + 1 < p.v.size(); ++i)
        CHECK(p.v[i].bulge == doctest::Approx(side));
    CHECK(p.v.front().startWidth > 0);  // still pen-stroke arcs
}
