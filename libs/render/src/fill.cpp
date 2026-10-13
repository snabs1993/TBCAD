#include "tbcad/render/fill.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <mapbox/earcut.hpp>

namespace tbcad::render {
namespace {

using Ring = std::vector<Vec2>;

double signedArea(const Ring& r) {
    double a = 0;
    for (std::size_t i = 0, n = r.size(); i < n; ++i)
        a += cross(r[i], r[(i + 1) % n]);
    return a * 0.5;
}

bool pointInRing(const Vec2& p, const Ring& r) {
    bool inside = false;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
        const Vec2& a = r[i];
        const Vec2& b = r[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

bool usable(const Ring& r) {
    if (r.size() < 3)
        return false;
    for (const Vec2& p : r)
        if (!isFinite(p))
            return false;
    return true;
}

}  // namespace

std::vector<Vec2> triangulateEvenOdd(const std::vector<Ring>& input) {
    std::vector<const Ring*> rings;
    for (const Ring& r : input)
        if (usable(r))
            rings.push_back(&r);
    const std::size_t n = rings.size();
    if (n == 0)
        return {};

    std::vector<double> area(n);
    for (std::size_t i = 0; i < n; ++i)
        area[i] = std::abs(signedArea(*rings[i]));

    // Nesting depth: how many other rings contain this ring's first vertex.
    std::vector<int> depth(n, 0);
    std::vector<std::vector<std::size_t>> containers(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (i != j && area[j] > area[i] && pointInRing((*rings[i])[0], *rings[j])) {
                ++depth[i];
                containers[i].push_back(j);
            }
        }
    }

    // Holes (odd depth) belong to the smallest container of depth-1.
    std::vector<std::vector<std::size_t>> holes(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0)
            continue;
        std::size_t best = n;
        for (std::size_t j : containers[i])
            if (depth[j] == depth[i] - 1 && (best == n || area[j] < area[best]))
                best = j;
        if (best != n)
            holes[best].push_back(i);
    }

    std::vector<Vec2> out;
    for (std::size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 != 0)
            continue;
        // Work relative to the outline's first vertex so earcut sees small numbers.
        const Vec2 shift = (*rings[i])[0];
        std::vector<std::vector<std::array<double, 2>>> poly;
        std::vector<Vec2> flat;
        auto addRing = [&](const Ring& r) {
            std::vector<std::array<double, 2>> pts;
            pts.reserve(r.size());
            for (const Vec2& p : r) {
                pts.push_back({p.x - shift.x, p.y - shift.y});
                flat.push_back(p);
            }
            poly.push_back(std::move(pts));
        };
        addRing(*rings[i]);
        for (std::size_t h : holes[i])
            addRing(*rings[h]);

        // earcut indexes the concatenation of all rings, in the order they were added.
        std::vector<std::uint32_t> idx = mapbox::earcut<std::uint32_t>(poly);
        out.reserve(out.size() + idx.size());
        for (std::uint32_t k : idx)
            out.push_back(flat[k]);
    }
    return out;
}

double triangleArea(const std::vector<Vec2>& t) {
    double a = 0;
    for (std::size_t i = 0; i + 2 < t.size(); i += 3)
        a += std::abs(cross(t[i + 1] - t[i], t[i + 2] - t[i])) * 0.5;
    return a;
}

}  // namespace tbcad::render
