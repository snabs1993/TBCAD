#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/draw/draw.h"
#include "tbcad/geom/geom.h"

namespace tbcad::draw::test {

// A document plus a ScriptHost with the built-in and the draw commands registered.
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;

    Fixture() : host(doc, reg) {
        cmd::registerBuiltinCommands(reg);
        draw::registerCommands(reg);
    }

    // Entities of the document in creation order.
    std::vector<const Entity*> entities() const {
        std::vector<const Entity*> v;
        doc.forEachEntity([&](const Entity& e) { v.push_back(&e); });
        std::sort(v.begin(), v.end(), [](const Entity* a, const Entity* b) {
            return a->common.id.value < b->common.id.value;
        });
        return v;
    }

    EntityId addEntity(EntityData d) {
        Transaction tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(d)});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
};

inline bool close(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}
inline bool close(Vec2 a, Vec2 b, double eps = 1e-9) {
    return close(a.x, b.x, eps) && close(a.y, b.y, eps);
}
// Angles modulo 2*pi.
inline bool closeAngle(double a, double b, double eps = 1e-9) {
    double d = std::abs(normalizeAngle(a) - normalizeAngle(b));
    return d <= eps || kTwoPi - d <= eps;
}

template <class T>
const T& as(const Entity* e) {
    REQUIRE(e != nullptr);
    const T* p = std::get_if<T>(&e->data);
    REQUIRE(p != nullptr);
    return *p;
}
template <class T>
const T& indirect(const Entity* e) {
    REQUIRE(e != nullptr);
    const auto* p = std::get_if<Indirect<T>>(&e->data);
    REQUIRE(p != nullptr);
    return **p;
}

inline void checkVertices(const PolylineData& pl, const std::vector<Vec2>& expect) {
    REQUIRE(pl.v.size() == expect.size());
    for (std::size_t i = 0; i < expect.size(); ++i) {
        CHECK_MESSAGE(close(pl.v[i].p, expect[i]), "vertex ", i, " is (", pl.v[i].p.x, ", ",
                      pl.v[i].p.y, ")");
    }
}

}  // namespace tbcad::draw::test
