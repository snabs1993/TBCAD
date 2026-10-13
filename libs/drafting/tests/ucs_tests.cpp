#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {
constexpr double kQuarter = kPi / 2;
}

TEST_CASE("UCS transforms points and vectors both ways") {
    Ucs u;
    u.origin = {100, 50};
    u.angle = kQuarter;
    CHECK(near(u.toWorld({10, 0}), {100, 60}, 1e-9));
    CHECK(near(u.toWorld({0, 10}), {90, 50}, 1e-9));
    CHECK(near(u.fromWorld({100, 60}), {10, 0}, 1e-9));
    CHECK(near(u.dirToWorld({1, 0}), {0, 1}, 1e-9));
    for (Vec2 p : {Vec2{1, 2}, Vec2{-30, 400}, Vec2{0, 0}})
        CHECK(near(u.fromWorld(u.toWorld(p)), p, 1e-9));
    CHECK(Ucs{}.isWorld());
    CHECK_FALSE(u.isWorld());
}

TEST_CASE("the UCS table is kept in the drawing header: save, replace, delete") {
    Document doc;
    {
        auto tx = doc.begin("ucs");
        Ucs a;
        a.name = "Wing";
        a.origin = {10, 20};
        a.angle = 0.5;
        REQUIRE(saveUcs(tx, a).ok());
        a.name = "Roof";
        REQUIRE(saveUcs(tx, a).ok());
        a.name = "wing";  // same name ignoring case replaces
        a.origin = {1, 2};
        REQUIRE(saveUcs(tx, a).ok());
        CHECK_FALSE(saveUcs(tx, Ucs{"World", {}, 0}).ok());
        CHECK_FALSE(saveUcs(tx, Ucs{"", {}, 0}).ok());
        CHECK_FALSE(saveUcs(tx, Ucs{"a,b", {}, 0}).ok());
        (void)tx.commit();
    }
    auto all = ucsList(doc);
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "wing");
    CHECK(near(all[0].origin, {1, 2}));
    CHECK(all[1].name == "Roof");
    CHECK(all[1].angle == doctest::Approx(0.5));
    {
        auto tx = doc.begin("del");
        CHECK_FALSE(deleteUcs(tx, "Nope").ok());
        REQUIRE(deleteUcs(tx, "ROOF").ok());
        (void)tx.commit();
    }
    CHECK(ucsList(doc).size() == 1);
}

TEST_CASE("the current UCS comes from the host setting: a name, World or an unnamed system") {
    World w;
    {
        auto tx = w.doc.begin("ucs");
        REQUIRE(saveUcs(tx, Ucs{"Wing", {10, 20}, kQuarter}).ok());
        (void)tx.commit();
    }
    CHECK(currentUcs(&w.doc, &w.host).isWorld());
    w.set(kUcsSettingKey, "Wing");
    CHECK(near(currentUcs(&w.doc, &w.host).origin, {10, 20}));
    w.set(kUcsSettingKey, "wing");
    CHECK(currentUcs(&w.doc, &w.host).angle == doctest::Approx(kQuarter));
    w.set(kUcsSettingKey, "World");
    CHECK(currentUcs(&w.doc, &w.host).isWorld());
    w.set(kUcsSettingKey, "Gone");  // a deleted system falls back to the world system
    CHECK(currentUcs(&w.doc, &w.host).isWorld());
    Ucs custom{"", {3.5, -4}, 0.25};
    w.set(kUcsSettingKey, unnamedUcsSetting(custom));
    Ucs back = currentUcs(&w.doc, &w.host);
    CHECK(near(back.origin, {3.5, -4}, 1e-9));
    CHECK(back.angle == doctest::Approx(0.25));
    w.set(kUcsSettingKey, "@junk");
    CHECK(currentUcs(&w.doc, &w.host).isWorld());
}

TEST_CASE("typed coordinates are in the current UCS") {
    cmd::PointParseContext ctx;
    ctx.lastPoint = {1, 1};
    Ucs u;
    u.origin = {100, 100};
    u.angle = kQuarter;
    CHECK(near(*parseTypedPoint("10,0", ctx, std::nullopt, u), {100, 110}, 1e-9));
    CHECK(near(*parseTypedPoint("@10,0", ctx, std::nullopt, u), {1, 11}, 1e-9));
    CHECK(near(*parseTypedPoint("@10<90", ctx, std::nullopt, u), {-9, 1}, 1e-9));
    CHECK(near(*parseTypedPoint("10<0", ctx, std::nullopt, u), {100, 110}, 1e-9));
    // The world system is unchanged.
    CHECK(near(*parseTypedPoint("10,20", ctx), {10, 20}));
}

TEST_CASE("ortho, polar and grid snap follow the UCS") {
    World w;
    {
        auto tx = w.doc.begin("ucs");
        REQUIRE(saveUcs(tx, Ucs{"Tilt", {5, 5}, 30 * kPi / 180}).ok());
        (void)tx.commit();
    }
    w.set(kUcsSettingKey, "Tilt");
    const Vec2 base{0, 0};
    const Vec2 axisX{std::cos(30 * kPi / 180), std::sin(30 * kPi / 180)};

    w.set(keys::ortho, "1");
    Vec2 p = w.hover(axisX * 100 + Vec2{-axisX.y, axisX.x} * 7, base);
    CHECK(near(p, axisX * 100, 1e-6));  // locked to the UCS X axis, not the world X axis

    w.set(keys::ortho, "0");
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "45");
    Vec2 q = w.hover(Vec2{std::cos(76 * kPi / 180), std::sin(76 * kPi / 180)} * 100, base);
    // 76 degrees in the world is 46 in the UCS: it snaps to 45 UCS = 75 world degrees.
    CHECK(near(q, Vec2{std::cos(75 * kPi / 180), std::sin(75 * kPi / 180)} * 100, 1e-6));

    w.set(keys::polar, "0");
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    Ucs u = currentUcs(&w.doc, &w.host);
    Vec2 raw = u.toWorld({23, 38});
    CHECK(near(w.hover(raw), u.toWorld({20, 40}), 1e-6));
}

TEST_CASE("the adaptive grid fades smoothly and never jumps between levels") {
    // The fine level is invisible just above 8 px and fully visible from 24 px.
    GridLevels a = adaptiveGridLevels(10, 0.8001);
    CHECK(a.fineMm == doctest::Approx(10));
    CHECK(a.fineAlpha < 0.01);
    GridLevels b = adaptiveGridLevels(10, 2.4);
    CHECK(b.fineAlpha == doctest::Approx(1));
    GridLevels c = adaptiveGridLevels(10, 1.6);
    CHECK(c.fineAlpha > 0.1);
    CHECK(c.fineAlpha < 0.9);
    // Monotone in zoom.
    double last = -1;
    for (double ppm = 0.8; ppm <= 2.4; ppm += 0.05) {
        double al = adaptiveGridLevels(10, ppm).fineAlpha;
        CHECK(al >= last - 1e-12);
        last = al;
    }
    // Crossing the switch from one decade to the next: what was the coarse level (full strength)
    // becomes the fine level at almost full strength, and the old fine level is almost gone.
    GridLevels before = adaptiveGridLevels(10, 0.8001);  // fine 10 mm just above 8 px
    GridLevels after = adaptiveGridLevels(10, 0.7999);   // fine 100 mm now
    CHECK(after.fineMm == doctest::Approx(before.coarseMm));
    CHECK(after.fineAlpha > 0.99);
    CHECK(before.fineAlpha < 0.01);
    CHECK(adaptiveGridLevels(0, 1).fineMm == 0);
    CHECK(adaptiveGridLevels(10, 0).fineMm == 0);
}

TEST_CASE("a rotated grid covers the whole view") {
    render::Camera2D cam;
    cam.viewportW = 1000;
    cam.viewportH = 800;
    cam.center = {500, 400};
    cam.pixelsPerMm = 1.0;
    GridStyle st;
    st.spacingMm = 10;
    st.lines = true;
    st.origin = {37, -12};
    st.angle = 0.6;
    render::Overlay ov;
    buildGrid(ov, cam, st);
    CHECK_FALSE(ov.empty());
    CHECK_FALSE(ov.build(cam).vertices.empty());
}

TEST_CASE("dynamic UCS: ortho follows the edge under the cursor") {
    World w;
    w.line({0, 0}, {100, 100});  // a 45 degree edge
    w.set(keys::dynUcs, "1");
    w.set(keys::ortho, "1");
    const Vec2 base{50, 50};
    // Cursor next to the edge: the first leg is locked along the edge direction, not the world X.
    Vec2 p = w.hover({80, 79}, base, 1);
    CHECK(near(p, {79.5, 79.5}, 1e-6));
    CHECK(w.env.dynamicUcs.has_value());
    // Far from any edge: the world axes apply again.
    Vec2 q = w.hover({500, 20}, base, 1);
    CHECK(near(q, {500, 50}));
    CHECK_FALSE(w.env.dynamicUcs.has_value());
    // Off: no change even next to the edge.
    w.set(keys::dynUcs, "0");
    CHECK(near(w.hover({80, 79}, base, 1), {80, 50}));
}
