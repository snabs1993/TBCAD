#pragma once

// Hatch settings shared by the commands and the UI (private to this module).

#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "tbcad/cmd/host.h"
#include "tbcad/core/math.h"
#include "tbcad/hatch/pattern.h"
#include "tbcad/hatchcmd/hatchcmd.h"

namespace tbcad::hatchcmd {

struct Params {
    std::string pattern = "LINE45";
    double scale = 1;
    bool scaleAuto = true;  // never set by the user: sized to the area when a hatch is made
    double angle = 0;       // radians
    Vec2 base{};
    double gap = 0;           // mm; 0 = boundaries must be closed
    bool associative = true;  // the hatch follows its boundary objects
    GradientData gradient{};  // used by GRADIENT
};

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
    if (auto v = host.setting(kSettingAssociative))
        p.associative = *v != "0";
    if (auto v = host.setting(kSettingGradientKind))
        p.gradient.kind = *v == "cylinder" ? GradientKind::Cylinder : GradientKind::Linear;
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
    host.setSetting(kSettingAssociative, p.associative ? "1" : "0");
    host.setSetting(kSettingGradientKind,
                    p.gradient.kind == GradientKind::Cylinder ? "cylinder" : "linear");
    host.setSetting(kSettingGradientColor1, colorText(p.gradient.color1));
    host.setSetting(kSettingGradientColor2, colorText(p.gradient.color2));
}

inline std::string summary(const Params& p) {
    return p.pattern + ", scale " + number(p.scale) + ", angle " + number(p.angle * 180.0 / kPi) +
           (p.gap > 0 ? ", gap " + number(p.gap) : "") + (p.associative ? "" : ", not linked");
}

}  // namespace tbcad::hatchcmd
