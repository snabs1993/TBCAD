#include "tbcad/render/colors.h"

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
    return c;
}

}  // namespace tbcad::render
