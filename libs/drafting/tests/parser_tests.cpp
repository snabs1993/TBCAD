#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {
cmd::PointParseContext ctx(std::optional<Vec2> base = std::nullopt, Vec2 last = {},
                           Vec2 cursor = {}) {
    cmd::PointParseContext c;
    c.base = base;
    c.lastPoint = last;
    c.cursor = cursor;
    return c;
}
}  // namespace

TEST_CASE("absolute coordinates, including negatives and units") {
    CHECK(near(*parseTypedPoint("10,20", ctx()), {10, 20}));
    CHECK(near(*parseTypedPoint("-5.5,3", ctx()), {-5.5, 3}));
    CHECK(near(*parseTypedPoint(" 1e2 , -2 ", ctx()), {100, -2}));
    CHECK(near(*parseTypedPoint("#7,8", ctx(Vec2{0, 0}, {50, 50})), {7, 8}));
    CHECK(near(*parseTypedPoint("2cm,1m", ctx()), {20, 1000}));
}

TEST_CASE("relative coordinates are measured from the last point") {
    auto c = ctx(Vec2{0, 0}, {100, 50});
    CHECK(near(*parseTypedPoint("@10,5", c), {110, 55}));
    CHECK(near(*parseTypedPoint("@-10,-5", c), {90, 45}));
    CHECK(near(*parseTypedPoint("@", c), {100, 50}));
}

TEST_CASE("polar coordinates: absolute and relative, angle in degrees") {
    CHECK(near(*parseTypedPoint("10<90", ctx()), {0, 10}, 1e-9));
    CHECK(near(*parseTypedPoint("@10<180", ctx(Vec2{0, 0}, {5, 5})), {-5, 5}, 1e-9));
    CHECK(near(*parseTypedPoint("@10<-90", ctx(Vec2{0, 0}, {5, 5})), {5, -5}, 1e-9));
    auto c = ctx();
    c.angleUnit = AngleUnit::Radians;
    CHECK(near(*parseTypedPoint("2<1.5707963267948966", c), {0, 2}, 1e-9));
}

TEST_CASE("direct distance runs from the base point towards the cursor or the given direction") {
    auto c = ctx(Vec2{10, 10}, {10, 10}, Vec2{10, 50});
    CHECK(near(*parseTypedPoint("25", c), {10, 35}));
    CHECK(near(*parseTypedPoint("-25", c), {10, -15}));
    CHECK(near(*parseTypedPoint("25", c, Vec2{1, 0}), {35, 10}));
    CHECK(near(*parseTypedPoint("25", c, Vec2{3, 4}), {25, 30}));
    // No base, or a cursor sitting on the base: nothing to measure along.
    CHECK_FALSE(parseTypedPoint("25", ctx()));
    CHECK_FALSE(parseTypedPoint("25", ctx(Vec2{1, 1}, {}, Vec2{1, 1})));
    // A number after @ or # is not a distance.
    CHECK_FALSE(parseTypedPoint("@25", c));
    CHECK_FALSE(parseTypedPoint("#25", c));
}

TEST_CASE("garbage is not a point") {
    for (const char* t : {"", "abc", "1,", ",2", "1,2,3", "<5", "5<", "5<x", "@1,", "1;2", "#"})
        CHECK_FALSE(parseTypedPoint(t, ctx(Vec2{0, 0}, {}, Vec2{1, 0})));
}

TEST_CASE("through the command runner: typed points, direct distance, and the error message") {
    World w;
    auto r = w.host.run("TWOLINE", {"1,1", "@10,0"});
    CHECK(r.finished);
    REQUIRE(w.doc.entityCount() == 1);

    w.host.clearLog();
    r = w.host.run("TWOLINE", {"nonsense", "0,0", "1,1"});
    CHECK(r.rejected == 1);
    CHECK(w.host.logContains("Invalid point \"nonsense\""));

    // Direct distance follows the rubber band: cursor straight above the first point.
    w.host.run("TWOLINE", {"100,100"});
    w.host.runner().pointerMoved(cmd::RawInput{{100, 140}, {}, 1});
    w.host.feed({"30"});
    EntityId last;
    w.doc.forEachEntity([&](const Entity& e) { last = e.common.id; });
    const auto* l = entityAs<LineData>(*w.doc.find(last));
    REQUIRE(l);
    CHECK(near(l->b, {100, 130}));
}

TEST_CASE("direct distance uses the ortho direction, not the raw cursor direction") {
    World w;
    w.set(keys::ortho, "1");
    w.host.run("TWOLINE", {"0,0"});
    // Raw cursor is mostly right and a little up; ortho locks to +X.
    w.host.runner().pointerMoved(cmd::RawInput{{60, 20}, {}, 1});
    w.host.feed({"15"});
    EntityId last;
    w.doc.forEachEntity([&](const Entity& e) { last = e.common.id; });
    const auto* l = entityAs<LineData>(*w.doc.find(last));
    REQUIRE(l);
    CHECK(near(l->b, {15, 0}));
}

TEST_CASE("snap mode names parse, including prefixes") {
    CHECK(parseSnapMode("END") == SnapMode::Endpoint);
    CHECK(parseSnapMode("mid") == SnapMode::Midpoint);
    CHECK(parseSnapMode("cen") == SnapMode::Center);
    CHECK(parseSnapMode("per") == SnapMode::Perpendicular);
    CHECK_FALSE(parseSnapMode("n"));  // nearest or node
    CHECK(parseSnapModes("endpoint, midpoint") == (maskOf(SnapMode::Endpoint) | maskOf(SnapMode::Midpoint)));
    CHECK(parseSnapModes("all") == kAllSnapModes);
    CHECK(parseSnapModes("none") == SnapModeMask{0});
    CHECK_FALSE(parseSnapModes("endpoint,bogus"));
    CHECK(formatSnapModes(0) == "none");
    CHECK(*parseSnapModes(formatSnapModes(kAllSnapModes)) == kAllSnapModes);
}

TEST_CASE("settings default sensibly and ortho excludes polar") {
    World w;
    w.host.setSetting(keys::osnap, "1");
    auto s = DraftingSettings::read(w.host);
    CHECK(s.osnap);
    CHECK_FALSE(s.ortho);
    CHECK(s.modes == kDefaultSnapModes);
    CHECK(s.polarIncrementsDeg == std::vector<double>{90});
    w.set(keys::ortho, "1");
    w.set(keys::polar, "1");
    s = DraftingSettings::read(w.host);
    CHECK(s.ortho);
    CHECK_FALSE(s.polar);
    w.set(keys::polarIncrement, "30, 45");
    CHECK(DraftingSettings::read(w.host).polarIncrementsDeg == std::vector<double>{30, 45});
    w.set(keys::polarIncrement, "junk");
    CHECK(DraftingSettings::read(w.host).polarIncrementsDeg == std::vector<double>{90});
}
