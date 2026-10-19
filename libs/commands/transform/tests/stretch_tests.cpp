#include <doctest/doctest.h>

#include "common.h"

using namespace tbtest;
using cmd::ScriptItem;

TEST_CASE("STRETCH moves only the vertices inside the crossing window") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {100, 0}});
    auto b = add(f.doc, LineData{{0, 50}, {100, 50}});  // outside the window
    auto c = add(f.doc, CircleData{{90, 5}, 3});        // centre inside: moves whole
    auto r = f.host.run("STRETCH", {"60,-10", "120,10", "100,0", "120,0"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(near(get<LineData>(f.doc, a).b, {120, 0}));
    CHECK(near(get<LineData>(f.doc, b).b, {100, 50}));
    CHECK(near(get<CircleData>(f.doc, c).center, {110, 5}));
    CHECK(f.host.undoStack().undoLabel() == "STRETCH");
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, a).b, {100, 0}));
}

TEST_CASE("STRETCH moves a line that lies completely inside the window") {
    Fixture f;
    auto a = add(f.doc, LineData{{10, 10}, {20, 10}});
    auto r = f.host.run("S", {"0,0", "30,30", "0,0", "5,5"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {15, 15}));
    CHECK(near(get<LineData>(f.doc, a).b, {25, 15}));
}

TEST_CASE("STRETCH moves the polyline vertices inside the window") {
    Fixture f;
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{50, 0}, 0}, {{50, 50}, 0}};
    auto p = add(f.doc, pl);
    auto r = f.host.run("STRETCH", {"40,-5", "60,60", "0,0", "10,0"});
    CHECK(r.finished);
    const auto& v = get<PolylineData>(f.doc, p).v;
    CHECK(near(v[0].p, {0, 0}));
    CHECK(near(v[1].p, {60, 0}));
    CHECK(near(v[2].p, {60, 50}));
}

TEST_CASE("STRETCH says so when no end point lies inside the window") {
    Fixture f;
    add(f.doc, LineData{{0, 0}, {100, 0}});
    auto r = f.host.run("STRETCH", {"40,-10", "60,10"});  // crosses the middle only
    CHECK(r.finished);
    CHECK(f.host.logContains("nothing to stretch"));
}

TEST_CASE("STRETCH with an empty window changes nothing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto r = f.host.run("STRETCH", {"200,200", "300,300"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("COPY Array makes a row of copies with a fixed spacing or fitted to a point") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("COPY", {"0,0", "Array", "4", "20,0"});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 4);  // the source plus three copies
    f.host.undo();
    CHECK(f.doc.entityCount() == 1);
    r = f.host.run("COPY", {"0,0", "Array", "4", "Fit", "90,0"});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 4);
    int found = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data); l && near(l->a, {90, 0}))
            ++found;
    });
    CHECK(found == 1);
}

TEST_CASE("STRETCH CPolygon moves only the vertices inside the polygon") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {100, 0}});
    auto b =
        add(f.doc, LineData{{0, 40}, {100, 40}});  // end inside the box but outside the triangle
    auto r = f.host.run("STRETCH", {"CP", "90,-10", "120,-10", "120,20", cmd::ScriptItem::enter(),
                                    "100,0", "110,0"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(near(get<LineData>(f.doc, a).b, {110, 0}));
    CHECK(near(get<LineData>(f.doc, b).b, {100, 40}));
}

TEST_CASE("STRETCH moves an ellipse whose centre is inside the window and leaves one outside") {
    Fixture f;
    EllipseData e;
    e.center = {90, 5};
    e.majorAxis = {10, 0};
    e.ratio = 0.5;
    auto in = add(f.doc, e);
    e.center = {0, 5};  // the window only catches its right-hand tip
    e.majorAxis = {62, 0};
    auto out = add(f.doc, e);
    f.host.run("STRETCH", {"60,-10", "120,20", "100,0", "120,0"});
    CHECK(near(get<EllipseData>(f.doc, in).center, {110, 5}));
    CHECK(near(get<EllipseData>(f.doc, in).majorAxis, {10, 0}));
    CHECK(near(get<EllipseData>(f.doc, out).center, {0, 5}));
    CHECK(f.host.undo());
    CHECK(near(get<EllipseData>(f.doc, in).center, {90, 5}));
}

TEST_CASE("STRETCH moves only the control points of a spline inside the window") {
    Fixture f;
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = {{0, 0}, {20, 30}, {80, 30}, {100, 0}};
    auto id = add(f.doc, Indirect<SplineData>(sp));
    f.host.run("STRETCH", {"60,10", "120,40", "0,0", "0,10"});
    const auto& s = get<SplineData>(f.doc, id);
    REQUIRE(s.control.size() == 4);
    CHECK(near(s.control[0], {0, 0}));
    CHECK(near(s.control[1], {20, 30}));
    CHECK(near(s.control[2], {80, 40}));
    CHECK(near(s.control[3], {100, 0}));  // y=0 is below the window
    CHECK(f.host.undo());
    CHECK(near(get<SplineData>(f.doc, id).control[2], {80, 30}));
}

TEST_CASE("STRETCH moves the fit points of a fit-only spline inside the window") {
    Fixture f;
    SplineData sp;
    sp.degree = 3;
    sp.fit = {{0, 0}, {30, 20}, {60, 20}, {90, 0}};
    auto id = add(f.doc, Indirect<SplineData>(sp));
    f.host.run("STRETCH", {"50,10", "100,30", "0,0", "5,0"});
    const auto& s = get<SplineData>(f.doc, id);
    REQUIRE(s.fit.size() == 4);
    CHECK(near(s.fit[0], {0, 0}));
    CHECK(near(s.fit[1], {30, 20}));
    CHECK(near(s.fit[2], {65, 20}));
}
