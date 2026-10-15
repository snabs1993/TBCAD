#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

TEST_CASE("RAY starts at one point and goes through each further point") {
    Fixture f;
    f.host.run("RAY", {"10,10", "20,10", "10,30", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& a = indirect<RayData>(es[0]);
    CHECK_FALSE(a.infinite);
    CHECK(close(a.origin, {10, 10}));
    CHECK(close(a.dir, {1, 0}));
    CHECK(close(indirect<RayData>(es[1]).dir, {0, 1}));
    CHECK(f.host.undoStack().undoCount() >= 1);
}

TEST_CASE("XLINE through two points, with the options for fixed directions") {
    Fixture f;
    f.host.run("XL", {"0,0", "10,10", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(indirect<RayData>(es[0]).infinite);
    CHECK(close(indirect<RayData>(es[0]).dir, {std::sqrt(0.5), std::sqrt(0.5)}));

    Fixture h;
    h.host.run("XLINE", {"Hor", "5,5", "7,9", ""});
    REQUIRE(h.entities().size() == 2);
    CHECK(close(indirect<RayData>(h.entities()[1]).dir, {1, 0}));
    CHECK(close(indirect<RayData>(h.entities()[1]).origin, {7, 9}));

    Fixture v;
    v.host.run("XLINE", {"Ver", "5,5", ""});
    CHECK(close(indirect<RayData>(v.entities()[0]).dir, {0, 1}));

    Fixture a;
    a.host.run("XLINE", {"Ang", "90", "1,1", ""});
    CHECK(close(indirect<RayData>(a.entities()[0]).dir, {0, 1}, 1e-9));
}

TEST_CASE("XLINE Bisect makes the bisector - Offset copies a line at a distance") {
    Fixture f;
    f.host.run("XLINE", {"Bisect", "0,0", "10,0", "0,10", ""});
    REQUIRE(f.entities().size() == 1);
    CHECK(close(indirect<RayData>(f.entities()[0]).dir, {std::sqrt(0.5), std::sqrt(0.5)}));

    Fixture o;
    auto line = o.addEntity(LineData{{0, 0}, {10, 0}});
    o.host.run("XLINE", {"Offset", "5", ScriptItem::pick(line, {5, 0}), "5,-20"});
    o.host.feed({ScriptItem::esc()});
    REQUIRE(o.entities().size() == 2);
    CHECK(close(indirect<RayData>(o.entities()[1]).origin, {0, -5}));
    CHECK(close(indirect<RayData>(o.entities()[1]).dir, {1, 0}));
}

TEST_CASE("rays are picked and snap like lines but stay out of the extents") {
    Fixture f;
    f.host.run("XL", {"0,0", "1,0", ""});
    f.addEntity(LineData{{0, 10}, {10, 10}});
    const Entity* ray = f.entities()[0];
    const BBox2 b = geom::bounds(*ray, f.doc);
    CHECK(b.max.x > 1e4);
    CHECK_FALSE(geom::countsForExtents(*ray));
    CHECK(geom::hitTest(*ray, {5000, 0.05}, 0.1, &f.doc));
    CHECK_FALSE(geom::hitTest(*ray, {5000, 5}, 0.1, &f.doc));
    std::vector<geom::CurveRef> cs;
    CHECK(geom::curvesOf(*ray, cs));
    CHECK(cs.size() == 1);
    CHECK(geom::endpoints(*ray).empty());  // an xline has no ends
}
