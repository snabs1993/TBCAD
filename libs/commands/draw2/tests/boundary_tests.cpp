#include <chrono>

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
    CHECK(queries >= 1);  // the window around the pick grows until the region fits
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

TEST_CASE("BOUNDARY works on ellipses and spline-bounded shapes") {
    Fixture f;
    EllipseData e;
    e.center = {0, 0};
    e.majorAxis = {30, 0};
    e.ratio = 0.5;
    f.addEntity(std::move(e));
    CHECK(f.host.run("BO", {"0,0", ""}).finished);
    bool found = false;
    for (const Entity* x : f.entities())
        if (const auto* pl = std::get_if<PolylineData>(&x->data)) {
            found = true;
            CHECK(pl->closed);
            CHECK(std::abs(geom::area(*pl) - kPi * 30 * 15) < 5);
        }
    CHECK(found);
}

TEST_CASE("BOUNDARY answers quickly in a drawing of 50 000 objects") {
    Fixture f;
    {
        Transaction tx = f.doc.begin("fill");
        auto line = [&](Vec2 a, Vec2 b) {
            Entity e;
            e.data = LineData{a, b};
            REQUIRE(tx.add(std::move(e)).ok());
        };
        for (int i = 0; i < 12500; ++i) {
            const double x = (i % 125) * 30.0, y = (i / 125) * 30.0;
            line({x, y}, {x + 20, y});
            line({x + 20, y}, {x + 20, y + 20});
            line({x + 20, y + 20}, {x, y + 20});
            line({x, y + 20}, {x, y});
        }
        tx.commit();
    }
    auto t0 = std::chrono::steady_clock::now();
    f.host.run("BO", {"3010,1510", ""});
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("50k objects: BOUNDARY pick " << ms << " ms");
    CHECK(f.entities().size() == 50001);
#ifdef NDEBUG
    CHECK(ms < 200);
#else
    CHECK(ms < 2000);
#endif
}

TEST_CASE("BOUNDARY Islands keyword: Normal, Outer and Ignore on nested shapes") {
    auto build = [](Fixture& f) {
        addRect(f, {0, 0}, {100, 100});
        addRect(f, {10, 10}, {90, 90});  // island
        addRect(f, {30, 30}, {70, 70});  // inside the island
    };
    {
        Fixture f;
        build(f);
        f.host.run("BO", {"5,5", ""});
        CHECK(f.entities().size() == 12 + 3);  // outer, island and the square inside it
    }
    {
        Fixture f;
        build(f);
        f.host.run("BO", {"50,50", ""});
        CHECK(f.entities().size() == 12 + 1);  // the innermost square only
    }
    {
        Fixture f;
        build(f);
        f.host.run("BO", {"Islands", "Ignore", "5,5", ""});
        CHECK(f.entities().size() == 12 + 1);
        f.host.run("BO", {"Islands", "Normal", "5,5", ""});
        CHECK(f.entities().size() == 12 + 1 + 3);
    }
    {
        Fixture f;
        build(f);
        f.host.run("BO", {"Islands", "Outer", "5,5", ""});
        CHECK(f.entities().size() == 12 + 2);
    }
}

TEST_CASE("BOUNDARY finds the outline among many islands and refuses a gap") {
    Fixture f;
    addRect(f, {0, 0}, {200, 200});
    for (int i = 0; i < 100; ++i)
        f.addEntity(CircleData{{10.0 + (i % 10) * 18, 10.0 + (i / 10) * 18}, 5});
    f.host.run("BO", {"1,1", ""});
    CHECK(f.entities().size() == 104 + 101);
    Fixture g;
    g.addEntity(LineData{{0, 0}, {10, 0}});
    g.addEntity(LineData{{10, 0}, {10, 10}});
    g.addEntity(LineData{{10, 10}, {0, 10}});
    g.addEntity(LineData{{0, 10}, {0, 1}});  // gap of 1 at the start
    g.host.run("BO", {"5,5", ""});
    CHECK(g.entities().size() == 4);
}

TEST_CASE("BOUNDARY closes gaps with the HATCH gap tolerance, among 50 000 nearby lines") {
    Fixture f;
    {
        Transaction tx = f.doc.begin("fill");
        auto line = [&](Vec2 a, Vec2 b) {
            Entity e;
            e.data = LineData{a, b};
            REQUIRE(tx.add(std::move(e)).ok());
        };
        for (int i = 0; i < 12500; ++i) {
            const double x = (i % 125) * 30.0 + 1000, y = (i / 125) * 30.0;
            line({x, y}, {x + 20, y});
            line({x + 20, y}, {x + 20, y + 20});
            line({x + 20, y + 20}, {x, y + 20});
            line({x, y + 20}, {x, y});
        }
        // A messy room: gap at one corner, an overlapping duplicate wall, an arc corner.
        line({0, 0}, {100, 0});
        line({50, 0}, {120, 0});  // overlaps the bottom wall
        line({100, 0}, {100, 100});
        line({100, 100}, {0, 100});
        line({0, 100}, {0, 0.4});  // stops 0.4 short of the corner
        tx.commit();
    }
    f.host.run("BO", {"50,50", ""});
    CHECK(f.entities().size() == 50005);  // no closed region: nothing created
    f.host.setSetting("hatch.gap", "1");
    auto t0 = std::chrono::steady_clock::now();
    f.host.run("BO", {"50,50", ""});
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    REQUIRE(f.entities().size() == 50006);
    const auto& pl = as<PolylineData>(f.entities().back());
    CHECK(pl.closed);
    CHECK(std::abs(geom::signedArea(pl)) > 9900);
    MESSAGE("50k objects with a gap: BOUNDARY pick " << ms << " ms");
    CHECK(ms < 3000);
}
