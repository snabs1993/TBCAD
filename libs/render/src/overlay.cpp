#include "tbcad/render/overlay.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "tbcad/render/fill.h"

namespace tbcad::render {
namespace {

constexpr Rgba kWindowBorder{51, 128, 255, 255};
constexpr Rgba kWindowFill{51, 128, 255, 40};
constexpr Rgba kCrossingBorder{51, 204, 102, 255};
constexpr Rgba kCrossingFill{51, 204, 102, 40};

void emitTriangle(OverlayMesh& m, Vec2 a, Vec2 b, Vec2 c, Rgba col) {
    for (const Vec2& p : {a, b, c}) {
        m.vertices.push_back(
            {static_cast<float>(p.x), static_cast<float>(p.y), col.r, col.g, col.b, col.a});
    }
}

// Quad covering the segment a-b with thickness `w`; both ends extended by w/2 when `caps`.
void emitSegment(OverlayMesh& m, Vec2 a, Vec2 b, double w, bool caps, Rgba col) {
    Vec2 dir = b - a;
    double len = length(dir);
    if (len <= 1e-9) {
        dir = {1, 0};
    } else {
        dir = dir / len;
    }
    const double half = std::max(w, 1.0) * 0.5;
    if (caps) {
        a = a - dir * half;
        b = b + dir * half;
    }
    const Vec2 n = perp(dir) * half;
    emitTriangle(m, a - n, a + n, b + n, col);
    emitTriangle(m, a - n, b + n, b - n, col);
}

// Walks a screen-space polyline emitting solid or dashed segments. The dash phase is
// continuous across vertices.
void emitPolyline(OverlayMesh& m, const std::vector<Vec2>& pts, bool closed,
                  const OverlayStyle& st) {
    const std::size_t n = pts.size();
    if (n < 2)
        return;
    const std::size_t segs = (closed && n > 2) ? n : n - 1;
    const bool dashed = st.dashPx > 0 && st.gapPx > 0;
    const double period = st.dashPx + st.gapPx;
    double pos = 0;  // distance walked so far in pixels
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 a = pts[i];
        const Vec2 b = pts[(i + 1) % n];
        const double len = distance(a, b);
        if (!dashed) {
            emitSegment(m, a, b, st.widthPx, st.widthPx > 1.0, st.color);
            continue;
        }
        if (len <= 0)
            continue;
        const Vec2 dir = (b - a) / len;
        double t = 0;
        while (t < len) {
            const double phase = std::fmod(pos + t, period);
            if (phase < st.dashPx) {
                const double end = std::min(len, t + (st.dashPx - phase));
                emitSegment(m, a + dir * t, a + dir * end, st.widthPx, false, st.color);
                t = end;
            } else {
                t += period - phase;
            }
        }
        pos += len;
    }
}

void markerOutline(MarkerKind kind, double r, std::vector<std::vector<Vec2>>& strokes,
                   bool& closedOut) {
    closedOut = false;
    switch (kind) {
        case MarkerKind::Square:
            strokes.push_back({{-r, -r}, {r, -r}, {r, r}, {-r, r}});
            closedOut = true;
            break;
        case MarkerKind::Triangle: {
            const double h = r * 0.866;
            strokes.push_back({{-r, h}, {r, h}, {0, -r}});
            closedOut = true;
            break;
        }
        case MarkerKind::Circle: {
            std::vector<Vec2> ring;
            constexpr int kSegs = 24;
            for (int i = 0; i < kSegs; ++i) {
                const double a = kTwoPi * i / kSegs;
                ring.push_back({r * std::cos(a), r * std::sin(a)});
            }
            strokes.push_back(std::move(ring));
            closedOut = true;
            break;
        }
        case MarkerKind::X:
            strokes.push_back({{-r, -r}, {r, r}});
            strokes.push_back({{-r, r}, {r, -r}});
            break;
        case MarkerKind::Cross:
            strokes.push_back({{-r, 0}, {r, 0}});
            strokes.push_back({{0, -r}, {0, r}});
            break;
    }
}

}  // namespace

void Overlay::line(Space space, Vec2 a, Vec2 b, const OverlayStyle& style) {
    Shape s;
    s.type = Shape::Type::Polyline;
    s.space = space;
    s.pts = {a, b};
    s.style = style;
    shapes_.push_back(std::move(s));
}

void Overlay::polyline(Space space, std::span<const Vec2> pts, bool closed,
                       const OverlayStyle& style) {
    Shape s;
    s.type = Shape::Type::Polyline;
    s.space = space;
    s.pts.assign(pts.begin(), pts.end());
    s.closed = closed;
    s.style = style;
    shapes_.push_back(std::move(s));
}

void Overlay::fill(Space space, std::span<const Vec2> ring, Rgba color) {
    Shape s;
    s.type = Shape::Type::Fill;
    s.space = space;
    s.pts.assign(ring.begin(), ring.end());
    s.style.color = color;
    shapes_.push_back(std::move(s));
}

void Overlay::marker(Space space, Vec2 p, MarkerKind kind, double sizePx, Rgba color, bool filled,
                     double widthPx) {
    Shape s;
    s.type = Shape::Type::Marker;
    s.space = space;
    s.pts = {p};
    s.marker = kind;
    s.sizePx = sizePx;
    s.filled = filled;
    s.style.color = color;
    s.style.widthPx = widthPx;
    shapes_.push_back(std::move(s));
}

void Overlay::rubberBand(Space space, Vec2 a, Vec2 b, Rgba color) {
    line(space, a, b, OverlayStyle{color, 1.0, 6.0, 4.0});
}

void Overlay::selectionWindow(Vec2 a, Vec2 b, bool crossing) {
    selectionWindow(a, b, crossing, crossing ? kCrossingBorder : kWindowBorder,
                    crossing ? kCrossingFill : kWindowFill);
}

void Overlay::selectionWindow(Vec2 a, Vec2 b, bool crossing, Rgba border, Rgba fillColor) {
    const Vec2 ring[4] = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}};
    fill(Space::Screen, ring, fillColor);
    OverlayStyle st;
    st.color = border;
    if (crossing) {
        st.dashPx = 6;
        st.gapPx = 4;
    }
    polyline(Space::Screen, ring, true, st);
}

OverlayMesh Overlay::build(const Camera2D& camera) const {
    OverlayMesh mesh;
    auto toScreen = [&](Space sp, const Vec2& p) {
        return sp == Space::World ? camera.worldToScreen(p) : p;
    };
    for (const Shape& s : shapes_) {
        std::vector<Vec2> pts;
        pts.reserve(s.pts.size());
        for (const Vec2& p : s.pts) {
            Vec2 q = toScreen(s.space, p);
            if (isFinite(q) && std::abs(q.x) < 1e7 && std::abs(q.y) < 1e7)
                pts.push_back(q);
        }
        if (pts.empty())
            continue;
        switch (s.type) {
            case Shape::Type::Polyline:
                emitPolyline(mesh, pts, s.closed, s.style);
                break;
            case Shape::Type::Fill: {
                std::vector<Vec2> tris = triangulateEvenOdd({pts});
                for (std::size_t i = 0; i + 2 < tris.size(); i += 3)
                    emitTriangle(mesh, tris[i], tris[i + 1], tris[i + 2], s.style.color);
                break;
            }
            case Shape::Type::Marker: {
                const Vec2 c = pts[0];
                const double r = s.sizePx * 0.5;
                std::vector<std::vector<Vec2>> strokes;
                bool closed = false;
                markerOutline(s.marker, r, strokes, closed);
                for (auto& st : strokes)
                    for (Vec2& p : st)
                        p = p + c;
                if (s.filled && closed) {
                    for (const auto& ring : strokes) {
                        std::vector<Vec2> tris = triangulateEvenOdd({ring});
                        for (std::size_t i = 0; i + 2 < tris.size(); i += 3)
                            emitTriangle(mesh, tris[i], tris[i + 1], tris[i + 2], s.style.color);
                    }
                }
                OverlayStyle st;
                st.color = s.style.color;
                st.widthPx = s.style.widthPx;
                for (const auto& stroke : strokes)
                    emitPolyline(mesh, stroke, closed, st);
                break;
            }
        }
    }
    return mesh;
}

}  // namespace tbcad::render
