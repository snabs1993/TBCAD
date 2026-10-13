// ACI colour index -> TBCAD palette index.
//
// The ACI layout is a public convention: 1..7 are the basic colours, 8..9 grays, 10..249 are 24
// hues in 15-degree steps with 10 shades each (full and half saturation at values 1.0, 0.8, 0.6,
// 0.5, 0.3), 250..255 grays. We rebuild approximate RGB values from that rule and pick the nearest
// entry of the TBCAD palette (squared RGB distance, lowest index on ties). 1..7 keep their
// meaning exactly.
#include <array>
#include <cmath>

#include "importer.h"

namespace tbcad::dxf::detail {

namespace {

struct Rgb3 {
    double r, g, b;
};

Rgb3 hsv(double hueDeg, double s, double v) {
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
    return {(r + m) * 255, (g + m) * 255, (b + m) * 255};
}

Rgb3 aciRgb(int i) {
    static constexpr double kValue[10] = {1.0, 1.0, 0.8, 0.8, 0.6, 0.6, 0.5, 0.5, 0.3, 0.3};
    static constexpr double kGray[6] = {0x33, 0x50, 0x69, 0x82, 0xBE, 0xFF};
    if (i == 8)
        return {128, 128, 128};
    if (i == 9)
        return {192, 192, 192};
    if (i >= 250)
        return {kGray[i - 250], kGray[i - 250], kGray[i - 250]};
    int n = i - 10;
    return hsv((n / 10) * 15.0, (n % 2 == 0) ? 1.0 : 0.5, kValue[n % 10]);
}

std::array<std::uint8_t, 256> buildTable() {
    std::array<std::uint8_t, 256> t{};
    for (int i = 1; i <= 7; ++i)
        t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    for (int i = 8; i <= 255; ++i) {
        Rgb3 want = aciRgb(i);
        double best = 1e300;
        int bestIdx = 7;
        for (int j = 1; j <= 255; ++j) {
            Rgba p = paletteColor(static_cast<std::uint8_t>(j));
            double dr = p.r - want.r, dg = p.g - want.g, db = p.b - want.b;
            double d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                bestIdx = j;
            }
        }
        t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bestIdx);
    }
    return t;
}

}  // namespace

Color mapAci(int aci) {
    static const std::array<std::uint8_t, 256> kTable = buildTable();
    if (aci < 1 || aci > 255)
        return Color::index(7);
    return Color::index(kTable[static_cast<std::size_t>(aci)]);
}

}  // namespace tbcad::dxf::detail
