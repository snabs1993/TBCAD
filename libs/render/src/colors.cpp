#include "tbcad/render/colors.h"

#include <cstdint>

namespace tbcad::render {

bool isLightBackground(Rgba bg) {
    // Rec. 601 luma on 0..255 values.
    return 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b > 127.5;
}

Rgba adaptToBackground(Rgba c, Rgba background) {
    const bool light = isLightBackground(background);
    if (light && c.r == 255 && c.g == 255 && c.b == 255)
        return {0, 0, 0, c.a};
    if (!light && c.r == 0 && c.g == 0 && c.b == 0)
        return {255, 255, 255, c.a};
    // On screen only (plots keep the real colours): pale colours on white paper and deep blue
    // on a dark background are hard to read (test.10), so they are moved toward readable.
    const double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    if (light && luma > 165) {
        const double k = 165 / luma;
        return {static_cast<std::uint8_t>(c.r * k), static_cast<std::uint8_t>(c.g * k),
                static_cast<std::uint8_t>(c.b * k), c.a};
    }
    if (!light && luma < 70) {
        const double t = (70 - luma) / (255 - luma);
        auto up = [t](std::uint8_t v) { return static_cast<std::uint8_t>(v + (255 - v) * t); };
        return {up(c.r), up(c.g), up(c.b), c.a};
    }
    return c;
}

}  // namespace tbcad::render
