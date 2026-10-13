#include <doctest/doctest.h>

#include "tbcad/ui/radial_wheel.h"

using namespace tbcad::ui;

TEST_CASE("wheel tool list parsing") {
    CHECK(parseWheelTools("") == defaultWheelTools());
    auto t = parseWheelTools(" line ,circle,,move");
    REQUIRE(t.size() == kWheelSlots);
    CHECK(t[0] == "LINE");
    CHECK(t[2].empty());
    CHECK(t[3] == "MOVE");
    CHECK(t[7].empty());
    CHECK(parseWheelTools("a,b,c,d,e,f,g,h,i,j").size() == kWheelSlots);
    CHECK(parseWheelTools(formatWheelTools(t)) == t);
    CHECK(parseWheelTools(formatWheelTools(defaultWheelTools())) == defaultWheelTools());
}

TEST_CASE("wheel opens only after the delay") {
    CHECK_FALSE(wheelShouldOpen(100, 250));
    CHECK(wheelShouldOpen(250, 250));
}

TEST_CASE("wheel slices are clockwise from the top") {
    CHECK(wheelSliceAt(0, -100, 20) == 0);   // up
    CHECK(wheelSliceAt(100, 0, 20) == 2);    // right
    CHECK(wheelSliceAt(0, 100, 20) == 4);    // down
    CHECK(wheelSliceAt(-100, 0, 20) == 6);   // left
    CHECK(wheelSliceAt(70, -70, 20) == 1);   // up-right
    CHECK(wheelSliceAt(-1, -100, 20) == 0);  // just left of up is still slice 0
    CHECK(wheelSliceAt(5, 5, 20) == -1);     // dead zone
    CHECK(wheelSliceAt(0, -100, 20, 0) == -1);
    CHECK(wheelSliceAt(0, -100, 20, 4) == 0);
    CHECK(wheelSliceAt(100, 0, 20, 4) == 1);
}
