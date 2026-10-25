// Grid snap options: separate X and Y spacing, a snap base point, and polar snap.
#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

TEST_CASE("snap with different X and Y spacing") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    w.set(keys::snapSpacingY, "4");
    CHECK(near(w.hover({13, 5.1}), {10, 4}));
    CHECK(near(w.hover({16, 6.1}), {20, 8}));
    // unset Y follows X
    w.host.setSetting(keys::snapSpacingY, "");
    CHECK(near(w.hover({13, 5.1}), {10, 10}));
}

TEST_CASE("the snap lattice passes through the snap base") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    w.set(keys::snapBase, "3,4");
    CHECK(near(w.hover({14, 13}), {13, 14}));
    CHECK(near(w.hover({-6, -5}), {-7, -6}));
    // a bad base is ignored
    w.set(keys::snapBase, "x,1");
    CHECK(near(w.hover({14, 13}), {10, 10}));
}

TEST_CASE("SNAPANG turns the snap lattice") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    w.set(keys::snapAngle, "90");
    // a lattice turned a quarter: still on multiples of 10 along the turned axes
    CHECK(near(w.hover({14, 13}), {10, 10}));
    w.set(keys::snapAngle, "45");
    const double r = std::sqrt(0.5);
    CHECK(near(w.hover(Vec2{10 * r, 10 * r} + Vec2{0.3, -0.2}), {10 * r, 10 * r}, 1e-9));
    w.set(keys::snapAngle, "0");
    CHECK(near(w.hover({14, 13}), {10, 10}));
}

TEST_CASE("isometric snap uses the Y spacing and the base") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::isoDraft, "1");
    w.set(keys::isoPlane, "top");
    w.set(keys::snapSpacing, "100");
    w.set(keys::snapSpacingY, "10");
    w.set(keys::snapBase, "1,2");
    const auto [a1, a2] = isoAxesDeg(IsoPlane::Top);
    const Vec2 b1{std::cos(a1 * kPi / 180), std::sin(a1 * kPi / 180)};
    const Vec2 b2{std::cos(a2 * kPi / 180), std::sin(a2 * kPi / 180)};
    const Vec2 lattice = Vec2{1, 2} + b1 * 20.0 + b2 * 10.0;
    CHECK(near(w.hover(lattice + Vec2{1, 1}), lattice, 1e-9));
}

TEST_CASE("polar snap: the free cursor is not snapped, tracking steps by the polar distance") {
    World w;
    w.set(keys::snap, "1");
    w.set(keys::snapSpacing, "10");
    w.set(keys::snapType, "polar");
    w.set(keys::polarDistance, "7");
    // no grid lattice
    CHECK(near(w.hover({13, 5.1}), {13, 5.1}));
    // along polar tracking: steps of 7
    w.set(keys::polar, "1");
    CHECK(near(w.hover({20.5, 0.2}, Vec2{0, 0}), {21, 0}, 1e-9));
    // distance 0 falls back to the snap spacing
    w.set(keys::polarDistance, "0");
    CHECK(near(w.hover({20.5, 0.2}, Vec2{0, 0}), {20, 0}, 1e-9));
    // grid type again: the lattice is back
    w.set(keys::snapType, "grid");
    w.set(keys::polar, "0");
    CHECK(near(w.hover({13, 5.1}), {10, 10}));
}

TEST_CASE("grid spacing 0 follows the snap X spacing") {
    World w;
    w.set(keys::snapSpacing, "2.5");
    w.set(keys::gridSpacing, "0");
    CHECK(DraftingSettings::read(w.host).gridSpacing == 2.5);
    w.set(keys::gridSpacing, "-3");  // invalid: default
    CHECK(DraftingSettings::read(w.host).gridSpacing == 10);
}
