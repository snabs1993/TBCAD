#include "support.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

TEST_CASE("measurement equals the point distance and rounds to the style precision") {
    Document doc;
    DimensionData d = aligned({0, 0}, {30, 40}, {0, 10});
    CHECK(measurement(d) == doctest::Approx(50));
    CHECK(dimensionText(styleOf(doc, {}), d) == "50.00");

    DimensionData h = linear({0, 0}, {100.126, 7}, {0, 10});
    CHECK(measurement(h) == doctest::Approx(100.126));
    CHECK(dimensionText(styleOf(doc, {}), h) == "100.13");

    DimensionData v = linear({0, 0}, {5, 25}, {10, 0}, kPi / 2);
    CHECK(measurement(v) == doctest::Approx(25));
}

TEST_CASE("number formatting: precision, zero suppression, round-off, units, prefix, suffix") {
    DimStyle s;
    CHECK(formatNumber(s, 12.5, false) == "12.50");
    s.precision = 0;
    CHECK(formatNumber(s, 12.6, false) == "13");
    s.precision = 3;
    s.suppressTrailingZeros = true;
    CHECK(formatNumber(s, 12.5, false) == "12.5");
    CHECK(formatNumber(s, 12, false) == "12");
    s.suppressLeadingZeros = true;
    CHECK(formatNumber(s, 0.5, false) == ".5");
    CHECK(formatNumber(s, -0.5, false) == "-.5");
    s = DimStyle{};
    s.roundOff = 5;
    s.precision = 0;
    CHECK(formatNumber(s, 103, false) == "105");
    s = DimStyle{};
    s.units = LengthUnit::Meter;
    s.precision = 3;
    CHECK(formatNumber(s, 2500, false) == "2.500");
    s.units = LengthUnit::Inch;
    s.precision = 1;
    CHECK(formatNumber(s, 25.4, false) == "1.0");
}

TEST_CASE("dimension text: prefixes, suffix, override") {
    Document doc;
    DimStyle s;
    s.prefix = "~";
    s.suffix = " mm";
    DimensionData lin = linear({0, 0}, {10, 0}, {0, 5});
    CHECK(dimensionText(s, lin) == "~10.00 mm");
    lin.textOverride = "<> typ.";
    CHECK(dimensionText(s, lin) == "~10.00 mm typ.");
    lin.textOverride = "ABC";
    CHECK(dimensionText(s, lin) == "ABC");

    DimStyle plain;
    CHECK(dimensionText(plain, radial(DimKind::Radius, {0, 0}, {10, 0})) == "R10.00");
    CHECK(dimensionText(plain, radial(DimKind::Diameter, {-10, 0}, {10, 0})) ==
          "\xC3\x98"
          "20.00");
}

TEST_CASE("angular dimension of perpendicular lines reads 90 degrees") {
    DimensionData d = angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {7, 7});
    CHECK(measurement(d) == doctest::Approx(kPi / 2));
    CHECK(dimensionText(DimStyle{}, d) == "90.00\xC2\xB0");
    d.textPos = {-7, 7};
    CHECK(measurement(d) == doctest::Approx(kPi / 2));
    d.textPos = {7, 0.1};  // sector between the +x ray and the +y ray, a sliver
    CHECK(measurement(d) == doctest::Approx(kPi / 2));

    // 30 degree lines: the arc position picks the acute or the obtuse sector.
    DimensionData e = angular2({0, 0}, {10, 0}, {0, 0}, {10 * std::cos(kPi / 6), 5}, {8, 2});
    CHECK(measurement(e) == doctest::Approx(kPi / 6));
    e.textPos = {-8, 2};
    CHECK(measurement(e) == doctest::Approx(kPi - kPi / 6));

    DimensionData three;
    three.kind = DimKind::Angular3Point;
    three.defPts = {Vec2{10, 0}, Vec2{0, 10}, Vec2{0, 0}, Vec2{5, 5}};
    CHECK(measurement(three) == doctest::Approx(kPi / 2));
}
