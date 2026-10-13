#include "tbcad/render/camera.h"

#include <algorithm>
#include <cmath>

namespace tbcad::render {

Vec2 Camera2D::screenToWorld(Vec2 px) const {
    return {center.x + (px.x - viewportW * 0.5) / pixelsPerMm,
            center.y - (px.y - viewportH * 0.5) / pixelsPerMm};
}

Vec2 Camera2D::worldToScreen(Vec2 w) const {
    return {viewportW * 0.5 + (w.x - center.x) * pixelsPerMm,
            viewportH * 0.5 - (w.y - center.y) * pixelsPerMm};
}

void Camera2D::zoomAt(Vec2 screenPt, double factor) {
    if (!(factor > 0) || !std::isfinite(factor))
        return;
    Vec2 anchor = screenToWorld(screenPt);
    pixelsPerMm = std::clamp(pixelsPerMm * factor, kMinPixelsPerMm, kMaxPixelsPerMm);
    center = {anchor.x - (screenPt.x - viewportW * 0.5) / pixelsPerMm,
              anchor.y + (screenPt.y - viewportH * 0.5) / pixelsPerMm};
}

void Camera2D::panPixels(double dx, double dy) {
    center.x -= dx / pixelsPerMm;
    center.y += dy / pixelsPerMm;
}

void Camera2D::fit(const BBox2& box, double margin) {
    if (box.empty())
        return;
    center = box.center();
    double w = box.width();
    double h = box.height();
    if (w <= 0 && h <= 0)
        return;
    double usable = std::max(0.0, 1.0 - 2.0 * margin);
    double sx = w > 0 ? viewportW * usable / w : kMaxPixelsPerMm;
    double sy = h > 0 ? viewportH * usable / h : kMaxPixelsPerMm;
    pixelsPerMm = std::clamp(std::min(sx, sy), kMinPixelsPerMm, kMaxPixelsPerMm);
}

BBox2 Camera2D::visibleWorld() const {
    Vec2 a = screenToWorld({0, 0});
    Vec2 b = screenToWorld({static_cast<double>(viewportW), static_cast<double>(viewportH)});
    BBox2 r;
    r.expand(a);
    r.expand(b);
    return r;
}

}  // namespace tbcad::render
