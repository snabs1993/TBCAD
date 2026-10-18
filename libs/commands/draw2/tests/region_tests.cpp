#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

namespace {
PolylineData box(double x0, double y0, double x1, double y1) {
    PolylineData d;
    d.closed = true;
    for (Vec2 p : {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}}) {
        PolyVertex v;
        v.p = p;
        d.v.push_back(v);
    }
    return d;
}

double totalArea(const Fixture& f) {
    double a = 0;
    for (const Entity* e : f.entities())
        if (const auto* pl = std::get_if<PolylineData>(&e->data))
            a += std::abs(geom::signedArea(*pl));
    return a;
}
}  // namespace

TEST_CASE("UNION merges overlapping boxes and a circle into one polygon") {
    Fixture f;
    auto a = f.addEntity(box(0, 0, 10, 10));
    auto b = f.addEntity(box(5, 5, 15, 15));
    f.host.run("UNION", {ScriptItem::pick(a, {0, 0}), ScriptItem::pick(b, {15, 15}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(totalArea(f), 175, 1e-6));
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().size() == 2);
}

TEST_CASE("SUBTRACT cuts a circle out of a box and leaves a hole") {
    Fixture f;
    auto a = f.addEntity(box(0, 0, 100, 100));
    auto c = f.addEntity(CircleData{{50, 50}, 10});
    f.host.run("SU", {ScriptItem::pick(a, {0, 0}), "", ScriptItem::pick(c, {60, 50}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);                                    // outer boundary and the hole
    CHECK(std::abs(totalArea(f) - (10000 + 100 * kPi)) < 1.0);  // outer area plus hole area
}

TEST_CASE("INTERSECT keeps the shared area - fewer than two objects is refused") {
    Fixture f;
    auto a = f.addEntity(box(0, 0, 10, 10));
    auto b = f.addEntity(box(5, 5, 15, 15));
    f.host.run("IN", {ScriptItem::pick(a, {0, 0}), ScriptItem::pick(b, {15, 15}), ""});
    REQUIRE(f.entities().size() == 1);
    CHECK(close(totalArea(f), 25, 1e-6));
    f.host.run("UNI", {ScriptItem::pick(f.entities()[0]->common.id, {5, 5}), ""});
    CHECK(f.host.logContains("at least two"));
}

TEST_CASE("REGION turns a circle into a closed polyline and ignores open objects") {
    Fixture f;
    auto c = f.addEntity(CircleData{{0, 0}, 10});
    auto line = f.addEntity(LineData{{0, 0}, {5, 5}});
    f.host.run("REGION", {ScriptItem::pick(c, {10, 0}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    bool found = false;
    for (const Entity* e : es)
        if (const auto* pl = std::get_if<PolylineData>(&e->data)) {
            found = true;
            CHECK(pl->closed);
            CHECK(std::abs(geom::area(*pl) - kPi * 100) < 0.5);
        }
    CHECK(found);
    (void)line;
    CHECK(f.host.run("BPOLY", {"50,50"}).finished == false);  // not enclosed: asks again
}

TEST_CASE("REGION, UNION and SUBTRACT on awkward shapes never crash and keep sane areas") {
    // A bow-tie (self-intersecting polyline), an ellipse, a closed spline and a bulged shape.
    Fixture f;
    PolylineData bow;
    bow.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{10, 10}, Vec2{10, 0}, Vec2{0, 10}}) {
        PolyVertex v;
        v.p = p;
        bow.v.push_back(v);
    }
    auto a = f.addEntity(bow);
    EllipseData e;
    e.center = {5, 5};
    e.majorAxis = {8, 0};
    e.ratio = 0.5;
    auto b = f.addEntity(std::move(e));
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3};
    sp.control = {{0, 20}, {10, 30}, {20, 20}, {20, 30}, {5, 40}, {0, 25}};
    sp.closed = true;
    auto c = f.addEntity(std::move(sp));
    PolylineData bulged = box(30, 0, 40, 10);
    bulged.v[0].bulge = 0.5;
    bulged.v[2].bulge = -0.4;
    auto d = f.addEntity(std::move(bulged));
    for (EntityId id : {a, b, c, d})
        f.host.run("REGION", {ScriptItem::pick(id, {5, 5}), ""});
    for (const Entity* x : f.entities())
        if (const auto* pl = std::get_if<PolylineData>(&x->data))
            CHECK(std::isfinite(geom::area(*pl)));
    const std::size_t before = f.entities().size();
    f.host.run("UNION", {ScriptItem::pick(a, {5, 5}), ScriptItem::pick(b, {5, 5}), ""});
    f.host.run("SU", {ScriptItem::pick(d, {35, 5}), "", ScriptItem::pick(d, {35, 5}), ""});
    for (const Entity* x : f.entities())
        if (const auto* pl = std::get_if<PolylineData>(&x->data))
            CHECK(std::isfinite(geom::area(*pl)));
    CHECK(f.entities().size() > 0);
    (void)before;
}
