#include "tbcad/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tbcad::ui {

namespace {

constexpr Rgba hex(std::uint32_t rgb) {
    return Rgba{static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                static_cast<std::uint8_t>((rgb >> 8) & 0xFF), static_cast<std::uint8_t>(rgb & 0xFF),
                255};
}

std::uint8_t lerp8(std::uint8_t a, std::uint8_t b, double t) {
    return static_cast<std::uint8_t>(std::lround(a + (b - a) * t));
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

}  // namespace

Theme darkTheme() {
    return Theme{.bgApp = hex(0x1B1D21),
                 .bgPanel = hex(0x23262B),
                 .bgViewport = hex(0x16181B),
                 .border = hex(0x343840),
                 .text = hex(0xE6E8EB),
                 .textMuted = hex(0x9097A1),
                 .accent = hex(0x3D8BFD),
                 .select = hex(0x4FA3FF),
                 .crossing = hex(0x3FB950),
                 .snap = hex(0xF2A93B),
                 .danger = hex(0xF85149),
                 .gridMinor = hex(0x24272C),
                 .gridMajor = hex(0x2E3238)};
}

Theme lightTheme() {
    return Theme{.bgApp = hex(0xF3F4F6),
                 .bgPanel = hex(0xFFFFFF),
                 .bgViewport = hex(0xFFFFFF),
                 .border = hex(0xD5D8DD),
                 .text = hex(0x1D2025),
                 .textMuted = hex(0x5E6570),
                 .accent = hex(0x1F6FEB),
                 .select = hex(0x1F6FEB),
                 .crossing = hex(0x1A7F37),
                 .snap = hex(0xC77700),
                 .danger = hex(0xCF222E),
                 .gridMinor = hex(0xECEEF1),
                 .gridMajor = hex(0xDADDE2)};
}

Theme themeFor(ThemeMode mode, bool systemDark) {
    switch (mode) {
        case ThemeMode::Light:
            return lightTheme();
        case ThemeMode::System:
            return systemDark ? darkTheme() : lightTheme();
        case ThemeMode::Dark:
            break;
    }
    return darkTheme();
}

ThemeMode parseThemeMode(std::string_view text) {
    if (text == "light")
        return ThemeMode::Light;
    if (text == "system")
        return ThemeMode::System;
    return ThemeMode::Dark;
}

std::string_view themeModeName(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return "light";
        case ThemeMode::System:
            return "system";
        case ThemeMode::Dark:
            break;
    }
    return "dark";
}

std::optional<Rgba> parseHexColor(std::string_view text) {
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8)
        return std::nullopt;
    int v[8] = {};
    for (std::size_t i = 0; i < text.size(); ++i) {
        v[i] = hexDigit(text[i]);
        if (v[i] < 0)
            return std::nullopt;
    }
    Rgba out{static_cast<std::uint8_t>(v[0] * 16 + v[1]),
             static_cast<std::uint8_t>(v[2] * 16 + v[3]),
             static_cast<std::uint8_t>(v[4] * 16 + v[5]), 255};
    if (text.size() == 8)
        out.a = static_cast<std::uint8_t>(v[6] * 16 + v[7]);
    return out;
}

std::string toHexColor(Rgba c) {
    char buf[16];
    if (c.a == 255)
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    else
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return buf;
}

Rgba withAlpha(Rgba color, double alpha) {
    color.a = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
    return color;
}

Rgba mix(Rgba a, Rgba b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return Rgba{lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t), lerp8(a.a, b.a, t)};
}

double luminance(Rgba c) {
    return (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
}

}  // namespace tbcad::ui
