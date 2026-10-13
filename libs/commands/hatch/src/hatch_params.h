#pragma once

// Hatch settings shared by the commands and the UI (private to this module).

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>

#include "tbcad/cmd/host.h"
#include "tbcad/core/math.h"
#include "tbcad/hatch/pattern.h"
#include "tbcad/hatchcmd/hatchcmd.h"

namespace tbcad::hatchcmd {

struct Params {
    std::string pattern = "LINE45";
    double scale = 1;
    double angle = 0;  // radians
    Vec2 base{};
    double gap = 0;           // mm; 0 = boundaries must be closed
    bool associative = true;  // the hatch follows its boundary objects
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

inline Params loadParams(const cmd::IHost& host) {
    Params p;
    if (auto v = host.setting(kSettingPattern); v && hatch::PatternRegistry::find(*v))
        p.pattern = hatch::PatternRegistry::find(*v)->name;
    double d = 0;
    if (auto v = host.setting(kSettingScale); v && parseNumber(*v, d) && d > 0)
        p.scale = d;
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
    return p;
}

inline void saveParams(cmd::IHost& host, const Params& p) {
    host.setSetting(kSettingPattern, p.pattern);
    host.setSetting(kSettingScale, number(p.scale));
    host.setSetting(kSettingAngle, number(p.angle * 180.0 / kPi));
    host.setSetting(kSettingBase, number(p.base.x) + "," + number(p.base.y));
    host.setSetting(kSettingGap, number(p.gap));
    host.setSetting(kSettingAssociative, p.associative ? "1" : "0");
}

inline std::string summary(const Params& p) {
    return p.pattern + ", scale " + number(p.scale) + ", angle " + number(p.angle * 180.0 / kPi) +
           (p.gap > 0 ? ", gap " + number(p.gap) : "") + (p.associative ? "" : ", not linked");
}

}  // namespace tbcad::hatchcmd
