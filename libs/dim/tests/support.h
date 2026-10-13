#pragma once

#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/dim/dim.h"

namespace tbcad::dim::test {

inline DimensionData linear(Vec2 a, Vec2 b, Vec2 where, double rotation = 0) {
    DimensionData d;
    d.kind = DimKind::Linear;
    d.rotation = rotation;
    d.defPts = {a, b, where, {}};
    return d;
}

inline DimensionData aligned(Vec2 a, Vec2 b, Vec2 where) {
    DimensionData d = linear(a, b, where);
    d.kind = DimKind::Aligned;
    return d;
}

inline DimensionData radial(DimKind k, Vec2 a, Vec2 b) {
    DimensionData d;
    d.kind = k;
    d.defPts = {a, b, {}, {}};
    return d;
}

inline DimensionData angular2(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1, Vec2 arcPoint) {
    DimensionData d;
    d.kind = DimKind::Angular2Line;
    d.defPts = {a0, a1, b0, b1};
    d.textPos = arcPoint;
    return d;
}

inline Entity entityOf(EntityData d) {
    Entity e;
    e.data = std::move(d);
    return e;
}

inline EntityId add(Document& doc, Entity e) {
    Transaction tx = doc.begin("add");
    EntityId id = tx.add(std::move(e)).value();
    tx.commit();
    return id;
}

inline bool near(Vec2 a, Vec2 b, double eps = 1e-6) {
    return distance(a, b) <= eps;
}

inline std::size_t count(const Layout& L, Role role) {
    std::size_t n = 0;
    for (const auto& l : L.lines)
        n += l.role == role ? 1 : 0;
    return n;
}

inline BBox2 box(const std::vector<Vec2>& pts) {
    BBox2 b;
    for (Vec2 p : pts)
        b.expand(p);
    return b;
}

inline BBox2 arrowBox(const Layout::Arrow& a) {
    return box(a.ring);
}

}  // namespace tbcad::dim::test
