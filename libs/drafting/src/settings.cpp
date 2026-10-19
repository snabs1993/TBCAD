#include "tbcad/drafting/settings.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace tbcad::drafting {

namespace {

constexpr std::array<std::string_view, kSnapModeCount> kNames = {
    "endpoint", "intersection", "midpoint",  "center",   "quadrant",  "perpendicular", "tangent",
    "nearest",  "node",         "insertion", "apparent", "extension", "parallel"};
constexpr std::array<std::string_view, kSnapModeCount> kLabels = {"Endpoint",
                                                                  "Intersection",
                                                                  "Midpoint",
                                                                  "Center",
                                                                  "Quadrant",
                                                                  "Perpendicular",
                                                                  "Tangent",
                                                                  "Nearest",
                                                                  "Node",
                                                                  "Insertion",
                                                                  "Apparent intersection",
                                                                  "Extension",
                                                                  "Parallel"};

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

bool toDouble(const std::string& s, double& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size())
        return false;
    out = v;
    return true;
}

}  // namespace

std::pair<double, double> isoAxesDeg(IsoPlane plane) {
    switch (plane) {
        case IsoPlane::Left:
            return {90.0, 150.0};
        case IsoPlane::Top:
            return {30.0, 150.0};
        case IsoPlane::Right:
            return {30.0, 90.0};
        default:
            return {0.0, 0.0};
    }
}

double isoNormalDeg(IsoPlane plane) {
    switch (plane) {
        case IsoPlane::Left:
            return 30.0;
        case IsoPlane::Top:
            return 90.0;
        case IsoPlane::Right:
            return 150.0;
        default:
            return 0.0;
    }
}

std::string_view isoPlaneName(IsoPlane plane) {
    switch (plane) {
        case IsoPlane::Left:
            return "Left";
        case IsoPlane::Top:
            return "Top";
        case IsoPlane::Right:
            return "Right";
        default:
            return "Off";
    }
}

std::optional<IsoPlane> parseIsoPlane(std::string_view text) {
    const std::string t = lower(text);
    if (t.empty())
        return std::nullopt;
    for (IsoPlane p : {IsoPlane::Left, IsoPlane::Top, IsoPlane::Right}) {
        const std::string n = lower(isoPlaneName(p));
        if (n.compare(0, t.size(), t) == 0)
            return p;
    }
    return std::nullopt;
}

IsoPlane nextIsoPlane(IsoPlane plane) {
    switch (plane) {
        case IsoPlane::Left:
            return IsoPlane::Top;
        case IsoPlane::Top:
            return IsoPlane::Right;
        default:
            return IsoPlane::Left;
    }
}

std::string_view snapModeName(SnapMode m) {
    return kNames[static_cast<std::size_t>(m)];
}

std::string_view snapModeLabel(SnapMode m) {
    return kLabels[static_cast<std::size_t>(m)];
}

std::optional<SnapMode> parseSnapMode(std::string_view text) {
    std::string t = lower(text);
    if (t.empty())
        return std::nullopt;
    std::optional<SnapMode> prefix;
    int prefixHits = 0;
    for (int i = 0; i < kSnapModeCount; ++i) {
        if (kNames[static_cast<std::size_t>(i)] == t)
            return static_cast<SnapMode>(i);
        if (kNames[static_cast<std::size_t>(i)].substr(0, t.size()) == t) {
            prefix = static_cast<SnapMode>(i);
            ++prefixHits;
        }
    }
    // "cen" is the usual short form of center, "end", "mid", "int", "per", "tan", "nea", "qua".
    if (prefixHits == 1)
        return prefix;
    return std::nullopt;
}

std::string formatSnapModes(SnapModeMask mask) {
    std::string out;
    for (int i = 0; i < kSnapModeCount; ++i)
        if (mask & maskOf(static_cast<SnapMode>(i))) {
            if (!out.empty())
                out += ',';
            out += kNames[static_cast<std::size_t>(i)];
        }
    return out.empty() ? "none" : out;
}

std::optional<SnapModeMask> parseSnapModes(std::string_view text) {
    SnapModeMask mask = 0;
    for (const std::string& w : words(text)) {
        std::string l = lower(w);
        if (l == "all") {
            mask |= kAllSnapModes;
        } else if (l == "none" || l == "off") {
            // contributes nothing
        } else if (auto m = parseSnapMode(l)) {
            mask |= maskOf(*m);
        } else {
            return std::nullopt;
        }
    }
    return mask;
}

bool parseBoolSetting(std::string_view text, bool fallback) {
    std::string t = lower(text);
    if (t == "1" || t == "true" || t == "on" || t == "yes")
        return true;
    if (t == "0" || t == "false" || t == "off" || t == "no")
        return false;
    return fallback;
}

std::vector<double> parsePolarIncrements(std::string_view text) {
    std::vector<double> out;
    for (const std::string& w : words(text)) {
        double v = 0;
        if (!toDouble(w, v) || !(v > 0) || v > 180)
            return {};
        out.push_back(v);
    }
    return out;
}

std::optional<std::vector<double>> parsePolarAngles(std::string_view text) {
    std::vector<double> out;
    for (const std::string& w : words(text)) {
        double v = 0;
        if (!toDouble(w, v) || !std::isfinite(v) || v < -360 || v > 360)
            return std::nullopt;
        out.push_back(v);
    }
    return out;
}

DraftingSettings DraftingSettings::read(const cmd::IHost& host) {
    DraftingSettings s;
    auto get = [&](const char* key) { return host.setting(key); };
    auto boolOf = [&](const char* key, bool fallback) {
        auto v = get(key);
        return v ? parseBoolSetting(*v, fallback) : fallback;
    };
    auto numOf = [&](const char* key, double fallback, double minExclusive) {
        auto v = get(key);
        double d = 0;
        if (v && toDouble(*v, d) && d > minExclusive)
            return d;
        return fallback;
    };
    s.osnap = boolOf(keys::osnap, true);
    s.ortho = boolOf(keys::ortho, false);
    s.polar = boolOf(keys::polar, false);
    s.otrack = boolOf(keys::otrack, false);
    s.snap = boolOf(keys::snap, false);
    s.dynUcs = boolOf(keys::dynUcs, false);
    s.grid = boolOf(keys::grid, false);
    if (boolOf(keys::isoDraft, false)) {
        s.isoPlane = IsoPlane::Top;
        if (auto p = get(keys::isoPlane))
            if (auto plane = parseIsoPlane(*p))
                s.isoPlane = *plane;
    }
    s.dynInput = boolOf(keys::dynInput, true);
    s.dynInputAbsolute = boolOf(keys::dynInputAbsolute, false);
    if (auto m = get(keys::osnapModes))
        if (auto mask = parseSnapModes(*m))
            s.modes = *mask;
    // A snap override from the right-click menu replaces the running modes for one point.
    if (auto once = get(keys::osnapOnce); once && !once->empty())
        if (auto mask = parseSnapModes(*once)) {
            s.osnap = true;
            s.modes = *mask;
        }
    s.aperturePx = numOf(keys::osnapAperture, 10, 0);
    s.snapSpacing = numOf(keys::snapSpacing, 10, 0);
    s.snapSpacingY = numOf(keys::snapSpacingY, s.snapSpacing, 0);
    if (auto b = get(keys::snapBase)) {
        const auto parts = words(*b);
        double x = 0, y = 0;
        if (parts.size() == 2 && toDouble(parts[0], x) && toDouble(parts[1], y) &&
            std::isfinite(x) && std::isfinite(y))
            s.snapBase = {x, y};
    }
    if (auto t = get(keys::snapType))
        s.polarSnap = lower(*t) == "polar";
    s.polarDistance = numOf(keys::polarDistance, 0, 0);
    s.gridSpacing = numOf(keys::gridSpacing, 10, 0);
    // A grid spacing of 0 follows the snap spacing, like AutoCAD's GRIDUNIT 0,0.
    if (auto g = get(keys::gridSpacing)) {
        double d = -1;
        if (toDouble(*g, d) && d == 0)
            s.gridSpacing = s.snapSpacing;
    }
    if (auto st = get(keys::gridStyle))
        s.gridLines = lower(*st) == "lines";
    if (auto p = get(keys::polarIncrement)) {
        auto inc = parsePolarIncrements(*p);
        if (!inc.empty())
            s.polarIncrementsDeg = std::move(inc);
    }
    if (auto p = get(keys::polarAdditional))
        if (auto a = parsePolarAngles(*p))
            s.polarAdditionalDeg = std::move(*a);
    s.polarRelative = boolOf(keys::polarRelative, false);
    // Ortho and polar tracking exclude each other; if both are stored, ortho wins.
    if (s.ortho)
        s.polar = false;
    return s;
}

}  // namespace tbcad::drafting
