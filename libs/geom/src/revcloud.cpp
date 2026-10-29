#include "tbcad/geom/revcloud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace tbcad::geom {

namespace {

// Each cloud arc sweeps 120 degrees: bulge = tan(sweep / 4).
const double kCloudBulge = std::tan(kPi / 6);

constexpr int kFlagCorners = 1;
constexpr int kFlagCalligraphy = 2;

// Signed area of a closed ring (counter-clockwise positive).
double ringArea(const std::vector<Vec2>& ring) {
    double a = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        a += cross(ring[i] - ring[0], ring[(i + 1) % ring.size()] - ring[0]);
    return a / 2;
}

// `pts` without repeated points; with `ring` also without the last point on the first.
std::vector<Vec2> distinct(const std::vector<Vec2>& pts, bool ring) {
    std::vector<Vec2> out;
    for (const Vec2& p : pts)
        if (out.empty() || !tol::isZeroLength(p - out.back()))
            out.push_back(p);
    while (ring && out.size() > 1 && tol::isZeroLength(out.front() - out.back()))
        out.pop_back();
    return out;
}

// b lies on the straight run from a to c (and between them).
bool onRun(Vec2 a, Vec2 b, Vec2 c) {
    const Vec2 u = b - a, w = c - b;
    const double lu = length(u), lw = length(w);
    if (lu <= 0 || lw <= 0)
        return true;
    return std::abs(cross(u, w)) <= 1e-9 * lu * lw && dot(u, w) > 0;
}

double longestChord(const PolylineData& pl) {
    const std::size_t n = pl.v.size();
    const std::size_t segs = pl.closed ? n : (n ? n - 1 : 0);
    double best = 0;
    for (std::size_t i = 0; i < segs; ++i)
        best = std::max(best, distance(pl.v[i].p, pl.v[(i + 1) % n].p));
    return best;
}

const XdataApp* markOf(const Entity& e) {
    for (const XdataApp& a : e.common.ext.get().xdata)
        if (a.app == kCloudApp)
            return &a;
    return nullptr;
}

std::string shortNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

}  // namespace

PolylineData cloudClosed(const std::vector<Vec2>& ringIn, bool corners, double arcLength,
                         bool inward, bool calligraphy) {
    const std::vector<Vec2> ring = distinct(ringIn, true);
    std::vector<Vec2> pts;
    const std::size_t n = ring.size();
    if (corners) {
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = ring[i], b = ring[(i + 1) % n];
            const int pieces =
                std::max(1, static_cast<int>(std::ceil(distance(a, b) / arcLength - 1e-9)));
            for (int k = 0; k < pieces; ++k)
                pts.push_back(lerp(a, b, static_cast<double>(k) / pieces));
        }
    } else if (n > 0) {
        double total = 0;
        for (std::size_t i = 0; i < n; ++i)
            total += distance(ring[i], ring[(i + 1) % n]);
        const int pieces = std::max(3, static_cast<int>(std::ceil(total / arcLength - 1e-9)));
        const double step = total / pieces;
        std::size_t edge = 0;
        double edgeStart = 0;
        for (int k = 0; k < pieces; ++k) {
            const double s = k * step;
            for (;;) {
                const double len = distance(ring[edge], ring[(edge + 1) % n]);
                if (s <= edgeStart + len + 1e-12 || edge + 1 >= n)
                    break;
                edgeStart += len;
                ++edge;
            }
            const Vec2 a = ring[edge], b = ring[(edge + 1) % n];
            const double len = distance(a, b);
            pts.push_back(len > 0 ? lerp(a, b, std::clamp((s - edgeStart) / len, 0.0, 1.0)) : a);
        }
    }
    double sign = ringArea(ring) >= 0 ? 1.0 : -1.0;
    if (inward)
        sign = -sign;
    PolylineData d;
    d.closed = true;
    for (const Vec2& p : pts) {
        PolyVertex v;
        v.p = p;
        v.bulge = sign * kCloudBulge;
        if (calligraphy) {  // each arc swells and thins like a pen stroke
            v.startWidth = 0.12 * arcLength;
            v.endWidth = 0.03 * arcLength;
        }
        d.v.push_back(v);
    }
    return d;
}

PolylineData cloudOpen(const std::vector<Vec2>& pathIn, double arcLength, bool inward,
                       bool calligraphy) {
    const std::vector<Vec2> path = distinct(pathIn, false);
    PolylineData d = cloudClosed(path, true, arcLength, inward, calligraphy);
    // cloudClosed() closes the ring: drop the pieces of the closing edge, keep the last point.
    d.closed = false;
    if (path.empty())
        return d;
    const Vec2 last = path.back();
    while (d.v.size() > 1 && !tol::isZeroLength(d.v.back().p - last))
        d.v.pop_back();
    if (!d.v.empty()) {
        d.v.back().bulge = 0;
        d.v.back().startWidth = d.v.back().endWidth = 0;
    }
    return d;
}

double cloudArcCount(const std::vector<Vec2>& points, bool closed, bool corners, double arcLength) {
    const std::vector<Vec2> pts = distinct(points, closed);
    const std::size_t edges = closed ? pts.size() : (pts.empty() ? 0 : pts.size() - 1);
    double total = 0, count = 0;
    for (std::size_t i = 0; i < edges; ++i) {
        const double d = distance(pts[i], pts[(i + 1) % pts.size()]);
        total += d;
        count += std::max(1.0, std::ceil(d / arcLength - 1e-9));
    }
    return corners || !closed ? count : std::max(3.0, std::ceil(total / arcLength - 1e-9));
}

void markCloud(Entity& e, double arcLength, bool corners) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return;
    const bool calligraphy = !pl->v.empty() && pl->v.front().startWidth > 0;
    ExtData& x = e.common.ext.edit();
    std::erase_if(x.xdata, [](const XdataApp& a) { return a.app == kCloudApp; });
    XdataApp app{kCloudApp, {}};
    app.items.push_back(DataItem{1040, {}, 0, arcLength});
    app.items.push_back(DataItem{1040, {}, 0, longestChord(*pl)});
    app.items.push_back(
        DataItem{1070, {}, (corners ? kFlagCorners : 0) | (calligraphy ? kFlagCalligraphy : 0), 0});
    x.xdata.push_back(std::move(app));
}

std::optional<CloudShape> cloudShape(const Entity& e) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return std::nullopt;
    const std::size_t n = pl->v.size();
    if (n < (pl->closed ? 3u : 2u))
        return std::nullopt;
    const std::size_t segs = pl->closed ? n : n - 1;
    const XdataApp* mark = markOf(e);
    const double first = pl->v[0].bulge;
    for (std::size_t i = 0; i < segs; ++i) {
        const double b = pl->v[i].bulge;
        if (b == 0 || (b > 0) != (first > 0))
            return std::nullopt;
        // Without the mark only our own arcs count: a polyline of arcs is not always a cloud.
        if (!mark && std::abs(std::abs(b) - kCloudBulge) > 1e-6)
            return std::nullopt;
    }
    CloudShape s;
    s.closed = pl->closed;
    const double chord = longestChord(*pl);
    if (!(chord > 0))
        return std::nullopt;
    s.arcLength = chord;
    int flags = -1;
    if (mark) {
        double len = 0, ref = 0;
        int reals = 0;
        for (const DataItem& it : mark->items) {
            if (it.code == 1040 && reals == 0)
                len = it.real, ++reals;
            else if (it.code == 1040 && reals == 1)
                ref = it.real, ++reals;
            else if (it.code == 1070)
                flags = static_cast<int>(it.integer);
        }
        // The arc length follows the cloud when it is scaled: the longest chord tells how much.
        if (len > 0 && ref > 0 && std::isfinite(len * chord / ref))
            s.arcLength = len * chord / ref;
    }
    std::vector<Vec2> merged;
    for (std::size_t i = 0; i < n; ++i) {
        const bool end = !pl->closed && (i == 0 || i + 1 == n);
        if (i == 0 || end || !onRun(pl->v[i - 1].p, pl->v[i].p, pl->v[(i + 1) % n].p))
            merged.push_back(pl->v[i].p);
    }
    s.corners = flags >= 0 ? (flags & kFlagCorners) != 0 : merged.size() < n || !pl->closed;
    if (s.corners) {
        s.outline = distinct(merged, pl->closed);
    } else {
        for (const PolyVertex& v : pl->v)
            s.outline.push_back(v.p);
        s.outline = distinct(s.outline, true);
    }
    if (s.outline.size() < (pl->closed ? 3u : 2u))
        return std::nullopt;
    const bool ccwArcs = first > 0;
    s.inward = ccwArcs != (ringArea(s.outline) >= 0);
    s.calligraphy = flags >= 0 ? (flags & kFlagCalligraphy) != 0
                               : pl->v[0].startWidth > 0 && pl->v[0].startWidth > pl->v[0].endWidth;
    return s;
}

Result<Entity> reshapeCloud(const Entity& e, const CloudShape& shape) {
    const auto* old = entityAs<PolylineData>(e);
    if (!old)
        return makeError(ErrorCode::InvalidArgument, "a revision cloud is a polyline");
    if (!(shape.arcLength > 0) || !std::isfinite(shape.arcLength))
        return makeError(ErrorCode::InvalidArgument, "The arc length must be greater than zero.");
    const std::vector<Vec2> pts = distinct(shape.outline, shape.closed);
    if (shape.closed ? pts.size() < 3 || std::abs(ringArea(pts)) < tol::kLinear : pts.size() < 2)
        return makeError(ErrorCode::InvalidArgument, "The cloud would enclose no area.");
    const double count = cloudArcCount(pts, shape.closed, shape.corners, shape.arcLength);
    if (!std::isfinite(count) || count > static_cast<double>(kMaxCloudArcs)) {
        double total = 0;
        for (std::size_t i = 0; i + (shape.closed ? 0 : 1) < pts.size(); ++i)
            total += distance(pts[i], pts[(i + 1) % pts.size()]);
        return makeError(ErrorCode::InvalidArgument,
                         "The arc length is too small for a cloud this size; use " +
                             shortNumber(total / static_cast<double>(kMaxCloudArcs) * 1.01) +
                             " or more.");
    }
    PolylineData fresh =
        shape.closed
            ? cloudClosed(pts, shape.corners, shape.arcLength, shape.inward, shape.calligraphy)
            : cloudOpen(pts, shape.arcLength, shape.inward, shape.calligraphy);
    Entity out = e;
    auto& pl = *entityAs<PolylineData>(out);
    pl.v = std::move(fresh.v);
    pl.closed = fresh.closed;
    markCloud(out, shape.arcLength, shape.corners);
    return out;
}

std::vector<Vec2> cloudGrips(const CloudShape& shape) {
    std::vector<Vec2> g = shape.outline;
    const std::size_t m = shape.outline.size();
    const std::size_t edges = shape.closed ? m : m - 1;
    for (std::size_t i = 0; i < edges; ++i)
        g.push_back((shape.outline[i] + shape.outline[(i + 1) % m]) * 0.5);
    return g;
}

Result<Entity> moveCloudGrip(const Entity& e, const CloudShape& shape, int index, Vec2 to) {
    const int m = static_cast<int>(shape.outline.size());
    const int edges = shape.closed ? m : m - 1;
    if (index < 0 || index >= m + edges)
        return makeError(ErrorCode::InvalidArgument, "no such grip");
    CloudShape s = shape;
    // An open cloud keeps its arcs on the same side of the path, whatever the path encloses.
    const bool keepSide = !s.closed;
    if (index < m) {
        s.outline[static_cast<std::size_t>(index)] = to;
    } else {
        const auto a = static_cast<std::size_t>(index - m);
        const auto b = (a + 1) % static_cast<std::size_t>(m);
        const Vec2 delta = to - (s.outline[a] + s.outline[b]) * 0.5;
        s.outline[a] += delta;
        s.outline[b] += delta;
    }
    // The outline's points stay cloud vertices from now on (a cloud made from a circle has a
    // vertex at every arc: each edge keeps its one arc unless it grows longer).
    s.corners = true;
    auto r = reshapeCloud(e, s);
    if (!r || !keepSide)
        return r;
    const double before = entityAs<PolylineData>(e)->v.front().bulge;
    for (PolyVertex& v : entityAs<PolylineData>(r.value())->v)
        if ((v.bulge > 0) != (before > 0))
            v.bulge = -v.bulge;
    auto& pl = *entityAs<PolylineData>(r.value());
    if (!pl.v.empty())
        pl.v.back().bulge = 0;
    return r;
}

}  // namespace tbcad::geom
