#include "tbcad/drafting/markers.h"

#include <cmath>
#include <vector>

namespace tbcad::drafting {

namespace {

using render::Overlay;
using render::Space;

void poly(Overlay& o, std::vector<Vec2> pts, bool closed, const render::OverlayStyle& st) {
    o.polyline(Space::Screen, pts, closed, st);
}

std::vector<Vec2> circlePts(Vec2 c, double r, int n = 20) {
    std::vector<Vec2> pts;
    for (int i = 0; i < n; ++i) {
        double a = kTwoPi * i / n;
        pts.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return pts;
}

}  // namespace

void drawSnapMarker(Overlay& o, const render::Camera2D& camera, Vec2 p, SnapMode mode,
                    bool darkTheme, double sizePx, std::optional<Rgba> color) {
    const Vec2 c = camera.worldToScreen(p);
    const double h = sizePx / 2;
    render::OverlayStyle st;
    st.color = color       ? *color
               : darkTheme ? Rgba{0xF2, 0xA9, 0x3B, 255}
                           : Rgba{0xC7, 0x77, 0x00, 255};
    st.widthPx = 2.0;
    switch (mode) {
        case SnapMode::Endpoint:
            poly(o,
                 {{c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}},
                 true, st);
            break;
        case SnapMode::Midpoint:
            poly(o, {{c.x, c.y - h}, {c.x + h, c.y + h * 0.8}, {c.x - h, c.y + h * 0.8}}, true, st);
            break;
        case SnapMode::Center:
            poly(o, circlePts(c, h), true, st);
            break;
        case SnapMode::Intersection:
            o.line(Space::Screen, {c.x - h, c.y - h}, {c.x + h, c.y + h}, st);
            o.line(Space::Screen, {c.x - h, c.y + h}, {c.x + h, c.y - h}, st);
            break;
        case SnapMode::Perpendicular: {
            // Right angle: the corner at the bottom left plus the small square inside it.
            poly(o, {{c.x - h, c.y - h}, {c.x - h, c.y + h}, {c.x + h, c.y + h}}, false, st);
            const double q = h * 0.8;
            poly(o, {{c.x - h, c.y + h - q}, {c.x - h + q, c.y + h - q}, {c.x - h + q, c.y + h}},
                 false, st);
            break;
        }
        case SnapMode::Tangent: {
            const Vec2 cc{c.x, c.y + h * 0.15};
            poly(o, circlePts(cc, h * 0.8), true, st);
            o.line(Space::Screen, {c.x - h, c.y - h * 0.65}, {c.x + h, c.y - h * 0.65}, st);
            break;
        }
        case SnapMode::Nearest:
            // Hourglass: two triangles meeting at the centre.
            poly(o,
                 {{c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x - h, c.y + h}, {c.x + h, c.y + h}},
                 true, st);
            break;
        case SnapMode::Quadrant:
            poly(o, {{c.x, c.y - h}, {c.x + h, c.y}, {c.x, c.y + h}, {c.x - h, c.y}}, true, st);
            break;
        case SnapMode::Node:
            poly(o, circlePts(c, h * 0.8), true, st);
            o.line(Space::Screen, {c.x - h, c.y - h}, {c.x + h, c.y + h}, st);
            o.line(Space::Screen, {c.x - h, c.y + h}, {c.x + h, c.y - h}, st);
            break;
        case SnapMode::Insertion:
            // Two small squares, one corner shared.
            poly(o,
                 {{c.x - h, c.y - h},
                  {c.x + h * 0.3, c.y - h},
                  {c.x + h * 0.3, c.y + h * 0.3},
                  {c.x - h, c.y + h * 0.3}},
                 true, st);
            poly(o,
                 {{c.x - h * 0.3, c.y - h * 0.3},
                  {c.x + h, c.y - h * 0.3},
                  {c.x + h, c.y + h},
                  {c.x - h * 0.3, c.y + h}},
                 true, st);
            break;
        case SnapMode::ApparentIntersection:
            // The intersection cross inside a square.
            poly(o,
                 {{c.x - h, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}},
                 true, st);
            o.line(Space::Screen, {c.x - h, c.y - h}, {c.x + h, c.y + h}, st);
            o.line(Space::Screen, {c.x - h, c.y + h}, {c.x + h, c.y - h}, st);
            break;
        case SnapMode::Extension:
            // Three dots in a row.
            for (double dx : {-h, 0.0, h})
                o.line(Space::Screen, {c.x + dx - h * 0.12, c.y}, {c.x + dx + h * 0.12, c.y}, st);
            break;
        case SnapMode::Parallel:
            // Two slanted strokes side by side.
            o.line(Space::Screen, {c.x - h, c.y - h * 0.4}, {c.x - h * 0.2, c.y + h}, st);
            o.line(Space::Screen, {c.x + h * 0.2, c.y - h}, {c.x + h, c.y + h * 0.4}, st);
            break;
    }
}

bool drawSnapMarker(Overlay& o, const render::Camera2D& camera, Vec2 p, std::string_view name,
                    bool darkTheme, double sizePx, std::optional<Rgba> color) {
    auto mode = parseSnapMode(name);
    if (!mode || snapModeName(*mode) != name)
        return false;
    drawSnapMarker(o, camera, p, *mode, darkTheme, sizePx, color);
    return true;
}

}  // namespace tbcad::drafting
