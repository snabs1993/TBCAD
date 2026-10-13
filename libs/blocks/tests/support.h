#pragma once

#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"

namespace tbcad::blocks::test {

inline Entity line(double x0, double y0, double x1, double y1) {
    Entity e;
    e.data = LineData{{x0, y0}, {x1, y1}};
    return e;
}

inline Entity circle(double x, double y, double r) {
    Entity e;
    e.data = CircleData{{x, y}, r};
    return e;
}

inline Entity insertOf(BlockId b, Vec2 pos, Vec2 scale = {1, 1}, double rot = 0) {
    Entity e;
    e.data = Indirect<InsertData>(InsertData{b, pos, scale, rot});
    return e;
}

inline bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return distance(a, b) <= eps;
}

// Adds a block and fills it; returns its id. `base` is the block base point.
inline BlockId addBlock(Document& doc, const char* name, const std::vector<Entity>& content,
                        Vec2 base = {}) {
    Transaction tx = doc.begin("block");
    BlockId id = tx.addBlock(BlockDef{.name = name, .base = base}).value();
    for (const Entity& e : content)
        REQUIRE(tx.add(e, id));
    tx.commit();
    return id;
}

inline EntityId addEntity(Document& doc, Entity e) {
    Transaction tx = doc.begin("add");
    EntityId id = tx.add(std::move(e)).value();
    tx.commit();
    return id;
}

}  // namespace tbcad::blocks::test
