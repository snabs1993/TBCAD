#include <cmath>

#include "tbcad/dim/dim.h"

namespace tbcad::dim {

namespace {

const DimStyle& defaultStyle() {
    static const DimStyle s{.id = kDimStyleStandard, .name = "Standard"};
    return s;
}

void suppressZeros(std::string& s, const DimStyle& style) {
    if (s.find('.') == std::string::npos)
        return;
    if (style.suppressTrailingZeros) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    if (style.suppressLeadingZeros) {
        std::size_t at = (!s.empty() && s[0] == '-') ? 1 : 0;
        if (s.compare(at, 2, "0.") == 0)
            s.erase(at, 1);
    }
    if (s.empty() || s == "-")
        s = "0";
}

const char* kindPrefix(DimKind k) {
    switch (k) {
        case DimKind::Radius:
        case DimKind::Jogged:
            return "R";
        case DimKind::Diameter:
            return "\xC3\x98";  // U+00D8 (Ø); the bundled font has it, U+2300 it does not
        default:
            return "";
    }
}

bool isAngular(DimKind k) {
    return k == DimKind::Angular2Line || k == DimKind::Angular3Point;
}

}  // namespace

const DimStyle& styleOf(const Document& doc, DimStyleId id) {
    if (const DimStyle* s = doc.dimStyle(id))
        return *s;
    if (const DimStyle* s = doc.dimStyle(doc.header().currentDimStyle))
        return *s;
    return defaultStyle();
}

std::string formatNumber(const DimStyle& style, double value, bool angular) {
    std::string s;
    if (angular) {
        s = formatAngle(value, style.precision, AngleUnit::Degrees, false);
    } else {
        if (style.roundOff > 0)
            value = std::round(value / style.roundOff) * style.roundOff;
        UnitFormat f;
        f.unit = style.units;
        s = formatLength(value, style.precision, f);
    }
    suppressZeros(s, style);
    if (angular)
        s += "\xC2\xB0";  // °
    return s;
}

double measurement(const DimensionData& d) {
    const auto& p = d.defPts;
    switch (d.kind) {
        case DimKind::Linear: {
            Vec2 u{std::cos(d.rotation), std::sin(d.rotation)};
            return std::abs(dot(p[1] - p[0], u));
        }
        case DimKind::Aligned:
        case DimKind::Diameter:
            return distance(p[0], p[1]);
        case DimKind::Radius:
            return distance(p[0], p[1]);
        case DimKind::Angular3Point:
        case DimKind::Angular2Line: {
            AngularGeometry g = angularGeometry(d);
            return g.ok ? g.sweep : 0;
        }
        case DimKind::ArcLength: {
            ArcSpan g = arcSpan(d);
            return g.ok ? g.arcRadius * g.sweep : 0;
        }
        case DimKind::Jogged:
            return distance(p[0], p[1]);
        case DimKind::Ordinate: {
            // along the X or Y axis of the UCS the dimension was made in (oblique is its angle)
            const Vec2 axis = d.rotation < kPi / 4
                                  ? Vec2{std::cos(d.oblique), std::sin(d.oblique)}
                                  : Vec2{-std::sin(d.oblique), std::cos(d.oblique)};
            return std::abs(dot(p[0] - p[2], axis));
        }
    }
    return 0;
}

namespace {

// A tolerance value (millimetres, or degrees for an angular dimension) the way the style writes
// numbers, with the tolerance precision.
std::string toleranceNumber(const DimStyle& style, double value, bool angular) {
    std::string s;
    if (angular) {
        s = formatAngle(value * kPi / 180, style.tolPrecision, AngleUnit::Degrees, false);
    } else {
        UnitFormat f;
        f.unit = style.units;
        s = formatLength(value, style.tolPrecision, f);
    }
    suppressZeros(s, style);
    if (angular)
        s += "\xC2\xB0";
    return s;
}

}  // namespace

std::string dimensionText(const DimStyle& style, const DimensionData& d) {
    const bool angular = isAngular(d.kind);
    const double m = measurement(d);
    std::string num;
    if (style.tolerance == DimTolerance::Limits) {
        // The two limits replace the measurement: the larger, then the smaller.
        const double toMeasure = angular ? kPi / 180 : 1;  // degrees to radians
        const double hi = m + style.tolUpper * toMeasure;
        const double lo = m - style.tolLower * toMeasure;
        DimStyle limitStyle = style;
        limitStyle.precision = style.tolPrecision;
        num = formatNumber(limitStyle, hi, angular) + " / " + formatNumber(limitStyle, lo, angular);
    } else {
        num = formatNumber(style, m, angular);
    }
    num = std::string(kindPrefix(d.kind)) + style.prefix + num + style.suffix;
    switch (style.tolerance) {
        case DimTolerance::Symmetrical:
            num += " \xC2\xB1" + toleranceNumber(style, style.tolUpper, angular);
            break;
        case DimTolerance::Deviation:
            num += " +" + toleranceNumber(style, style.tolUpper, angular) + " -" +
                   toleranceNumber(style, style.tolLower, angular);
            break;
        case DimTolerance::Limits:
        case DimTolerance::None:
            break;
    }
    if (style.altUnits && !angular) {
        UnitFormat f;
        f.unit = style.altUnit;
        std::string alt = formatLength(m * style.altScale, style.altPrecision, f);
        suppressZeros(alt, style);
        num += " [" + style.altPrefix + alt + style.altSuffix + "]";
    }
    if (d.textOverride.empty())
        return num;
    std::string out = d.textOverride;
    std::size_t at = out.find("<>");
    if (at == std::string::npos)
        return out;
    out.replace(at, 2, num);
    return out;
}

}  // namespace tbcad::dim
