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

void clipPolyline(const DisplayPolyline& in, const std::vector<Vec2>& poly, bool invert,
                  DisplayList& out) {
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
            if (insidePolygon(poly, mid) != invert) {
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
        s += cross(p[i] - p[0], p[(i + 1) % p.size()] - p[0]);
    return s / 2;
}

bool inTriangle(Vec2 a, Vec2 b, Vec2 c, Vec2 p) {
    return cross(b - a, p - a) >= 0 && cross(c - b, p - b) >= 0 && cross(a - c, p - c) >= 0;
}

bool isConvex(const std::vector<Vec2>& ccw) {
    const std::size_t n = ccw.size();
    for (std::size_t i = 0; i < n; ++i)
        if (cross(ccw[(i + 1) % n] - ccw[i], ccw[(i + 2) % n] - ccw[(i + 1) % n]) < -1e-12)
            return false;
    return true;
}

// Triangles of a simple counter-clockwise polygon (ear clipping). Degenerate corners drop out.
void triangulate(std::vector<Vec2> p, std::vector<std::vector<Vec2>>& out) {
    std::size_t guard = p.size() * p.size() + 8;
    while (p.size() > 3 && guard-- > 0) {
        const std::size_t n = p.size();
        bool cut = false;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
            const double turn = cross(b - a, c - b);
            if (std::abs(turn) <= 1e-18 * (1 + std::abs(a.x) + std::abs(a.y))) {
                p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));  // straight or doubled
                cut = true;
                break;
            }
            if (turn < 0)
                continue;
            bool ear = true;
            for (std::size_t k = 0; ear && k < n; ++k) {
                const Vec2 q = p[k];
                if (k == i || k == (i + 1) % n || k == (i + n - 1) % n)
                    continue;
                if ((q.x == a.x && q.y == a.y) || (q.x == b.x && q.y == b.y) ||
                    (q.x == c.x && q.y == c.y))
                    continue;
                ear = !inTriangle(a, b, c, q);
            }
            if (!ear)
                continue;
            out.push_back({a, b, c});
            p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
            cut = true;
            break;
        }
        if (!cut)
            break;  // not simple (crosses itself): keep what was found
    }
    if (p.size() == 3 && std::abs(cross(p[1] - p[0], p[2] - p[1])) > 0)
        out.push_back(std::move(p));
}

// Convex hull (counter-clockwise) as indices into `p`.
std::vector<std::size_t> hullIndices(const std::vector<Vec2>& p) {
    std::vector<std::size_t> idx(p.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return p[a].x < p[b].x || (p[a].x == p[b].x && p[a].y < p[b].y);
    });
    std::vector<std::size_t> h(2 * idx.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < idx.size(); ++i) {
        while (k >= 2 && cross(p[h[k - 1]] - p[h[k - 2]], p[idx[i]] - p[h[k - 2]]) <= 0)
            --k;
        h[k++] = idx[i];
    }
    for (std::size_t i = idx.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross(p[h[k - 1]] - p[h[k - 2]], p[idx[i]] - p[h[k - 2]]) <= 0)
            --k;
        h[k++] = idx[i];
    }
    h.resize(k > 1 ? k - 1 : k);
    return h;
}

// `ring` cut to one convex counter-clockwise piece.
std::vector<Vec2> clipToConvex(std::vector<Vec2> r, const std::vector<Vec2>& piece) {
    for (std::size_t i = 0; i < piece.size() && r.size() >= 3; ++i)
        r = clipHalfPlane(r, piece[i], piece[(i + 1) % piece.size()]);
    return r;
}

// Whether a ring edge crosses or touches the boundary anywhere.
bool ringMeetsBoundary(const std::vector<Vec2>& ring, const std::vector<Vec2>& poly) {
    std::vector<double> ts;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        crossings(poly, ring[i], ring[(i + 1) % ring.size()], ts);
        if (!ts.empty())
            return true;
    }
    return false;
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

DisplayList clipDisplay(const DisplayList& in, const std::vector<Vec2>& polygon, bool invert) {
    DisplayList out;
    if (polygon.size() < 3)
        return in;
    std::vector<Vec2> ccw = polygon;
    if (signedArea(ccw) < 0)
        std::reverse(ccw.begin(), ccw.end());
    // Fills are cut against convex pieces. Inside: the boundary itself when convex, else its
    // triangles. Outside (invert): what lies beyond the convex hull, plus the triangles of the
    // pockets between the hull and the boundary.
    std::vector<std::vector<Vec2>> pieces;
    std::vector<Vec2> outer;
    if (!invert) {
        if (isConvex(ccw))
            pieces.push_back(ccw);
        else
            triangulate(ccw, pieces);
    } else if (isConvex(ccw)) {
        outer = ccw;
    } else {
        const std::vector<std::size_t> hull = hullIndices(ccw);
        for (std::size_t i : hull)
            outer.push_back(ccw[i]);
        const std::size_t n = ccw.size();
        for (std::size_t h = 0; h < hull.size(); ++h) {
            const std::size_t from = hull[h], to = hull[(h + 1) % hull.size()];
            const std::size_t steps = (to + n - from) % n;
            if (steps < 2)
                continue;
            // The pocket runs back along the boundary from `to` to `from`, counter-clockwise.
            std::vector<Vec2> pocket;
            for (std::size_t k = 0; k <= steps; ++k)
                pocket.push_back(ccw[(to + n - k) % n]);
            if (std::abs(signedArea(pocket)) > 1e-12) {
                if (signedArea(pocket) < 0)
                    std::reverse(pocket.begin(), pocket.end());
                triangulate(pocket, pieces);
            }
        }
    }
    for (const DisplayItem& item : in.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            clipPolyline(*pl, polygon, invert, out);
        } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
            // Left whole when nothing of it reaches the boundary (real text keeps its glyph).
            bool whole = !fill->rings.empty();
            bool keep = true;
            for (const auto& ring : fill->rings) {
                if (ring.empty())
                    continue;
                const bool kept = insidePolygon(polygon, ring[0]) != invert;
                if (&ring == &fill->rings.front())
                    keep = kept;
                if (kept != keep || ringMeetsBoundary(ring, polygon)) {
                    whole = false;
                    break;
                }
            }
            if (whole) {
                // ... and the boundary does not lie within it.
                for (const auto& ring : fill->rings)
                    if (ring.size() >= 3 && insidePolygon(ring, polygon[0]))
                        whole = false;
            }
            if (whole) {
                if (keep)
                    out.items.push_back(item);
                continue;
            }
            DisplayFill f;
            f.style = fill->style;
            f.glyph = fill->glyph;
            for (const auto& ring : fill->rings) {
                if (invert) {
                    // Outside the hull = disjoint pieces: beyond edge i, inside edges 0..i-1.
                    std::vector<Vec2> inside = ring;
                    for (std::size_t i = 0; i < outer.size() && inside.size() >= 3; ++i) {
                        const Vec2 a = outer[i], b = outer[(i + 1) % outer.size()];
                        std::vector<Vec2> beyond = clipHalfPlane(inside, b, a);  // reversed edge
                        if (beyond.size() >= 3 && std::abs(signedArea(beyond)) > 1e-12)
                            f.rings.push_back(std::move(beyond));
                        inside = clipHalfPlane(inside, a, b);
                    }
                }
                for (const auto& piece : pieces) {
                    std::vector<Vec2> r = clipToConvex(ring, piece);
                    if (r.size() >= 3 && std::abs(signedArea(r)) > 1e-12)
                        f.rings.push_back(std::move(r));
                }
            }
            if (!f.rings.empty())
                out.items.emplace_back(std::move(f));
        } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
            DisplayPoints k;
            k.style = pts->style;
            for (const Vec2& p : pts->pts)
                if (insidePolygon(polygon, p) != invert)
                    k.pts.push_back(p);
            if (!k.pts.empty())
                out.items.emplace_back(std::move(k));
        }
    }
    return out;
}

}  // namespace tbcad::blocks
