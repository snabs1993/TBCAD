#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {
Vec2 dirDeg(double deg, double r) {
    return {r * std::cos(deg * kPi / 180), r * std::sin(deg * kPi / 180)};
}

std::vector<cmd::CursorField> fieldsOf(World& w, Vec2 cursor, std::optional<Vec2> base) {
    cmd::FilterFeedback fb;
    w.hover(cursor, base, 1, &fb);
    return w.dyn.fields(fb);
}
}  // namespace

TEST_CASE("without locks the filter does nothing and the fields follow the cursor") {
    World w;
    auto f = fieldsOf(w, {30, 40}, Vec2{0, 0});
    REQUIRE(f.size() == 2);
    CHECK(f[0].label == "Distance (mm)");
    CHECK(f[0].text == "50");
    CHECK(f[1].label == "Angle");
    CHECK(f[1].text.rfind("53.13", 0) == 0);
    CHECK_FALSE(f[0].locked);
    CHECK(near(w.hover({30, 40}, Vec2{0, 0}), {30, 40}));
}

TEST_CASE("no base point: X and Y fields") {
    World w;
    auto f = fieldsOf(w, {12.5, -3}, std::nullopt);
    REQUIRE(f.size() == 2);
    CHECK(f[0].label == "X");
    CHECK(f[0].text == "12.5");
    CHECK(f[1].label == "Y");
    CHECK(f[1].text == "-3");
}

TEST_CASE("a locked distance keeps the direction of the cursor") {
    World w;
    fieldsOf(w, {30, 40}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "100"));
    CHECK(near(w.hover({30, 40}, Vec2{0, 0}), {60, 80}));
    CHECK(near(w.hover({0, 5}, Vec2{0, 0}), {0, 100}));
    auto f = fieldsOf(w, {0, 5}, Vec2{0, 0});
    CHECK(f[0].locked);
    CHECK(f[0].text == "100");
    CHECK_FALSE(f[1].locked);
}

TEST_CASE("a locked angle keeps the distance along the ray") {
    World w;
    fieldsOf(w, {30, 40}, Vec2{10, 10});
    REQUIRE(w.dyn.edit(1, "90"));
    CHECK(near(w.hover({50, 60}, Vec2{10, 10}), {10, 60}, 1e-9));
    // Behind the base point the distance stays at zero.
    CHECK(near(w.hover({50, -60}, Vec2{10, 10}), {10, 10}, 1e-9));
}

TEST_CASE("both locked: an exact point - X and Y lock one coordinate each") {
    World w;
    fieldsOf(w, {1, 1}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "10"));
    REQUIRE(w.dyn.edit(1, "30"));
    CHECK(near(w.hover({1, 1}, Vec2{0, 0}), dirDeg(30, 10), 1e-9));

    World v;
    fieldsOf(v, {5, 6}, std::nullopt);
    REQUIRE(v.dyn.edit(0, "12"));
    CHECK(near(v.hover({5, 6}), {12, 6}));
    REQUIRE(v.dyn.edit(1, "-4"));
    CHECK(near(v.hover({5, 6}), {12, -4}));
}

TEST_CASE("lockedPoint exists only when every field is locked") {
    World w;
    cmd::FilterFeedback fb;
    w.hover({30, 40}, Vec2{1, 1}, 1, &fb);
    CHECK_FALSE(w.dyn.lockedPoint(fb));
    REQUIRE(w.dyn.edit(0, "10"));
    CHECK_FALSE(w.dyn.lockedPoint(fb));
    REQUIRE(w.dyn.edit(1, "90"));
    auto p = w.dyn.lockedPoint(fb);
    REQUIRE(p);
    CHECK(near(*p, {1, 11}, 1e-9));
}

TEST_CASE("empty text unlocks - unreadable text is refused and changes nothing") {
    World w;
    fieldsOf(w, {30, 40}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "100"));
    CHECK_FALSE(w.dyn.edit(0, "abc"));
    CHECK_FALSE(w.dyn.edit(0, "-5"));  // a distance is not negative
    CHECK(w.dyn.hasLocks());
    CHECK(w.dyn.edit(0, ""));
    CHECK_FALSE(w.dyn.hasLocks());
    CHECK(near(w.hover({30, 40}, Vec2{0, 0}), {30, 40}));
}

TEST_CASE("a locked value beats object snap") {
    World w;
    w.set(keys::osnap, "1");
    w.line({0, 0}, {50, 0});
    fieldsOf(w, {49, 1}, Vec2{0, 20});
    REQUIRE(w.dyn.edit(0, "7"));
    const Vec2 p = w.hover({49, 1}, Vec2{0, 20});
    CHECK(std::abs(distance(p, {0, 20}) - 7) < 1e-9);
}

TEST_CASE("locks end at the click and when the base point changes") {
    World w;
    fieldsOf(w, {30, 40}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "100"));
    CHECK(near(w.hover({30, 40}, Vec2{0, 0}, 1, nullptr, false, true), {60, 80}));
    CHECK_FALSE(w.dyn.hasLocks());

    REQUIRE(w.dyn.edit(1, "45"));
    w.hover({30, 40}, Vec2{5, 5});  // a new base point: the old locks are stale
    CHECK_FALSE(w.dyn.hasLocks());
}

TEST_CASE("reset clears locks and hides the fields - the setting switches them off") {
    World w;
    fieldsOf(w, {30, 40}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "100"));
    w.dyn.reset();
    CHECK_FALSE(w.dyn.hasLocks());
    cmd::FilterFeedback fb;
    fb.hasPoint = true;
    fb.point = {1, 1};
    CHECK(w.dyn.fields(fb).empty());  // no prompt seen since the reset

    fieldsOf(w, {30, 40}, Vec2{0, 0});
    CHECK(w.dyn.fields(fb).size() == 2);
    w.set(keys::dynInput, "0");
    CHECK(w.dyn.fields(fb).empty());
    CHECK_FALSE(w.dyn.focusNext());
}

TEST_CASE("focus goes through the fields and back to none on reset") {
    World w;
    CHECK_FALSE(w.dyn.focusNext());  // no prompt yet: Tab belongs to object snap
    fieldsOf(w, {30, 40}, Vec2{0, 0});
    CHECK(w.dyn.focused() == -1);
    CHECK(w.dyn.focusNext());
    CHECK(w.dyn.focused() == 0);
    CHECK(w.dyn.focusNext());
    CHECK(w.dyn.focused() == 1);
    CHECK(w.dyn.focusNext());
    CHECK(w.dyn.focused() == 0);
    w.dyn.reset();
    CHECK(w.dyn.focused() == -1);
}

TEST_CASE("values are typed in the unit of the drawing") {
    World w;
    {
        auto tx = w.doc.begin("units");
        REQUIRE(tx.modifyHeader([](Header& h) { h.units = LengthUnit::Centimeter; }).ok());
        (void)tx.commit();
    }
    fieldsOf(w, {30, 0}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "5"));  // 5 cm
    CHECK(near(w.hover({30, 0}, Vec2{0, 0}), {50, 0}));
    auto f = fieldsOf(w, {30, 0}, Vec2{0, 0});
    CHECK(f[0].text == "5");
}

TEST_CASE("a typed distance follows ORTHO instead of the raw cursor direction") {
    World w;
    w.set(keys::ortho, "1");
    fieldsOf(w, {30, 10}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "100"));
    CHECK(near(w.hover({30, 10}, Vec2{0, 0}), {100, 0}));
    CHECK(near(w.hover({-5, 40}, Vec2{0, 0}), {0, 100}));
    CHECK(near(w.hover({-40, -5}, Vec2{0, 0}), {-100, 0}));
    // Shift switches ORTHO off for the move: the raw direction comes back.
    CHECK(near(w.hover({30, 40}, Vec2{0, 0}, 1, nullptr, true), {60, 80}));
}

TEST_CASE("a typed distance follows polar tracking when the cursor is near an increment") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "45");
    fieldsOf(w, {30, 29}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(0, "10"));
    const double r = 10 / std::sqrt(2.0);
    CHECK(near(w.hover({30, 29}, Vec2{0, 0}), {r, r}));
    // Far from every increment the raw direction is kept.
    const Vec2 raw = dirDeg(20, 50);
    CHECK(near(w.hover(raw, Vec2{0, 0}), dirDeg(20, 10), 1e-9));
}

TEST_CASE("a typed angle beats ORTHO and a typed distance beats an object snap") {
    World w;
    w.set(keys::ortho, "1");
    w.set(keys::osnap, "1");
    w.line({100, 0}, {100, 50});
    fieldsOf(w, {30, 10}, Vec2{0, 0});
    REQUIRE(w.dyn.edit(1, "45"));
    const Vec2 p = w.hover({30, 10}, Vec2{0, 0});
    CHECK(near(p, dirDeg(45, dot(Vec2{30, 10}, dirDeg(45, 1))), 1e-9));
    REQUIRE(w.dyn.edit(0, "100"));
    CHECK(near(w.hover({99, 1}, Vec2{0, 0}), dirDeg(45, 100), 1e-9));
}

TEST_CASE("without a lock ORTHO and an endpoint snap still agree: the snap wins") {
    World w;
    w.set(keys::ortho, "1");
    w.set(keys::osnap, "1");
    w.line({100, 0}, {100, 50});
    CHECK(near(w.hover({99, 1}, Vec2{0, 0}), {100, 0}));
}

TEST_CASE("a whole coordinate typed into a field fixes the point: relative after the first") {
    World w;
    cmd::FilterFeedback fb;
    // first point: absolute
    w.hover({1, 1}, std::nullopt, 1, &fb);
    REQUIRE(w.dyn.edit(0, "10,5"));
    w.hover({1, 1}, std::nullopt, 1, &fb);
    REQUIRE(w.dyn.lockedPoint(fb));
    CHECK(near(*w.dyn.lockedPoint(fb), {10, 5}));
    w.dyn.reset();

    // after the first point: relative to the base
    const Vec2 base{100, 50};
    w.hover({120, 60}, base, 1, &fb);
    REQUIRE(w.dyn.edit(0, "10,5"));
    CHECK(near(w.hover({0, 0}, base, 1, &fb), {110, 55}, 1e-9));
    REQUIRE(w.dyn.lockedPoint(fb));
    CHECK(near(*w.dyn.lockedPoint(fb), {110, 55}, 1e-9));
    // # makes it absolute, @ and polar forms work
    REQUIRE(w.dyn.edit(1, "#10,5"));
    CHECK(near(w.hover({0, 0}, base, 1, &fb), {10, 5}, 1e-9));
    REQUIRE(w.dyn.edit(0, "20<90"));
    CHECK(near(w.hover({0, 0}, base, 1, &fb), {100, 70}, 1e-9));
    REQUIRE(w.dyn.edit(0, "@-3,0"));
    CHECK(near(w.hover({0, 0}, base, 1, &fb), {97, 50}, 1e-9));
    // the absolute setting makes plain x,y absolute
    w.set(keys::dynInputAbsolute, "1");
    REQUIRE(w.dyn.edit(0, "10,5"));
    CHECK(near(w.hover({0, 0}, base, 1, &fb), {10, 5}, 1e-9));
    // nonsense is refused
    CHECK_FALSE(w.dyn.edit(0, "a,b"));
}
