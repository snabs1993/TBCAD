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

// A length written as UNITS says (LUNITS): 1 scientific "1.5000E+01", 2 decimal "15.0000",
// 3 engineering "1'-3.0000\"", 4 architectural "1'-3 1/2\"", 5 fractional "15 1/2". `value` is in
// drawing units; engineering and architectural take it as inches. For the fractional forms
// `precision` is the power of two of the smallest fraction (4 = sixteenths).
std::string formatLengthType(double value, int lunits, Precision precision);
// An angle written as UNITS says (AUNITS): 0 decimal degrees "45.0", 1 "45d30'0\"", 2 grads "50g",
// 3 radians "0.7854r", 4 surveyor's bearing "N 45d0' E". `radians` is counted the usual way (from
// east, counter-clockwise).
std::string formatAngleType(double radians, int aunits, Precision precision);

// Shortest decimal string that round-trips the double (locale independent).
std::string formatDouble(double v);

}  // namespace tbcad
