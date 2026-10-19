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

TEST_CASE("tolerances: symmetrical, deviation and limits") {
    DimensionData d = aligned({0, 0}, {100, 0}, {0, 10});
    DimStyle s;
    s.tolerance = DimTolerance::Symmetrical;
    s.tolUpper = 0.05;
    CHECK(dimensionText(s, d) ==
          "100.00 \xC2\xB1"
          "0.05");
    s.tolerance = DimTolerance::Deviation;
    s.tolLower = 0.02;
    CHECK(dimensionText(s, d) == "100.00 +0.05 -0.02");
    s.tolerance = DimTolerance::Limits;
    CHECK(dimensionText(s, d) == "100.05 / 99.98");
    s.tolPrecision = 3;
    CHECK(dimensionText(s, d) == "100.050 / 99.980");
    // Prefix, suffix and a kind prefix wrap the whole tolerance form.
    DimensionData r = radial(DimKind::Radius, {0, 0}, {10, 0});
    s.tolerance = DimTolerance::Symmetrical;
    s.tolPrecision = 1;
    s.suffix = "mm";
    CHECK(dimensionText(s, r) ==
          "R10.00mm \xC2\xB1"
          "0.1");
    // "<>" in a text override stands for all of it.
    d.textOverride = "(<>)";
    s.suffix.clear();
    CHECK(dimensionText(s, d) ==
          "(100.00 \xC2\xB1"
          "0.1)");
    // Angles: the tolerance is in degrees.
    DimensionData a = angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {3, 3});
    s.tolUpper = 0.5;
    s.tolPrecision = 1;
    CHECK(dimensionText(s, a) ==
          "90.00\xC2\xB0 \xC2\xB1"
          "0.5\xC2\xB0");
    s.tolerance = DimTolerance::Limits;
    s.tolLower = 0.25;
    s.tolPrecision = 2;
    CHECK(dimensionText(s, a) == "90.50\xC2\xB0 / 89.75\xC2\xB0");
}

TEST_CASE("alternate units follow the measurement in brackets") {
    DimensionData d = aligned({0, 0}, {254, 0}, {0, 10});
    DimStyle s;
    s.altUnits = true;
    CHECK(dimensionText(s, d) == "254.00 [10.00]");
    s.altPrecision = 1;
    s.altSuffix = "in";
    CHECK(dimensionText(s, d) == "254.00 [10.0in]");
    s.altUnit = LengthUnit::Meter;
    s.altScale = 2;
    s.altPrecision = 3;
    s.altPrefix = "~";
    s.altSuffix.clear();
    CHECK(dimensionText(s, d) == "254.00 [~0.508]");
    // With a tolerance the bracket comes last; angles have no alternate form.
    s.tolerance = DimTolerance::Symmetrical;
    s.tolUpper = 0.1;
    CHECK(dimensionText(s, d) ==
          "254.00 \xC2\xB1"
          "0.10 [~0.508]");
    CHECK(dimensionText(s, angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {3, 3})) ==
          "90.00\xC2\xB0 \xC2\xB1"
          "0.10\xC2\xB0");
    s.altUnits = false;
    CHECK(dimensionText(s, d) ==
          "254.00 \xC2\xB1"
          "0.10");
}

TEST_CASE("angular dimensions: degrees, minutes and seconds, gradians, radians, own decimals") {
    DimStyle s;
    s.precision = 2;
    const double rad = kPi / 180.0;
    // Same decimals as the lengths unless DIMADEC says otherwise.
    CHECK(formatNumber(s, 90 * rad, true) == "90.00\xC2\xB0");
    s.angularPrecision = 0;
    CHECK(formatNumber(s, 90 * rad, true) == "90\xC2\xB0");
    s.angularPrecision = 1;
    CHECK(formatNumber(s, 33.25 * rad, true) == "33.2\xC2\xB0");

    s.angleFormat = DimAngleFormat::Dms;
    s.angularPrecision = 0;
    CHECK(formatNumber(s, 33.5 * rad, true) == "34\xC2\xB0");
    s.angularPrecision = 2;
    CHECK(formatNumber(s, 33.5 * rad, true) ==
          "33\xC2\xB0"
          "30'");
    s.angularPrecision = 4;
    CHECK(formatNumber(s, (33 + 30 / 60.0 + 15 / 3600.0) * rad, true) ==
          "33\xC2\xB0"
          "30'15\"");
    s.angularPrecision = 5;
    CHECK(formatNumber(s, (33 + 30 / 60.0 + 15.5 / 3600.0) * rad, true) ==
          "33\xC2\xB0"
          "30'15.5\"");
    // Rounding carries into the larger parts.
    s.angularPrecision = 2;
    CHECK(formatNumber(s, 59.9999 * rad, true) ==
          "60\xC2\xB0"
          "00'");
    s.angularPrecision = 4;
    CHECK(formatNumber(s, 10.99999 * rad, true) ==
          "11\xC2\xB0"
          "00'00\"");

    s.angleFormat = DimAngleFormat::Gradians;
    s.angularPrecision = 1;
    CHECK(formatNumber(s, kPi / 2, true) == "100.0g");
    s.angleFormat = DimAngleFormat::Radians;
    s.angularPrecision = 3;
    CHECK(formatNumber(s, kPi / 2, true) == "1.571r");
}

TEST_CASE("DIMDSEP: a decimal comma in lengths and angles") {
    DimStyle s;
    s.commaDecimal = true;
    CHECK(formatNumber(s, 12.5, false) == "12,50");
    CHECK(formatNumber(s, 90 * kPi / 180.0, true) == "90,00\xC2\xB0");
    s.precision = 0;
    CHECK(formatNumber(s, 12.6, false) == "13");
}
