#include "tbcad/drafting/settings.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>

namespace tbcad::drafting {

namespace {

constexpr std::array<std::string_view, kSnapModeCount> kNames = {
    "endpoint",      "intersection", "midpoint", "center", "quadrant",
    "perpendicular", "tangent",      "nearest",  "node"};
constexpr std::array<std::string_view, kSnapModeCount> kLabels = {
    "Endpoint",      "Intersection", "Midpoint", "Center", "Quadrant",
    "Perpendicular", "Tangent",      "Nearest",  "Node"};

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
    s.snap = boolOf(keys::snap, false);
    s.grid = boolOf(keys::grid, false);
    if (auto m = get(keys::osnapModes))
        if (auto mask = parseSnapModes(*m))
            s.modes = *mask;
    s.aperturePx = numOf(keys::osnapAperture, 10, 0);
    s.snapSpacing = numOf(keys::snapSpacing, 10, 0);
    s.gridSpacing = numOf(keys::gridSpacing, 10, 0);
    if (auto st = get(keys::gridStyle))
        s.gridLines = lower(*st) == "lines";
    if (auto p = get(keys::polarIncrement)) {
        auto inc = parsePolarIncrements(*p);
        if (!inc.empty())
            s.polarIncrementsDeg = std::move(inc);
    }
    // Ortho and polar tracking exclude each other; if both are stored, ortho wins.
    if (s.ortho)
        s.polar = false;
    return s;
}

}  // namespace tbcad::drafting
