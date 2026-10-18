#include <algorithm>
#include <cmath>
#include <numbers>

#include "tbcad/pdf/pdf.h"

namespace tbcad::pdf {

void Page::op(std::string_view text) {
    content_ += text;
    content_ += '\n';
}

void Page::nums(std::initializer_list<double> values, std::string_view opName) {
    for (double v : values) {
        content_ += formatNumber(v);
        content_ += ' ';
    }
    content_ += opName;
    content_ += '\n';
}

void Page::beginLayer(int layer) {
    if (std::find(layersUsed_.begin(), layersUsed_.end(), layer) == layersUsed_.end())
        layersUsed_.push_back(layer);
    op("/OC /L" + std::to_string(layer) + " BDC");
}
void Page::endLayer() {
    op("EMC");
}

void Page::save() {
    op("q");
}
void Page::restore() {
    op("Q");
}

void Page::transform(double a, double b, double c, double d, double e, double f) {
    // Matrix entries need more than 4 decimals (e.g. cos/sin of a rotation).
    for (double v : {a, b, c, d, e, f}) {
        content_ += formatNumber(v, 8);
        content_ += ' ';
    }
    content_ += "cm\n";
}

void Page::drawImage(std::shared_ptr<const ImageData> image, double blx, double bly, double brx,
                     double bry, double tlx, double tly, double opacity) {
    if (!image || image->width <= 0 || image->height <= 0 ||
        image->rgba.size() <
            static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height) * 4)
        return;
    const std::size_t i = pics_.size();
    pics_.push_back({std::move(image), std::clamp(opacity, 0.0, 1.0)});
    save();
    content_ += "/Gs" + std::to_string(i) + " gs\n";
    transform(brx - blx, bry - bly, tlx - blx, tly - bly, blx, bly);
    content_ += "/Im" + std::to_string(i) + " Do\n";
    restore();
}

void Page::setStrokeRGB(double r, double g, double b) {
    auto c = [](double v) { return std::clamp(v, 0.0, 1.0); };
    nums({c(r), c(g), c(b)}, "RG");
}

void Page::setFillRGB(double r, double g, double b) {
    auto c = [](double v) { return std::clamp(v, 0.0, 1.0); };
    nums({c(r), c(g), c(b)}, "rg");
}

void Page::setLineWidthMm(double widthMm) {
    nums({std::max(widthMm, 0.0)}, "w");
}

void Page::setDashMm(const std::vector<double>& pattern, double phase) {
    content_ += '[';
    bool any = false;
    for (double v : pattern) {
        if (any)
            content_ += ' ';
        content_ += formatNumber(std::max(v, 0.0));
        any = true;
    }
    content_ += "] ";
    content_ += formatNumber(phase);
    content_ += " d\n";
}

void Page::setLineCap(LineCap cap) {
    nums({static_cast<double>(cap)}, "J");
}
void Page::setLineJoin(LineJoin join) {
    nums({static_cast<double>(join)}, "j");
}
void Page::setMiterLimit(double limit) {
    nums({std::max(limit, 1.0)}, "M");
}

void Page::moveTo(double x, double y) {
    nums({x, y}, "m");
    hasCurrentPoint_ = true;
}

void Page::lineTo(double x, double y) {
    nums({x, y}, "l");
    hasCurrentPoint_ = true;
}

void Page::curveTo(double x1, double y1, double x2, double y2, double x3, double y3) {
    nums({x1, y1, x2, y2, x3, y3}, "c");
    hasCurrentPoint_ = true;
}

void Page::closePath() {
    op("h");
}

void Page::rect(double x, double y, double w, double h) {
    nums({x, y, w, h}, "re");
    hasCurrentPoint_ = true;
}

void Page::arcTo(double cx, double cy, double radius, double startRad, double endRad, bool ccw) {
    double sweep = endRad - startRad;
    if (ccw) {
        while (sweep < 0)
            sweep += 2 * std::numbers::pi;
    } else {
        while (sweep > 0)
            sweep -= 2 * std::numbers::pi;
    }
    const double x0 = cx + radius * std::cos(startRad);
    const double y0 = cy + radius * std::sin(startRad);
    if (hasCurrentPoint_) {
        lineTo(x0, y0);
    } else {
        moveTo(x0, y0);
    }
    if (sweep == 0.0 || radius <= 0.0)
        return;

    const int segments =
        std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (std::numbers::pi / 2) - 1e-9)));
    const double step = sweep / segments;
    // Control-point distance for a circular arc of angle `step`.
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    double a = startRad;
    for (int i = 0; i < segments; ++i) {
        const double a1 = a + step;
        const double c0 = std::cos(a), s0 = std::sin(a);
        const double c1 = std::cos(a1), s1 = std::sin(a1);
        curveTo(cx + radius * (c0 - k * s0), cy + radius * (s0 + k * c0),
                cx + radius * (c1 + k * s1), cy + radius * (s1 - k * c1), cx + radius * c1,
                cy + radius * s1);
        a = a1;
    }
}

void Page::stroke() {
    op("S");
    hasCurrentPoint_ = false;
}

void Page::fill(FillRule rule) {
    op(rule == FillRule::EvenOdd ? "f*" : "f");
    hasCurrentPoint_ = false;
}

void Page::fillStroke(FillRule rule) {
    op(rule == FillRule::EvenOdd ? "B*" : "B");
    hasCurrentPoint_ = false;
}

void Page::clipRect(double x, double y, double w, double h) {
    rect(x, y, w, h);
    op("W n");
    hasCurrentPoint_ = false;
}

void Page::clipPolygon(const std::vector<std::pair<double, double>>& pts) {
    if (pts.size() < 3)
        return;
    moveTo(pts[0].first, pts[0].second);
    for (std::size_t i = 1; i < pts.size(); ++i)
        lineTo(pts[i].first, pts[i].second);
    closePath();
    op("W* n");
    hasCurrentPoint_ = false;
}

}  // namespace tbcad::pdf
