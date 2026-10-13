#include "tbcad/core/color.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tbcad {

LineWeight LineWeight::mm(double millimetres) {
    double h = std::round(millimetres * 100.0);
    h = std::clamp(h, 0.0, 32767.0);
    return {static_cast<std::int16_t>(h)};
}

namespace {

Rgba fromHsv(double hueDeg, double s, double v) {
    double c = v * s;
    double hp = hueDeg / 60.0;
    double x = c * (1 - std::abs(std::fmod(hp, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1)
        r = c, g = x;
    else if (hp < 2)
        r = x, g = c;
    else if (hp < 3)
        g = c, b = x;
    else if (hp < 4)
        g = x, b = c;
    else if (hp < 5)
        r = x, b = c;
    else
        r = c, b = x;
    double m = v - c;
    auto q = [](double f) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(f, 0.0, 1.0) * 255));
    };
    return {q(r + m), q(g + m), q(b + m), 255};
}

// Shade k (0..9) within a hue: pure, then lighter tints and darker shades.
struct Shade {
    double s, v;
};
constexpr Shade kShades[10] = {{1.0, 1.0}, {1.0, 0.75}, {1.0, 0.5},  {0.5, 1.0},   {0.5, 0.75},
                               {0.5, 0.5}, {1.0, 0.38}, {0.25, 1.0}, {0.25, 0.75}, {0.25, 0.5}};

std::array<Rgba, 256> buildPalette() {
    std::array<Rgba, 256> p{};
    p[0] = {0, 0, 0, 255};
    p[1] = {255, 0, 0, 255};
    p[2] = {255, 255, 0, 255};
    p[3] = {0, 255, 0, 255};
    p[4] = {0, 255, 255, 255};
    p[5] = {0, 0, 255, 255};
    p[6] = {255, 0, 255, 255};
    p[7] = {255, 255, 255, 255};
    p[8] = {128, 128, 128, 255};
    p[9] = {192, 192, 192, 255};
    for (int i = 10; i <= 249; ++i) {
        int n = i - 10;
        const Shade& sh = kShades[n % 10];
        p[static_cast<std::size_t>(i)] = fromHsv((n / 10) * 15.0, sh.s, sh.v);
    }
    for (int i = 250; i <= 255; ++i) {
        auto g = static_cast<std::uint8_t>(i == 255 ? 255 : 45 * (i - 249));
        p[static_cast<std::size_t>(i)] = {g, g, g, 255};
    }
    return p;
}

}  // namespace

Rgba paletteColor(std::uint8_t index) {
    static const std::array<Rgba, 256> kPalette = buildPalette();
    return kPalette[index];
}

}  // namespace tbcad
