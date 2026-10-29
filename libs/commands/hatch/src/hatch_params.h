#pragma once

// Hatch settings shared by the commands and the UI (private to this module).

#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/math.h"
#include "tbcad/hatch/boundary.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/hatch/pattern.h"
#include "tbcad/hatchcmd/hatchcmd.h"

namespace tbcad::hatchcmd {

struct Params {
    std::string pattern = "LINE45";
    double scale = 1;
    bool scaleAuto = true;  // never set by the user: sized to the area when a hatch is made
    double angle = 0;       // radians
    Vec2 base{};
    double gap = 0;  // mm; 0 = boundaries must be closed
    hatch::IslandStyle island = hatch::IslandStyle::Normal;
    bool associative = true;  // the hatch follows its boundary objects
    GradientData gradient{};  // used by GRADIENT
    bool separate = false;    // one hatch per picked area
    enum class Order { None, Back, Front, Behind, InFront };
    Order order = Order::Behind;
    bool annotative = false;
    bool doubled = false;
    std::int8_t transparency = kTransparencyByLayer;
};

inline const char* orderWord(Params::Order o) {
    switch (o) {
        case Params::Order::None:
            return "none";
        case Params::Order::Back:
            return "back";
        case Params::Order::Front:
            return "front";
        case Params::Order::InFront:
            return "infront";
        default:
            return "behind";
    }
}

inline Params::Order orderFromWord(const std::string& w) {
    for (auto o : {Params::Order::None, Params::Order::Back, Params::Order::Front,
                   Params::Order::Behind, Params::Order::InFront})
        if (w == orderWord(o))
            return o;
    return Params::Order::Behind;
}

inline std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

inline bool parseNumber(const std::string& s, double& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && std::isfinite(out);
}

inline std::optional<Color> parseColor(const std::string& text) {
    std::string t = text;
    while (!t.empty() && t.front() == ' ')
        t.erase(t.begin());
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    if (t.empty())
        return std::nullopt;
    if (t[0] == '#' && t.size() == 7) {
        unsigned v = 0;
        auto [ptr, ec] = std::from_chars(t.data() + 1, t.data() + 7, v, 16);
        if (ec != std::errc() || ptr != t.data() + 7)
            return std::nullopt;
        return Color::rgb(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
                          static_cast<std::uint8_t>(v));
    }
    auto comma = t.find(',');
    if (comma == std::string::npos) {
        double d = 0;
        if (!parseNumber(t, d) || d < 1 || d > 255 || d != std::floor(d))
            return std::nullopt;
        return Color::index(static_cast<std::uint8_t>(d));
    }
    auto c2 = t.find(',', comma + 1);
    double r = 0, g = 0, b = 0;
    if (c2 == std::string::npos || !parseNumber(t.substr(0, comma), r) ||
        !parseNumber(t.substr(comma + 1, c2 - comma - 1), g) || !parseNumber(t.substr(c2 + 1), b))
        return std::nullopt;
    auto ok = [](double v) { return v >= 0 && v <= 255 && v == std::floor(v); };
    if (!ok(r) || !ok(g) || !ok(b))
        return std::nullopt;
    return Color::rgb(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                      static_cast<std::uint8_t>(b));
}

inline std::string colorText(const Color& c) {
    if (c.kind == ColorKind::Index)
        return std::to_string(c.indexValue());
    return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
}

// The gradient kinds as command keywords, in GradientKind order (1..9).
inline const std::vector<std::string> kGradientKeywords = {
    "Linear",        "Cylinder",         "INVCylinder", "Spherical", "INVSpherical",
    "Hemispherical", "INVHemispherical", "CUrved",      "INVCUrved"};

inline const char* gradientKeyword(GradientKind k) {
    const auto i = static_cast<std::size_t>(k);
    return i >= 1 && i <= kGradientKeywords.size() ? kGradientKeywords[i - 1].c_str() : "Linear";
}

inline GradientKind gradientFromKeyword(const std::string& k) {
    for (std::size_t i = 0; i < kGradientKeywords.size(); ++i)
        if (kGradientKeywords[i] == k)
            return static_cast<GradientKind>(i + 1);
    return GradientKind::Linear;
}

inline std::string transparencyText(std::int8_t t) {
    return t == kTransparencyByLayer   ? "ByLayer"
           : t == kTransparencyByBlock ? "ByBlock"
                                       : std::to_string(t);
}

inline Params loadParams(const cmd::IHost& host) {
    Params p;
    if (auto v = host.setting(kSettingPattern); v && hatch::PatternRegistry::find(*v))
        p.pattern = hatch::PatternRegistry::find(*v)->name;
    double d = 0;
    if (auto v = host.setting(kSettingScale); v && parseNumber(*v, d) && d > 0) {
        p.scale = d;
        p.scaleAuto = false;
    }
    if (auto v = host.setting(kSettingAngle); v && parseNumber(*v, d))
        p.angle = d * kPi / 180.0;
    if (auto v = host.setting(kSettingBase)) {
        auto comma = v->find(',');
        double x = 0, y = 0;
        if (comma != std::string::npos && parseNumber(v->substr(0, comma), x) &&
            parseNumber(v->substr(comma + 1), y))
            p.base = {x, y};
    }
    if (auto v = host.setting(kSettingGap); v && parseNumber(*v, d) && d >= 0)
        p.gap = d;
    if (auto v = host.setting(kSettingIsland))
        p.island = *v == "outer"    ? hatch::IslandStyle::Outer
                   : *v == "ignore" ? hatch::IslandStyle::Ignore
                                    : hatch::IslandStyle::Normal;
    if (auto v = host.setting(kSettingAssociative))
        p.associative = *v != "0";
    if (auto v = host.setting(kSettingGradientKind))
        p.gradient.kind = hatch::gradientKindFromName(*v).value_or(GradientKind::Linear);
    if (auto v = host.setting(kSettingGradientShift); v && parseNumber(*v, d) && d >= 0 && d <= 1)
        p.gradient.shift = d;
    if (auto v = host.setting(kSettingGradientTint); v && parseNumber(*v, d) && d >= 0 && d <= 1)
        p.gradient.tint = d;
    if (auto v = host.setting(kSettingGradientOneColor))
        p.gradient.oneColor = *v == "1";
    if (auto v = host.setting(kSettingSeparate))
        p.separate = *v == "1";
    if (auto v = host.setting(kSettingDrawOrder))
        p.order = orderFromWord(*v);
    if (auto v = host.setting(kSettingAnnotative))
        p.annotative = *v == "1";
    if (auto v = host.setting(kSettingDouble))
        p.doubled = *v == "1";
    if (auto v = host.setting(kSettingTransparency)) {
        if (*v == "ByBlock")
            p.transparency = kTransparencyByBlock;
        else if (parseNumber(*v, d) && d >= 0 && d <= 90)
            p.transparency = static_cast<std::int8_t>(d);
    }
    if (auto v = host.setting(kSettingGradientColor1))
        if (auto c = parseColor(*v))
            p.gradient.color1 = *c;
    if (auto v = host.setting(kSettingGradientColor2))
        if (auto c = parseColor(*v))
            p.gradient.color2 = *c;
    return p;
}

inline void saveParams(cmd::IHost& host, const Params& p) {
    host.setSetting(kSettingPattern, p.pattern);
    if (!p.scaleAuto)
        host.setSetting(kSettingScale, number(p.scale));
    host.setSetting(kSettingAngle, number(p.angle * 180.0 / kPi));
    host.setSetting(kSettingBase, number(p.base.x) + "," + number(p.base.y));
    host.setSetting(kSettingGap, number(p.gap));
    host.setSetting(kSettingIsland, p.island == hatch::IslandStyle::Outer    ? "outer"
                                    : p.island == hatch::IslandStyle::Ignore ? "ignore"
                                                                             : "normal");
    host.setSetting(kSettingAssociative, p.associative ? "1" : "0");
    host.setSetting(kSettingGradientKind, hatch::gradientName(p.gradient.kind));
    host.setSetting(kSettingGradientShift, number(p.gradient.shift));
    host.setSetting(kSettingGradientTint, number(p.gradient.tint));
    host.setSetting(kSettingGradientOneColor, p.gradient.oneColor ? "1" : "0");
    host.setSetting(kSettingSeparate, p.separate ? "1" : "0");
    host.setSetting(kSettingDrawOrder, orderWord(p.order));
    host.setSetting(kSettingAnnotative, p.annotative ? "1" : "0");
    host.setSetting(kSettingDouble, p.doubled ? "1" : "0");
    host.setSetting(kSettingTransparency, transparencyText(p.transparency));
    host.setSetting(kSettingGradientColor1, colorText(p.gradient.color1));
    host.setSetting(kSettingGradientColor2, colorText(p.gradient.color2));
}

// Every field saveParams writes.
inline bool sameSettings(const Params& a, const Params& b) {
    const auto& ga = a.gradient;
    const auto& gb = b.gradient;
    return a.pattern == b.pattern && a.scale == b.scale && a.scaleAuto == b.scaleAuto &&
           a.angle == b.angle && a.base == b.base && a.gap == b.gap && a.island == b.island &&
           a.associative == b.associative && a.separate == b.separate && a.order == b.order &&
           a.annotative == b.annotative && a.doubled == b.doubled &&
           a.transparency == b.transparency && ga.kind == gb.kind && ga.shift == gb.shift &&
           ga.tint == gb.tint && ga.oneColor == gb.oneColor && ga.color1 == gb.color1 &&
           ga.color2 == gb.color2;
}

inline std::string summary(const Params& p) {
    return p.pattern + ", scale " + number(p.scale) + ", angle " + number(p.angle * 180.0 / kPi) +
           (p.gap > 0 ? ", gap " + number(p.gap) : "") +
           (p.island == hatch::IslandStyle::Outer    ? ", islands: outer"
            : p.island == hatch::IslandStyle::Ignore ? ", islands: ignore"
                                                     : "") +
           (p.associative ? "" : ", not linked") + (p.separate ? ", separate" : "") +
           (p.annotative ? ", annotative" : "") + (p.doubled ? ", double" : "");
}

}  // namespace tbcad::hatchcmd
