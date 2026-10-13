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
