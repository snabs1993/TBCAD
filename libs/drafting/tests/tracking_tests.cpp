#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {
Vec2 dirDeg(double deg, double r) {
    return {r * std::cos(deg * kPi / 180), r * std::sin(deg * kPi / 180)};
}
}  // namespace

TEST_CASE("ortho locks to the nearest axis through the base point") {
    World w;
    w.set(keys::ortho, "1");
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}), {53, 10}));
    CHECK(near(w.hover({14, 77}, Vec2{10, 10}), {10, 77}));
    CHECK(near(w.hover({-30, 8}, Vec2{10, 10}), {-30, 10}));
    CHECK(near(w.hover({9, -40}, Vec2{10, 10}), {10, -40}));
    // No base point: unchanged.
    CHECK(near(w.hover({53, 12}), {53, 12}));
    w.set(keys::ortho, "0");
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}), {53, 12}));
}

TEST_CASE("polar at 30 degrees: 31 degrees snaps, 35 degrees does not") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "30");
    cmd::FilterFeedback fb;
    const Vec2 base{5, 5};
    const Vec2 near31 = base + dirDeg(31, 100);
    CHECK(near(w.hover(near31, base, 1, &fb), base + dirDeg(30, 100), 1e-9));
    REQUIRE(fb.guide);
    CHECK(fb.tooltip.find("Polar: 100") == 0);
    CHECK(fb.tooltip.find("< 30") != std::string::npos);
    // The tracking ray runs along the 30 degree line through the base point.
    CHECK(std::abs(cross(normalized(fb.guide->second - fb.guide->first), dirDeg(30, 1))) < 1e-9);
    CHECK(distance(fb.guide->first, base) > 0);

    const Vec2 near35 = base + dirDeg(35, 100);
    cmd::FilterFeedback fb2;
    CHECK(near(w.hover(near35, base, 1, &fb2), near35));
    CHECK_FALSE(fb2.active);
    CHECK_FALSE(fb2.guide);
}

TEST_CASE("polar increments: 90, 45, 15 and a custom list") {
    World w;
    w.set(keys::polar, "1");
    const Vec2 base{};
    w.set(keys::polarIncrement, "90");
    CHECK(near(w.hover(dirDeg(1.5, 50), base), dirDeg(0, 50), 1e-9));
    CHECK(near(w.hover(dirDeg(92, 50), base), dirDeg(90, 50), 1e-9));
    CHECK(near(w.hover(dirDeg(45, 50), base), dirDeg(45, 50)));  // 45 is not a 90 multiple
    w.set(keys::polarIncrement, "45");
    CHECK(near(w.hover(dirDeg(44, 50), base), dirDeg(45, 50), 1e-9));
    CHECK(near(w.hover(dirDeg(-134, 50), base), dirDeg(-135, 50), 1e-9));
    w.set(keys::polarIncrement, "15");
    CHECK(near(w.hover(dirDeg(76, 50), base), dirDeg(75, 50), 1e-9));
    w.set(keys::polarIncrement, "90,25");
    CHECK(near(w.hover(dirDeg(51, 50), base), dirDeg(50, 50), 1e-9));
    CHECK(near(w.hover(dirDeg(89, 50), base), dirDeg(90, 50), 1e-9));
}

TEST_CASE("polar angle tolerance is 2 degrees") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "30");
    CHECK(near(w.hover(dirDeg(31.9, 100), Vec2{}), dirDeg(30, 100), 1e-9));
    CHECK(near(w.hover(dirDeg(32.1, 100), Vec2{}), dirDeg(32.1, 100)));
}

TEST_CASE("ortho and polar exclude each other: ortho wins when both are stored") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::ortho, "1");
    w.set(keys::polarIncrement, "30");
    // 30.5 degrees would snap to 30 under polar, but ortho locks to the x axis.
    CHECK(near(w.hover(Vec2{100, 59}, Vec2{}), {100, 0}));
}

TEST_CASE("osnap beats polar, polar beats grid snap") {
    World w;
    w.line({50, 0}, {50, 100});
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, "endpoint");
    w.set(keys::ortho, "1");
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    // Near the endpoint (50,100): osnap wins even though ortho would move the point.
    CHECK(near(w.hover({52, 97}, Vec2{0, 0}), {50, 100}));
    // Elsewhere ortho wins; the distance is rounded to the snap spacing.
    CHECK(near(w.hover({133, 8}, Vec2{0, 0}), {130, 0}));
    // No base point: only the grid snap applies.
    CHECK(near(w.hover({133, 8}), {130, 10}));
}

TEST_CASE("grid snap rounds to the spacing around the origin - off means off") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "2.5");
    CHECK(near(w.hover({3.9, -1.2}), {5, -0.0}));  // -1.2 is nearer to 0 than to -2.5
    CHECK(near(w.hover({3.9, 6.3}), {5, 7.5}));
    CHECK(near(w.hover({-3.9, -6.3}), {-5, -7.5}));
    w.set(keys::snap, "0");
    CHECK(near(w.hover({3.9, 6.3}), {3.9, 6.3}));
}

TEST_CASE("the toggles from the status bar settings reach a running command") {
    World w;
    w.host.run("TWOLINE", {"0,0"});
    w.host.runner().pointerMoved(cmd::RawInput{{80, 9}, {}, 1});
    CHECK(w.host.runner().feedback().active == false);
    w.set(keys::ortho, "1");
    w.host.runner().pointerMoved(cmd::RawInput{{80, 9}, {}, 1});
    CHECK(w.host.runner().feedback().active);
    w.host.feed({cmd::ScriptItem::click({80, 9})});
    REQUIRE(w.doc.entityCount() == 1);
    const Entity* e = nullptr;
    w.doc.forEachEntity([&](const Entity& x) { e = &x; });
    CHECK(near(entityAs<LineData>(*e)->b, {80, 0}));
}

TEST_CASE("Shift gives temporary ortho while ORTHO is off") {
    World w;
    cmd::FilterFeedback fb;
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}, 1, &fb, true), {53, 10}));
    CHECK(fb.tooltip.rfind("Ortho:", 0) == 0);
    // Released again: the cursor moves freely.
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}, 1, nullptr, false), {53, 12}));
    // No base point: Shift changes nothing.
    CHECK(near(w.hover({53, 12}, std::nullopt, 1, nullptr, true), {53, 12}));
}

TEST_CASE("Shift beats polar tracking with ortho") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "30");
    // 31 degrees snaps to 30 under polar; Shift forces the nearest axis instead.
    const Vec2 raw = dirDeg(31, 50);
    CHECK(near(w.hover(raw, Vec2{0, 0}), dirDeg(30, 50), 1e-6));
    CHECK(near(w.hover(raw, Vec2{0, 0}, 1, nullptr, true), {raw.x, 0}));
}

TEST_CASE("Shift turns ORTHO off while it is held") {
    World w;
    w.set(keys::ortho, "1");
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}), {53, 10}));
    CHECK(near(w.hover({53, 12}, Vec2{10, 10}, 1, nullptr, true), {53, 12}));
}
