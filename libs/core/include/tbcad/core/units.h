#pragma once

#include <string>
#include <string_view>

#include "tbcad/core/result.h"

namespace tbcad {

enum class LengthUnit { Millimeter, Centimeter, Meter, Inch, Foot };
enum class AngleUnit { Degrees, Radians, Gradians };

// Number of decimal places, clamped to 0..16 when formatting.
using Precision = int;

struct UnitFormat {
    LengthUnit unit = LengthUnit::Millimeter;
    bool showSuffix = false;
    bool trimTrailingZeros = false;
};

constexpr double mmPerUnit(LengthUnit u) {
    switch (u) {
        case LengthUnit::Millimeter:
            return 1.0;
        case LengthUnit::Centimeter:
            return 10.0;
        case LengthUnit::Meter:
            return 1000.0;
        case LengthUnit::Inch:
            return 25.4;
        case LengthUnit::Foot:
            return 304.8;
    }
    return 1.0;
}

std::string_view unitSuffix(LengthUnit u);

// Formats a length given in millimetres in the requested unit. Always uses '.' as decimal mark.
std::string formatLength(double mm, Precision precision, const UnitFormat& format = {});

// Parses "12.5", "-0.25", "1e3", optionally followed by a unit (mm, cm, m, in, ", ft, ').
// Without a unit suffix the number is taken in defaultUnit. Returns millimetres.
Result<double> parseLength(std::string_view text, LengthUnit defaultUnit = LengthUnit::Millimeter);

// Formats an angle given in radians.
std::string formatAngle(double radians, Precision precision, AngleUnit unit = AngleUnit::Degrees,
                        bool showSuffix = false);

// Parses a number with optional suffix d (degrees), r (radians) or g (gradians).
// Without suffix the number is taken in defaultUnit. Returns radians.
Result<double> parseAngle(std::string_view text, AngleUnit defaultUnit = AngleUnit::Degrees);

// Shortest decimal string that round-trips the double (locale independent).
std::string formatDouble(double v);

}  // namespace tbcad
