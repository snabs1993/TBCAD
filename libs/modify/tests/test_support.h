#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "tbcad/modify/modify.h"

namespace tbcad::testing {

inline Entity make(EntityData d) {
    Entity e;
    e.common.layer = LayerId{7};
    e.common.color = Color::byLayer();
    e.data = std::move(d);
    return e;
}
inline Entity line(Vec2 a, Vec2 b) {
    return make(LineData{a, b});
}
inline Entity circle(Vec2 c, double r) {
    return make(CircleData{c, r});
}
inline Entity arc(Vec2 c, double r, double a0, double a1) {
    return make(ArcData{c, r, a0, a1});
}
inline Entity poly(std::vector<PolyVertex> v, bool closed) {
    PolylineData p;
    p.v = std::move(v);
    p.closed = closed;
    return make(p);
}
inline std::vector<PolyVertex> pts(std::vector<Vec2> p) {
    std::vector<PolyVertex> v;
    for (Vec2 q : p)
        v.push_back({q, 0, 0, 0});
    return v;
}

inline bool near(Vec2 a, Vec2 b, double tol = 1e-6) {
    return distance(a, b) <= tol;
}
inline bool near(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) <= tol;
}

template <class T>
const T& as(const Entity& e) {
    const T* p = entityAs<T>(e);
    REQUIRE(p != nullptr);
    return *p;
}

// Vertices of a polyline entity, as points.
inline std::vector<Vec2> verts(const Entity& e) {
    std::vector<Vec2> out;
    for (const auto& v : as<PolylineData>(e).v)
        out.push_back(v.p);
    return out;
}

// True when every point of `want` has a vertex within tol and the counts match.
inline bool sameVertexSet(const std::vector<Vec2>& got, const std::vector<Vec2>& want,
                          double tol = 1e-6) {
    if (got.size() != want.size())
        return false;
    for (Vec2 w : want) {
        if (std::none_of(got.begin(), got.end(), [&](Vec2 g) { return near(g, w, tol); }))
            return false;
    }
    return true;
}

inline double totalLength(const Entity& e) {
    return geom::length(e);
}

}  // namespace tbcad::testing
