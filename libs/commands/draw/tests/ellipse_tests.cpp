#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

TEST_CASE("ELLIPSE from axis endpoints and the other half-axis") {
    Fixture f;
    CHECK(f.host.run("EL", {"0,0", "100,0", "30"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.center, {50, 0}));
    CHECK(close(e.majorAxis, {50, 0}));
    CHECK(close(e.ratio, 0.6));
    CHECK(closeAngle(e.startParam, 0));
    CHECK(e.endParam >= kTwoPi - 1e-9);
}

TEST_CASE("ELLIPSE with a tilted axis") {
    Fixture f;
    f.host.run("ELLIPSE", {"0,0", "30,40", "10"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.center, {15, 20}));
    CHECK(close(e.majorAxis, {15, 20}));
    CHECK(close(e.ratio, 10.0 / 25.0));
}

TEST_CASE("ELLIPSE: a longer other axis becomes the major axis") {
    Fixture f;
    f.host.run("EL", {"0,0", "100,0", "80"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.center, {50, 0}));
    CHECK(close(e.majorAxis, {0, 80}));
    CHECK(close(e.ratio, 50.0 / 80.0));
}

TEST_CASE("ELLIPSE Center") {
    Fixture f;
    f.host.run("EL", {"Center", "0,0", "50,0", "25"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.center, {0, 0}));
    CHECK(close(e.majorAxis, {50, 0}));
    CHECK(close(e.ratio, 0.5));
}

TEST_CASE("ELLIPSE Rotation gives the ratio as cos(angle)") {
    Fixture f;
    f.host.run("EL", {"0,0", "100,0", "Rotation", "60"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(indirect<EllipseData>(es[0]).ratio, 0.5));
}

TEST_CASE("ELLIPSE Rotation of 90 degrees is refused") {
    Fixture f;
    f.host.run("EL", {"0,0", "100,0", "R", "90"});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("between 0 and 89.9"));
}

TEST_CASE("ELLIPSE Arc") {
    Fixture f;
    CHECK(f.host.run("EL", {"Arc", "0,0", "100,0", "30", "0", "90"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.ratio, 0.6));
    CHECK(closeAngle(e.startParam, 0));
    CHECK(closeAngle(e.endParam, kPi / 2));
}

TEST_CASE("ELLIPSE Arc with a picked end point converts the direction to a parameter") {
    Fixture f;
    f.host.run("EL", {"A", "0,0", "100,0", "25", "0", ScriptItem::click({50, 25})});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    // The centre is (50,0), so the picked point (50,25) is straight above it: 90 degrees.
    CHECK(closeAngle(e.endParam, kPi / 2));
}

TEST_CASE("ELLIPSE Arc at 45 degrees") {
    Fixture f;
    f.host.run("EL", {"Arc", "Center", "0,0", "40,0", "20", "0", ScriptItem::click({10, 10})});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    // Direction 45 deg: tan(t) = tan(45)/ratio -> t = atan2(1/0.5, 1).
    CHECK(closeAngle(e.endParam, std::atan2(2.0, 1.0)));
}

TEST_CASE("ELLIPSE with a zero axis is refused - Esc draws nothing") {
    Fixture f;
    f.host.run("EL", {"0,0", "0,0"});
    CHECK(f.host.logContains("zero length"));
    CHECK(f.entities().empty());
    f.host.run("EL", {"0,0", "10,0", ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("ELLIPSE previews while the other axis moves and is one undo step") {
    Fixture f;
    f.host.run("EL", {"0,0", "100,0"});
    cmd::RawInput in;
    in.worldCursor = {50, 20};
    f.host.runner().pointerMoved(in);
    CHECK_FALSE(f.host.preview().items.empty());
    f.host.feed({"20"});
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE("ELLIPSE Isocircle draws a circle in the current isometric plane") {
    {
        // Not offered while isometric drafting is off: the word is not a point.
        Fixture off;
        CHECK_FALSE(off.host.run("EL", {"Isocircle"}).finished);
        CHECK(off.entities().empty());
    }
    Fixture f;
    f.host.setSetting("drafting.isodraft", "1");
    f.host.setSetting("drafting.isoplane", "Top");
    CHECK(f.host.run("EL", {"Isocircle", "10,20", "5"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& e = indirect<EllipseData>(es[0]);
    CHECK(close(e.center, {10, 20}));
    CHECK(e.ratio == doctest::Approx(1 / std::sqrt(3.0)));
    CHECK(std::abs(length(e.majorAxis) - 5 * std::sqrt(1.5)) < 1e-9);
    CHECK(std::abs(e.majorAxis.y) < 1e-9);  // top plane: horizontal major axis
    f.host.setSetting("drafting.isoplane", "Left");
    f.host.run("EL", {"Isocircle", "0,0", "5"});
    es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& l = indirect<EllipseData>(es[1]);
    CHECK(std::abs(std::atan2(l.majorAxis.y, l.majorAxis.x) - 120 * kPi / 180) < 1e-9);
}
