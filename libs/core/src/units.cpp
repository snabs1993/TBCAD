#include "tbcad/core/units.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

#include "tbcad/core/math.h"

namespace tbcad {

namespace {

std::string toFixed(double v, int precision) {
    precision = std::clamp(precision, 0, 16);
    char buf[512];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, precision);
    if (r.ec != std::errc{})
        return formatDouble(v);
    return std::string(buf, r.ptr);
}

void trimZeros(std::string& s) {
    if (s.find('.') == std::string::npos)
        return;
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
}

// "-0.000" -> "0.000"
void fixNegativeZero(std::string& s) {
    if (s.size() > 1 && s[0] == '-' &&
        std::all_of(s.begin() + 1, s.end(), [](char c) { return c == '0' || c == '.'; })) {
        s.erase(0, 1);
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Splits "<number><suffix>"; the number grammar is [+-] digits [. digits] [e [+-] digits].
bool splitNumber(std::string_view text, double& value, std::string_view& suffix) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    if (i < n && (text[i] == '+' || text[i] == '-'))
        ++i;
    std::size_t digits = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i, ++digits;
    if (i < n && text[i] == '.') {
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(text[i])))
            ++i, ++digits;
    }
    if (digits == 0)
        return false;
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < n && (text[j] == '+' || text[j] == '-'))
            ++j;
        std::size_t expDigits = 0;
        while (j < n && std::isdigit(static_cast<unsigned char>(text[j])))
            ++j, ++expDigits;
        if (expDigits > 0)
            i = j;
    }
    std::string_view num = text.substr(0, i);
    if (!num.empty() && num.front() == '+')
        num.remove_prefix(1);
    auto r = std::from_chars(num.data(), num.data() + num.size(), value);
    if (r.ec != std::errc{} || r.ptr != num.data() + num.size() || !std::isfinite(value)) {
        return false;
    }
    suffix = trim(text.substr(i));
    return true;
}

}  // namespace

std::string formatDouble(double v) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

std::string_view unitSuffix(LengthUnit u) {
    switch (u) {
        case LengthUnit::Millimeter:
            return "mm";
        case LengthUnit::Centimeter:
            return "cm";
        case LengthUnit::Meter:
            return "m";
        case LengthUnit::Inch:
            return "in";
        case LengthUnit::Foot:
            return "ft";
    }
    return "mm";
}

std::string formatLength(double mm, Precision precision, const UnitFormat& format) {
    std::string s = toFixed(mm / mmPerUnit(format.unit), precision);
    fixNegativeZero(s);
    if (format.trimTrailingZeros)
        trimZeros(s);
    if (format.showSuffix) {
        s += ' ';
        s += unitSuffix(format.unit);
    }
    return s;
}

Result<double> parseLength(std::string_view text, LengthUnit defaultUnit) {
    text = trim(text);
    double v = 0;
    std::string_view suffix;
    if (!splitNumber(text, v, suffix)) {
        return makeError(ErrorCode::Parse, "not a valid length: '" + std::string(text) + "'");
    }
    LengthUnit unit = defaultUnit;
    if (!suffix.empty()) {
        std::string s = lower(suffix);
        if (s == "mm")
            unit = LengthUnit::Millimeter;
        else if (s == "cm")
            unit = LengthUnit::Centimeter;
        else if (s == "m")
            unit = LengthUnit::Meter;
        else if (s == "in" || s == "\"")
            unit = LengthUnit::Inch;
        else if (s == "ft" || s == "'")
            unit = LengthUnit::Foot;
        else
            return makeError(ErrorCode::Parse, "unknown length unit '" + std::string(suffix) + "'");
    }
    double mm = v * mmPerUnit(unit);
    if (!std::isfinite(mm))
        return makeError(ErrorCode::OutOfRange, "length out of range");
    return mm;
}

std::string formatAngle(double radians, Precision precision, AngleUnit unit, bool showSuffix) {
    double v = radians;
    const char* suffix = "r";
    switch (unit) {
        case AngleUnit::Degrees:
            v = radians * 180.0 / kPi;
            suffix = "d";
            break;
        case AngleUnit::Gradians:
            v = radians * 200.0 / kPi;
            suffix = "g";
            break;
        case AngleUnit::Radians:
            break;
    }
    std::string s = toFixed(v, precision);
    fixNegativeZero(s);
    if (showSuffix)
        s += suffix;
    return s;
}

namespace {

// Whole part and fraction "n/d" (reduced) of a non-negative value, to 1/2^precision.
std::string fractionText(double v, Precision precision) {
    const int denom = 1 << std::clamp(precision, 0, 8);
    long long whole = static_cast<long long>(std::floor(v));
    long long num = std::llround((v - static_cast<double>(whole)) * denom);
    if (num == denom) {
        ++whole;
        num = 0;
    }
    int d = denom;
    while (num != 0 && num % 2 == 0 && d > 1) {
        num /= 2;
        d /= 2;
    }
    if (num == 0)
        return std::to_string(whole);
    const std::string frac = std::to_string(num) + "/" + std::to_string(d);
    return whole == 0 ? frac : std::to_string(whole) + " " + frac;
}

}  // namespace

std::string formatLengthType(double value, int lunits, Precision precision) {
    precision = std::clamp(precision, 0, 8);
    const bool negative = value < 0;
    const double a = std::fabs(value);
    std::string s;
    switch (lunits) {
        case 1: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*E", precision, a);
            s = buf;
            break;
        }
        case 3:
        case 4: {
            long long feet = static_cast<long long>(std::floor(a / 12.0));
            double inches = a - static_cast<double>(feet) * 12.0;
            const double step = lunits == 3 ? std::pow(10.0, -precision) : 1.0 / (1 << precision);
            inches = std::round(inches / step) * step;
            if (inches >= 12.0 - step / 2) {
                ++feet;
                inches = 0;
            }
            const std::string in =
                lunits == 3 ? toFixed(inches, precision) : fractionText(inches, precision);
            s = feet == 0 && lunits == 4 ? in + "\"" : std::to_string(feet) + "'-" + in + "\"";
            break;
        }
        case 5:
            s = fractionText(a, precision);
            break;
        default:
            s = toFixed(a, precision);
            break;
    }
    if (negative && a >= 0.5 * std::pow(10.0, -precision))
        s = "-" + s;
    return s;
}

std::string formatAngleType(double radians, int aunits, Precision precision) {
    precision = std::clamp(precision, 0, 8);
    double deg = radians * 180.0 / kPi;
    deg = std::fmod(deg, 360.0);
    if (deg < 0)
        deg += 360.0;
    auto dms = [&](double d) {
        // Degrees, then minutes and seconds as far as the precision goes (0: d, 1-2: d m, 3+: d m
        // s).
        long long totalSec = std::llround(d * 3600.0);
        long long dd = totalSec / 3600, mm = (totalSec / 60) % 60, ss = totalSec % 60;
        if (precision == 0)
            return std::to_string(std::llround(d)) + "d";
        if (precision <= 2)
            return std::to_string(dd) + "d" + std::to_string(std::llround((d - dd) * 60.0)) + "'";
        std::string out = std::to_string(dd) + "d" + std::to_string(mm) + "'";
        if (precision <= 4)
            return out + std::to_string(ss) + "\"";
        const double sec = (d - static_cast<double>(dd)) * 3600.0 - static_cast<double>(mm) * 60.0;
        return out + toFixed(sec, precision - 4) + "\"";
    };
    switch (aunits) {
        case 1:
            return dms(deg);
        case 2:
            return toFixed(deg * 400.0 / 360.0, precision) + "g";
        case 3:
            return toFixed(deg * kPi / 180.0, precision) + "r";
        case 4: {
            // Bearing from north or south towards east or west.
            const bool north = deg <= 180.0;
            const double fromNorth = north ? std::fabs(90.0 - deg) : std::fabs(270.0 - deg);
            const bool east = deg < 90.0 || deg > 270.0;
            if (std::fabs(deg - 90.0) < 1e-9)
                return "N";
            if (std::fabs(deg - 270.0) < 1e-9)
                return "S";
            if (deg < 1e-9 || std::fabs(deg - 360.0) < 1e-9)
                return "E";
            if (std::fabs(deg - 180.0) < 1e-9)
                return "W";
            return std::string(north ? "N " : "S ") + dms(fromNorth) + (east ? " E" : " W");
        }
        default: {
            std::string out = toFixed(deg, precision);
            return out;
        }
    }
}

Result<double> parseAngle(std::string_view text, AngleUnit defaultUnit) {
    text = trim(text);
    double v = 0;
    std::string_view suffix;
    if (!splitNumber(text, v, suffix)) {
        return makeError(ErrorCode::Parse, "not a valid angle: '" + std::string(text) + "'");
    }
    AngleUnit unit = defaultUnit;
    if (!suffix.empty()) {
        std::string s = lower(suffix);
        if (s == "d" || s == "deg" || s == "\xC2\xB0")
            unit = AngleUnit::Degrees;
        else if (s == "r" || s == "rad")
            unit = AngleUnit::Radians;
        else if (s == "g" || s == "grad")
            unit = AngleUnit::Gradians;
        else
            return makeError(ErrorCode::Parse, "unknown angle unit '" + std::string(suffix) + "'");
    }
    switch (unit) {
        case AngleUnit::Degrees:
            return v * kPi / 180.0;
        case AngleUnit::Gradians:
            return v * kPi / 200.0;
        case AngleUnit::Radians:
            return v;
    }
    return v;
}

}  // namespace tbcad
