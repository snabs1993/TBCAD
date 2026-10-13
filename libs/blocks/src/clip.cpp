#include "tbcad/blocks/clip.h"

#include <algorithm>
#include <cmath>

namespace tbcad::blocks {

namespace {

bool onSegment(Vec2 a, Vec2 b, Vec2 p) {
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len == 0)
        return std::hypot(p.x - a.x, p.y - a.y) < 1e-12;
    const double d = std::abs(cross(b - a, p - a)) / len;
    if (d > 1e-9)
        return false;
    const double t = ((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / (len * len);
    return t >= -1e-12 && t <= 1 + 1e-12;
}

// Parameters along a->b where it crosses an edge of the polygon (0 < t < 1).
void crossings(const std::vector<Vec2>& poly, Vec2 a, Vec2 b, std::vector<double>& ts) {
    const Vec2 d = b - a;
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 c = poly[i];
        const Vec2 e = poly[(i + 1) % n] - c;
        const double den = cross(d, e);
        if (std::abs(den) < 1e-18)
            continue;
        const double t = cross(c - a, e) / den;
        const double u = cross(c - a, d) / den;
        if (t > 0 && t < 1 && u >= 0 && u <= 1)
            ts.push_back(t);
    }
}

void clipPolyline(const DisplayPolyline& in, const std::vector<Vec2>& poly, DisplayList& out) {
    const std::size_t count = in.pts.size();
    if (count < 2)
        return;
    const std::size_t segs = in.closed ? count : count - 1;
    DisplayPolyline run;
    run.style = in.style;
    auto flush = [&] {
        if (run.pts.size() >= 2)
            out.items.emplace_back(run);
        run.pts.clear();
    };
    std::vector<double> ts;
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 a = in.pts[i];
        const Vec2 b = in.pts[(i + 1) % count];
        ts.assign({0.0});
        crossings(poly, a, b, ts);
        ts.push_back(1.0);
        std::sort(ts.begin(), ts.end());
        for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
            if (ts[k + 1] - ts[k] < 1e-12)
                continue;
            const Vec2 p = a + (b - a) * ts[k];
            const Vec2 q = a + (b - a) * ts[k + 1];
            const Vec2 mid = a + (b - a) * ((ts[k] + ts[k + 1]) / 2);
            if (insidePolygon(poly, mid)) {
                if (run.pts.empty() ||
                    std::hypot(run.pts.back().x - p.x, run.pts.back().y - p.y) > 1e-9) {
                    flush();
                    run.pts.push_back(p);
                }
                run.pts.push_back(q);
            } else {
                flush();
            }
        }
    }
    flush();
}

// Sutherland-Hodgman against one half-plane of the (counter-clockwise) clip polygon.
std::vector<Vec2> clipHalfPlane(const std::vector<Vec2>& ring, Vec2 a, Vec2 b) {
    std::vector<Vec2> out;
    auto side = [&](Vec2 p) { return cross(b - a, p - a); };
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec2 p = ring[i];
        const Vec2 q = ring[(i + 1) % ring.size()];
        const double sp = side(p), sq = side(q);
        if (sp >= 0)
            out.push_back(p);
        if ((sp >= 0) != (sq >= 0)) {
            const double t = sp / (sp - sq);
            out.push_back(p + (q - p) * t);
        }
    }
    return out;
}

double signedArea(const std::vector<Vec2>& p) {
    double s = 0;
    for (std::size_t i = 0; i < p.size(); ++i)
        s += cross(p[i], p[(i + 1) % p.size()]);
    return s / 2;
}

}  // namespace

bool insidePolygon(const std::vector<Vec2>& polygon, Vec2 p) {
    bool in = false;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 a = polygon[i], b = polygon[j];
        if (onSegment(a, b, p))
            return true;
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

DisplayList clipDisplay(const DisplayList& in, const std::vector<Vec2>& polygon) {
    DisplayList out;
    if (polygon.size() < 3)
        return in;
    std::vector<Vec2> ccw = polygon;
    if (signedArea(ccw) < 0)
        std::reverse(ccw.begin(), ccw.end());
    for (const DisplayItem& item : in.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            clipPolyline(*pl, polygon, out);
        } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
            DisplayFill f;
            f.style = fill->style;
            for (const auto& ring : fill->rings) {
                std::vector<Vec2> r = ring;
                for (std::size_t i = 0; i < ccw.size() && r.size() >= 3; ++i)
                    r = clipHalfPlane(r, ccw[i], ccw[(i + 1) % ccw.size()]);
                if (r.size() >= 3)
                    f.rings.push_back(std::move(r));
            }
            if (!f.rings.empty())
                out.items.emplace_back(std::move(f));
        } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
            DisplayPoints k;
            k.style = pts->style;
            for (const Vec2& p : pts->pts)
                if (insidePolygon(polygon, p))
                    k.pts.push_back(p);
            if (!k.pts.empty())
                out.items.emplace_back(std::move(k));
        }
    }
    return out;
}

}  // namespace tbcad::blocks
