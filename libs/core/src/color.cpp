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

// AutoCAD's index colours 10..249: 24 hues 15 degrees apart (ten indices each); within a hue
// ten shades, pure and half-saturated at brightness 100, 80, 60, 50 and 30 %. Components are cut
// off (not rounded) like AutoCAD's table, e.g. 11 = FF7F7F, 21 = FF9F7F, 17 = 7F3F3F.
struct Shade {
    double s, v;  // saturation, brightness of 255
};
constexpr Shade kShades[10] = {{1.0, 255.0}, {0.5, 255.0}, {1.0, 204.0}, {0.5, 204.0}, {1.0, 153.0},
                               {0.5, 153.0}, {1.0, 127.5}, {0.5, 127.5}, {1.0, 76.5},  {0.5, 76.5}};

Rgba fromHueShade(int hueStep, const Shade& sh) {
    const int hue = hueStep * 15;
    const int sector = hue / 60;
    const double f = (hue % 60) / 60.0;
    const double hi = sh.v, lo = sh.v * (1.0 - sh.s);
    const double up = lo + (hi - lo) * f, down = lo + (hi - lo) * (1.0 - f);
    double r = 0, g = 0, b = 0;
    switch (sector) {
        case 0:
            r = hi, g = up, b = lo;
            break;
        case 1:
            r = down, g = hi, b = lo;
            break;
        case 2:
            r = lo, g = hi, b = up;
            break;
        case 3:
            r = lo, g = down, b = hi;
            break;
        case 4:
            r = up, g = lo, b = hi;
            break;
        default:
            r = hi, g = lo, b = down;
            break;
    }
    auto q = [](double c) { return static_cast<std::uint8_t>(std::floor(c + 1e-9)); };
    return {q(r), q(g), q(b), 255};
}

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
        const int n = i - 10;
        p[static_cast<std::size_t>(i)] = fromHueShade(n / 10, kShades[n % 10]);
    }
    // The six greys of AutoCAD's table.
    constexpr std::uint8_t kGreys[6] = {51, 80, 105, 130, 190, 255};
    for (int i = 250; i <= 255; ++i)
        p[static_cast<std::size_t>(i)] = {kGreys[i - 250], kGreys[i - 250], kGreys[i - 250], 255};
    return p;
}

}  // namespace

Rgba paletteColor(std::uint8_t index) {
    static const std::array<Rgba, 256> kPalette = buildPalette();
    return kPalette[index];
}

}  // namespace tbcad
