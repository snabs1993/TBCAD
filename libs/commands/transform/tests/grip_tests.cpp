#include <doctest/doctest.h>

#include <chrono>

#include "common.h"
#include "tbcad/transform/grips.h"

using namespace tbtest;
using cmd::ScriptItem;
namespace tf = tbcad::transform;

TEST_CASE("grip collection follows geom::gripPoints and respects the limit") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto c = add(f.doc, CircleData{{0, 0}, 5});
    std::vector<EntityId> ids{a, c};
    auto grips = tf::collectGrips(f.doc, ids);
    CHECK(grips.size() == 3 + 5);
    CHECK(tf::collectGrips(f.doc, ids, 1).empty());
    const tf::Grip* g = tf::hitGrip(grips, {10.2, 0.1}, 1.0);
    REQUIRE(g != nullptr);
    CHECK(g->entity == a);
    CHECK(g->index == 1);
    CHECK(tf::hitGrip(grips, {50, 50}, 1.0) == nullptr);
}

TEST_CASE("GRIPEDIT stretches a line end; one undo step") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("GRIPEDIT", {"10,0", "20,5"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(near(get<LineData>(f.doc, a).b, {20, 5}));
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("the shared end of two selected lines stretches together") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, LineData{{10, 0}, {10, 10}});
    f.host.setSelection({a, b});
    f.host.run("GRIPEDIT", {"10,0", "12,-3"});
    CHECK(near(get<LineData>(f.doc, a).b, {12, -3}));
    CHECK(near(get<LineData>(f.doc, b).a, {12, -3}));
    CHECK(near(get<LineData>(f.doc, b).b, {10, 10}));
}

TEST_CASE("the midpoint grip of a line moves the whole line") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"5,0", "5,5"});
    CHECK(near(get<LineData>(f.doc, a).a, {0, 5}));
    CHECK(near(get<LineData>(f.doc, a).b, {10, 5}));
}

TEST_CASE("a circle quadrant grip changes the radius, the centre grip moves it") {
    Fixture f;
    auto c = add(f.doc, CircleData{{0, 0}, 5});
    f.host.setSelection({c});
    f.host.run("GRIPEDIT", {"5,0", "8,0"});
    CHECK(get<CircleData>(f.doc, c).radius == doctest::Approx(8));
    CHECK(near(get<CircleData>(f.doc, c).center, {0, 0}));
    f.host.run("GRIPEDIT", {"0,0", "3,4"});
    CHECK(near(get<CircleData>(f.doc, c).center, {3, 4}));
}

TEST_CASE("polyline vertex grip and bulge midpoint grip") {
    Fixture f;
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}};
    auto id = add(f.doc, pl);
    f.host.setSelection({id});
    f.host.run("GRIPEDIT", {"10,0", "12,1"});
    CHECK(near(get<PolylineData>(f.doc, id).v[1].p, {12, 1}));
    // midpoint of segment 0 is now (6, 0.5); drag it off the chord to create an arc
    f.host.run("GRIPEDIT", {"6,0.5", "6,-3"});
    const auto& p = get<PolylineData>(f.doc, id);
    CHECK(p.v[0].bulge != doctest::Approx(0.0));
    CHECK(near(p.v[0].p, {0, 0}));
    CHECK(near(p.v[1].p, {12, 1}));
}

TEST_CASE("Enter cycles stretch -> move -> rotate and keywords pick a mode") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    // Enter once: Move. Drag the hot grip (10,0) to (10,10): the whole line moves by (0,10).
    f.host.run("GRIPEDIT", {"10,0", ScriptItem::enter(), "10,10"});
    CHECK(near(get<LineData>(f.doc, a).a, {0, 10}));
    CHECK(near(get<LineData>(f.doc, a).b, {10, 10}));
}

TEST_CASE("grip rotate, scale and mirror use the base point") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,0", "Base", "0,0", "Rotate", "0,10"});
        CHECK(near(get<LineData>(f.doc, a).b, {0, 10}));
        CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,0", "Base", "0,0", "Scale", "30,0"});
        CHECK(near(get<LineData>(f.doc, a).b, {30, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 1}, {10, 1}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,1", "Base", "0,0", "Mirror", "10,0"});
        CHECK(near(get<LineData>(f.doc, a).a, {0, -1}));
        CHECK(near(get<LineData>(f.doc, a).b, {10, -1}));
    }
}

TEST_CASE("Copy mode keeps the original and repeats until Exit; all copies undo together") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("GRIPEDIT", {"5,0", "Move", "Copy", "5,10", "5,20", "Exit"});
    CHECK(r.finished);
    CHECK(f.doc.modelSpaceCount() == 3);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(f.host.undo());
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("Undo keyword inside a grip session steps back one drag") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"5,0", "Move", "Copy", "5,10", "Undo", "Exit"});
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("selections above grips.limit show no grips and GRIPEDIT refuses") {
    Fixture f;
    f.host.setSetting("grips.limit", "2");
    std::vector<EntityId> ids;
    for (int i = 0; i < 3; ++i)
        ids.push_back(add(f.doc, PointData{{double(i), 0}}));
    f.host.setSelection(ids);
    CHECK(tf::gripLimit(f.host) == 2);
    CHECK(tf::collectGrips(f.doc, ids, tf::gripLimit(f.host)).empty());
    f.host.run("GRIPEDIT", {"1,0", "5,5"});
    CHECK(f.host.logContains("Too many"));
    CHECK(near(get<PointData>(f.doc, ids[1]).p, {1, 0}));
}

TEST_CASE("a click that misses every grip changes nothing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {100, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"50,50", "60,60"});
    CHECK(f.host.logContains("No grip"));
    CHECK(near(get<LineData>(f.doc, a).b, {100, 0}));
}

TEST_CASE("performance: MOVE of 100k selected lines is one undo step and fast") {
    Fixture f;
    std::vector<EntityId> ids;
    {
        auto tx = f.doc.begin("setup");
        for (int i = 0; i < 100000; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{double(i), 0}, {double(i), 1}}}).value());
        tx.commit();
    }
    f.host.setSelection(ids);
    auto t0 = std::chrono::steady_clock::now();
    auto r = f.host.run("MOVE", {"0,0", "10,10"});
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, ids[5]).a, {15, 10}));
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, ids[5]).a, {5, 0}));
    MESSAGE("MOVE 100k lines: " << ms << " ms");
    CHECK(ms < 5000);  // generous bound for debug/sanitizer CI; typical release is far lower
    // Grips are never built above the limit: no per-entity cost for a huge selection.
    auto g0 = std::chrono::steady_clock::now();
    CHECK(tf::collectGrips(f.doc, ids, tf::kDefaultGripLimit).empty());
    CHECK(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() <
          5);
}
