#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tbcad/dim/dim.h"
#include "tbcad/text/text.h"

namespace tbcad::dim {

namespace {

const DimStyle& defaultStyle() {
    static const DimStyle s{.id = kDimStyleStandard, .name = "Standard"};
    return s;
}

// Which zeros a number drops: `zeros` is one of DimStyle's DIMAZIN / DIMTZIN / DIMALTZ fields
// (-1: the style's length setting).
void suppressZeros(std::string& s, const DimStyle& style, int zeros = -1) {
    if (s.find('.') == std::string::npos)
        return;
    const bool trailing = zeros < 0 ? style.suppressTrailingZeros : (zeros & 2) != 0;
    const bool leading = zeros < 0 ? style.suppressLeadingZeros : (zeros & 1) != 0;
    if (trailing) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    if (leading) {
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

int childStyleCode(DimKind kind) {
    switch (kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            return 0;
        case DimKind::Angular2Line:
        case DimKind::Angular3Point:
        case DimKind::ArcLength:
            return 2;
        case DimKind::Diameter:
            return 3;
        case DimKind::Radius:
        case DimKind::Jogged:
            return 4;
        case DimKind::Ordinate:
            return 6;
    }
    return -1;
}

std::string childStyleName(const std::string& parent, int code) {
    return parent + "$" + std::to_string(code);
}

namespace {
const DimStyle& childOf(const Document& doc, const DimStyle& base, int code) {
    // A child or an override is never a parent itself.
    if (code < 0 || base.name.find('$') != std::string::npos)
        return base;
    if (const DimStyle* c = doc.dimStyleByName(childStyleName(base.name, code)))
        return *c;
    return base;
}
}  // namespace

const DimStyle& drawStyleOf(const Document& doc, const DimensionData& d) {
    return childOf(doc, styleOf(doc, d.style), childStyleCode(d.kind));
}

const DimStyle& leaderStyleOf(const Document& doc, DimStyleId id) {
    return childOf(doc, styleOf(doc, id), 7);
}

DimStyleId overrideStyle(Transaction& tx, DimStyleId base,
                         const std::function<void(DimStyle&)>& change) {
    const Document& doc = tx.document();
    const DimStyle from = styleOf(doc, base);
    DimStyle derived = from;
    change(derived);
    if (derived == from)
        return from.id;
    std::string root = from.name;
    if (auto at = root.rfind('$'); at != std::string::npos && at > 0)
        root.resize(at);
    // An override with the same settings is shared.
    for (const auto& [id, st] : doc.dimStyles()) {
        if (st.name.rfind(root + "$", 0) != 0)
            continue;
        DimStyle same = derived;
        same.id = st.id;
        same.name = st.name;
        if (same == st)
            return id;
    }
    // Two digits: "$0".."$9" are left to AutoCAD's child styles (childStyleCode).
    int n = 10;
    while (doc.dimStyleByName(root + "$" + std::to_string(n)))
        ++n;
    derived.id = {};
    derived.name = root + "$" + std::to_string(n);
    auto added = tx.addDimStyle(std::move(derived));
    return added ? added.value() : DimStyleId{};
}

// Degrees, minutes and seconds. Decimals 0..1 show degrees, 2..3 add minutes, 4 and more add
// seconds (with decimals - 4 places), as DIMADEC does for DIMAUNIT 1. The value is rounded to
// the finest part shown, so 59.9999 degrees becomes 60 and carries.
static std::string formatDms(double radians, int decimals) {
    const double deg = radians * 180.0 / kPi;
    const bool neg = deg < 0;
    const double a = std::abs(deg);
    const int secDecimals = decimals >= 4 ? std::min(decimals - 4, 6) : 0;
    double scale = 1;  // parts per degree at the finest unit shown
    if (decimals >= 4) {
        scale = 3600.0;
        for (int i = 0; i < secDecimals; ++i)
            scale *= 10;
    } else if (decimals >= 2) {
        scale = 60.0;
    }
    long long total = std::llround(a * scale);
    const long long perDeg = static_cast<long long>(scale);
    long long d = total / perDeg;
    long long rest = total % perDeg;
    std::string out = neg && total != 0 ? "-" : "";
    out += std::to_string(d) + "\xC2\xB0";
    if (decimals >= 2) {
        long long unitsPerMinute = decimals >= 4 ? perDeg / 60 : 1;
        long long m = rest / unitsPerMinute;
        rest %= unitsPerMinute;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02lld", m);
        out += buf;
        out += "'";
        if (decimals >= 4) {
            long long unitsPerSecondFrac = unitsPerMinute / 60;  // 10^secDecimals
            long long sec = rest / unitsPerSecondFrac;
            long long frac = rest % unitsPerSecondFrac;
            std::snprintf(buf, sizeof buf, "%02lld", sec);
            out += buf;
            if (secDecimals > 0) {
                char fb[16];
                std::snprintf(fb, sizeof fb, "%0*lld", secDecimals, frac);
                out += ".";
                out += fb;
            }
            out += "\"";
        }
    }
    return out;
}

// Inches the way an architect writes them: whole inches and a reduced fraction of 1/2^precision.
// `feet` splits off the feet (12'-6 1/2"); otherwise everything stays in inches (150 1/2").
static std::string formatImperial(double mm, int precision, bool feet) {
    const int denom = 1 << std::clamp(precision, 0, 8);
    const bool neg = mm < 0;
    long long parts = std::llround(std::abs(mm) / 25.4 * denom);
    long long whole = parts / denom;
    long long num = parts % denom;
    long long den = denom;
    while (num != 0 && num % 2 == 0) {
        num /= 2;
        den /= 2;
    }
    std::string out = neg && parts != 0 ? "-" : "";
    std::string frac = num != 0 ? std::to_string(num) + "/" + std::to_string(den) : "";
    if (feet) {
        const long long ft = whole / 12;
        whole %= 12;
        out += std::to_string(ft) + "'-" + std::to_string(whole);
        if (!frac.empty())
            out += " " + frac;
    } else if (whole != 0 || frac.empty()) {
        out += std::to_string(whole);
        if (!frac.empty())
            out += " " + frac;
    } else {
        out += frac;
    }
    return out + "\"";
}

std::string formatNumber(const DimStyle& style, double value, bool angular) {
    std::string s;
    if (angular) {
        const int decimals = style.angularPrecision >= 0 ? style.angularPrecision : style.precision;
        switch (style.angleFormat) {
            case DimAngleFormat::Dms:
                return formatDms(value, decimals);
            case DimAngleFormat::Gradians:
                s = formatAngle(value, decimals, AngleUnit::Gradians, false);
                break;
            case DimAngleFormat::Radians:
                s = formatAngle(value, decimals, AngleUnit::Radians, false);
                break;
            default:
                s = formatAngle(value, decimals, AngleUnit::Degrees, false);
                break;
        }
    } else {
        if (style.roundOff > 0)
            value = std::round(value / style.roundOff) * style.roundOff;
        if (style.linearFormat != DimLinearFormat::Decimal)
            return formatImperial(value, style.precision,
                                  style.linearFormat == DimLinearFormat::Architectural);
        UnitFormat f;
        f.unit = style.units;
        s = formatLength(value, style.precision, f);
    }
    suppressZeros(s, style, angular ? style.angularZeros : -1);
    if (style.commaDecimal)
        for (char& c : s)
            if (c == '.')
                c = ',';
    if (angular) {
        switch (style.angleFormat) {
            case DimAngleFormat::Gradians:
                s += "g";
                break;
            case DimAngleFormat::Radians:
                s += "r";
                break;
            default:
                s += "\xC2\xB0";  // °
                break;
        }
    }
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
    suppressZeros(s, style, style.tolZeros);
    if (angular)
        s += "\xC2\xB0";
    return s;
}

}  // namespace

namespace {

// The text of a dimension in three parts: what comes before the tolerance stack, the stack
// itself (empty when the tolerance is not stacked) and what follows it.
struct TextParts {
    std::string before{}, up{}, down{}, after{};
};

TextParts dimensionParts(const DimStyle& style, const DimensionData& d, bool stack) {
    const bool angular = isAngular(d.kind);
    // DIMLFAC scales lengths, never angles.
    const double m = angular ? measurement(d) : measurement(d) * style.linearFactor;
    TextParts t;
    std::string num;
    if (style.tolerance == DimTolerance::Limits) {
        // The two limits replace the measurement: the larger, then the smaller.
        const double toMeasure = angular ? kPi / 180 : 1;  // degrees to radians
        const double hi = m + style.tolUpper * toMeasure;
        const double lo = m - style.tolLower * toMeasure;
        DimStyle limitStyle = style;
        limitStyle.precision = style.tolPrecision;
        if (stack) {
            t.up = formatNumber(limitStyle, hi, angular);
            t.down = formatNumber(limitStyle, lo, angular);
        } else {
            num = formatNumber(limitStyle, hi, angular) + " / " +
                  formatNumber(limitStyle, lo, angular);
        }
    } else {
        num = formatNumber(style, m, angular);
    }
    // A prefix from the style replaces the radius and diameter signs, as in AutoCAD.
    std::string head =
        (style.prefix.empty() ? std::string(kindPrefix(d.kind)) : style.prefix) + num;
    std::string tail = style.suffix;
    switch (style.tolerance) {
        case DimTolerance::Symmetrical:
            tail += " \xC2\xB1" + toleranceNumber(style, style.tolUpper, angular);
            break;
        case DimTolerance::Deviation:
            if (stack) {
                // Upper over lower, after a small space, like AutoCAD stacks them.
                head += tail + " ";
                tail.clear();
                t.up = "+" + toleranceNumber(style, style.tolUpper, angular);
                t.down = "-" + toleranceNumber(style, style.tolLower, angular);
            } else {
                tail += " +" + toleranceNumber(style, style.tolUpper, angular) + " -" +
                        toleranceNumber(style, style.tolLower, angular);
            }
            break;
        case DimTolerance::Limits:
        case DimTolerance::None:
            break;
    }
    if (style.altUnits && !angular) {
        UnitFormat f;
        f.unit = style.altUnit;
        std::string alt = formatLength(m * style.altScale, style.altPrecision, f);
        suppressZeros(alt, style, style.altZeros);
        tail += " [" + style.altPrefix + alt + style.altSuffix + "]";
    }
    t.before = std::move(head);
    t.after = std::move(tail);
    if (d.textOverride.empty())
        return t;
    const std::size_t at = d.textOverride.find("<>");
    if (at == std::string::npos)
        return {d.textOverride, {}, {}, {}};
    t.before = d.textOverride.substr(0, at) + t.before;
    t.after += d.textOverride.substr(at + 2);
    return t;
}

bool stacks(const DimStyle& style) {
    return style.tolerance == DimTolerance::Deviation || style.tolerance == DimTolerance::Limits;
}

}  // namespace

std::string dimensionText(const DimStyle& style, const DimensionData& d) {
    const TextParts t = dimensionParts(style, d, false);
    return t.before + t.after;
}

std::optional<StackedText> stackedDimensionText(const DimStyle& style, const DimensionData& d) {
    if (!stacks(style))
        return std::nullopt;
    TextParts t = dimensionParts(style, d, true);
    if (t.up.empty() && t.down.empty())
        return std::nullopt;  // an override without "<>" replaces everything
    StackedText s;
    // Stack parts may not hold the stack separators themselves.
    auto esc = [](const std::string& v) {
        std::string o;
        for (char c : v) {
            if (c == '^' || c == '/' || c == '#' || c == '\\' || c == ';')
                o += '\\';
            o += c;
        }
        return o;
    };
    // MTEXT draws a stack at 0.7 of the height around it: DIMTFAC sets the height of each line,
    // and \A1 keeps the measurement centred on the stack.
    std::string stackMarkup = "\\S" + esc(t.up) + "^" + esc(t.down) + ";";
    const double f = style.tolHeightFactor / 0.7;
    if (std::abs(f - 1) > 1e-9) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", f);
        stackMarkup = std::string("{\\H") + buf + "x;" + stackMarkup + "}";
    }
    s.markup =
        "\\A1;" + text::markupFromPlain(t.before) + stackMarkup + text::markupFromPlain(t.after);
    s.before = std::move(t.before);
    s.up = std::move(t.up);
    s.down = std::move(t.down);
    s.after = std::move(t.after);
    return s;
}

}  // namespace tbcad::dim
