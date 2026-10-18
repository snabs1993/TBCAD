#include <algorithm>
#include <cmath>

#include "tbcad/print/print.h"

namespace tbcad::print {

namespace {

constexpr double kMmPerInch = 25.4;

double mmToPx(double mm, double dpi) {
    return mm * dpi / kMmPerInch;
}

std::vector<Vec2> arcPoints(const exportlib::PlotArc& a, const DeviceMetrics& m) {
    const double rPx = mmToPx(a.radius, m.dpiX);
    // A chord error of a quarter pixel.
    const double step =
        rPx > 0.5 ? std::clamp(2 * std::acos(std::max(0.0, 1.0 - 0.25 / rPx)), 0.01, 1.0) : 1.0;
    double sweep = a.full ? 2 * kPi : a.end - a.start;
    while (!a.full && sweep < 0)
        sweep += 2 * kPi;
    const int n = std::max(2, static_cast<int>(std::ceil(sweep / step)));
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const double t = a.start + sweep * i / n;
        pts.push_back(toDevice(
            {a.center.x + a.radius * std::cos(t), a.center.y + a.radius * std::sin(t)}, m));
    }
    return pts;
}

std::vector<double> dashPx(const exportlib::PlotStyle& s, const DeviceMetrics& m) {
    std::vector<double> d;
    for (double mm : s.dashMm)
        d.push_back(std::max(1.0, mmToPx(mm, m.dpiX)));
    return d;
}

double widthPx(const exportlib::PlotStyle& s, const DeviceMetrics& m) {
    return std::max(1.0, mmToPx(s.widthMm, m.dpiX));
}

}  // namespace

Vec2 toDevice(Vec2 p, const DeviceMetrics& m) {
    return {mmToPx(p.x, m.dpiX) - m.offsetXPx, m.pageHeightPx - mmToPx(p.y, m.dpiY) - m.offsetYPx};
}

void drawSheet(const exportlib::PlotSheet& sheet, const DeviceMetrics& m, Canvas& canvas) {
    using namespace exportlib;
    auto conv = [&](const std::vector<Vec2>& mm) {
        std::vector<Vec2> out;
        out.reserve(mm.size());
        for (const Vec2& p : mm)
            out.push_back(toDevice(p, m));
        return out;
    };
    int clips = 0;
    for (const PlotPrimitive& prim : sheet.prims) {
        if (const auto* p = std::get_if<PlotPath>(&prim)) {
            if (p->pts.size() >= 2)
                canvas.polyline(conv(p->pts), p->closed, p->style.color, widthPx(p->style, m),
                                dashPx(p->style, m));
        } else if (const auto* a = std::get_if<PlotArc>(&prim)) {
            canvas.polyline(arcPoints(*a, m), a->full, a->style.color, widthPx(a->style, m),
                            dashPx(a->style, m));
        } else if (const auto* f = std::get_if<PlotFill>(&prim)) {
            std::vector<std::vector<Vec2>> rings;
            for (const auto& r : f->rings)
                if (r.size() >= 3)
                    rings.push_back(conv(r));
            if (!rings.empty())
                canvas.fill(rings, f->color);
        } else if (const auto* d = std::get_if<PlotDots>(&prim)) {
            const double dia = widthPx(d->style, m);
            for (const Vec2& pt : d->pts)
                canvas.dot(toDevice(pt, m), d->style.color, dia);
        } else if (const auto* im = std::get_if<PlotImage>(&prim)) {
            canvas.image(*im->data,
                         {toDevice(im->corners[0], m), toDevice(im->corners[1], m),
                          toDevice(im->corners[2], m), toDevice(im->corners[3], m)},
                         im->opacity);
        } else if (const auto* c = std::get_if<PlotClip>(&prim)) {
            if (c->begin) {
                BBox2 r;
                r.expand(toDevice(c->rect.min, m));
                r.expand(toDevice(c->rect.max, m));
                canvas.pushClip(r);
                ++clips;
            } else if (clips > 0) {
                canvas.popClip();
                --clips;
            }
        }
    }
    while (clips-- > 0)
        canvas.popClip();
}

}  // namespace tbcad::print
