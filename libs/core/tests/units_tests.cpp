#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/core/math.h"
#include "tbcad/core/units.h"

using namespace tbcad;

TEST_CASE("parseLength accepts plain numbers") {
    CHECK(parseLength("12.5").value() == doctest::Approx(12.5));
    CHECK(parseLength("1e3").value() == doctest::Approx(1000));
    CHECK(parseLength("-0.25").value() == doctest::Approx(-0.25));
    CHECK(parseLength("  +7 ").value() == doctest::Approx(7));
    CHECK(parseLength(".5").value() == doctest::Approx(0.5));
    CHECK(parseLength("5.").value() == doctest::Approx(5));
    CHECK(parseLength("2.5E-1").value() == doctest::Approx(0.25));
}

TEST_CASE("parseLength rejects malformed input") {
    for (const char* bad : {"12,5", "", " ", "abc", ".", "-", "1.2.3", "1e", "nan", "inf", "0x10",
                            "12 parsecs", "1,000"}) {
        CAPTURE(bad);
        auto r = parseLength(bad);
        CHECK_FALSE(r);
        if (!r)
            CHECK(r.error().code != ErrorCode::Generic);
    }
}

TEST_CASE("parseLength units") {
    CHECK(parseLength("3cm").value() == doctest::Approx(30));
    CHECK(parseLength("3 CM").value() == doctest::Approx(30));
    CHECK(parseLength("2m").value() == doctest::Approx(2000));
    CHECK(parseLength("1in").value() == doctest::Approx(25.4));
    CHECK(parseLength("1\"").value() == doctest::Approx(25.4));
    CHECK(parseLength("2ft").value() == doctest::Approx(609.6));
    CHECK(parseLength("2'").value() == doctest::Approx(609.6));
    CHECK(parseLength("10mm").value() == doctest::Approx(10));
    CHECK(parseLength("10", LengthUnit::Inch).value() == doctest::Approx(254));
    CHECK(parseLength("10cm", LengthUnit::Inch).value() == doctest::Approx(100));
    CHECK_FALSE(parseLength("1e400"));
}

TEST_CASE("formatLength") {
    CHECK(formatLength(12.5, 2) == "12.50");
    CHECK(formatLength(12.4, 0) == "12");
    CHECK(formatLength(-0.0000001, 3) == "0.000");
    CHECK(formatLength(25.4, 3, {LengthUnit::Inch}) == "1.000");
    CHECK(formatLength(25.4, 3, {LengthUnit::Inch, true}) == "1.000 in");
    CHECK(formatLength(1500, 4, {LengthUnit::Meter, true, true}) == "1.5 m");
    CHECK(formatLength(1000, 2, {LengthUnit::Millimeter, false, true}) == "1000");
    CHECK(formatLength(0.5, 99).size() == 18);  // precision clamped to 16 decimals
    CHECK(formatLength(1.0, -3) == "1");
}

TEST_CASE("formatLength / parseLength round trip at precision 6") {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> mant(-1.0, 1.0);
    std::uniform_int_distribution<int> expo(-3, 7);
    for (int i = 0; i < 1000; ++i) {
        double mm = mant(rng) * std::pow(10.0, expo(rng));
        std::string s = formatLength(mm, 6);
        auto back = parseLength(s);
        REQUIRE(back);
        CAPTURE(s);
        CHECK(std::abs(back.value() - mm) <= 5e-7 + 1e-12 * std::abs(mm));
    }
}

TEST_CASE("formatLength round trip in other units") {
    for (LengthUnit u :
         {LengthUnit::Centimeter, LengthUnit::Meter, LengthUnit::Inch, LengthUnit::Foot}) {
        UnitFormat f{u, true, false};
        std::string s = formatLength(1234.5678, 8, f);
        auto back = parseLength(s);
        REQUIRE(back);
        CHECK(back.value() == doctest::Approx(1234.5678).epsilon(1e-6));
    }
}

TEST_CASE("angles") {
    CHECK(parseAngle("90").value() == doctest::Approx(kPi / 2));
    CHECK(parseAngle("90d").value() == doctest::Approx(kPi / 2));
    CHECK(parseAngle("90 deg").value() == doctest::Approx(kPi / 2));
    CHECK(parseAngle("90\xC2\xB0").value() == doctest::Approx(kPi / 2));
    CHECK(parseAngle("1.5r").value() == doctest::Approx(1.5));
    CHECK(parseAngle("100g").value() == doctest::Approx(kPi / 2));
    CHECK(parseAngle("-45").value() == doctest::Approx(-kPi / 4));
    CHECK(parseAngle("2", AngleUnit::Radians).value() == doctest::Approx(2));
    CHECK(parseAngle("180d", AngleUnit::Radians).value() == doctest::Approx(kPi));
    CHECK_FALSE(parseAngle("90,5"));
    CHECK_FALSE(parseAngle("ninety"));
    CHECK_FALSE(parseAngle("90x"));

    CHECK(formatAngle(kPi / 2, 2) == "90.00");
    CHECK(formatAngle(kPi / 2, 1, AngleUnit::Degrees, true) == "90.0d");
    CHECK(formatAngle(kPi, 3, AngleUnit::Radians, true) == "3.142r");
    CHECK(formatAngle(kPi / 2, 0, AngleUnit::Gradians, true) == "100g");
    CHECK(formatAngle(-1e-9, 2) == "0.00");

    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> a(-kTwoPi, kTwoPi);
    for (int i = 0; i < 500; ++i) {
        double r = a(rng);
        auto back = parseAngle(formatAngle(r, 9, AngleUnit::Degrees, true));
        REQUIRE(back);
        CHECK(std::abs(back.value() - r) < 1e-8);
    }
}

TEST_CASE("formatDouble round trips exactly") {
    for (double v : {0.0, 1.0, -2.5, 0.1, 1e-300, 1.7976931348623157e308, 123456789.123456789}) {
        CHECK(std::stod(formatDouble(v)) == v);
    }
}
