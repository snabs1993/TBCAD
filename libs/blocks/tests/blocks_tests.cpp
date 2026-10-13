#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

TEST_CASE("instance transform: base to position, then rotation and scale") {
    InsertData ins{BlockId{1}, {10, 20}, {2, 3}, kPi / 2};
    Transform2 t = instanceTransform(ins, {1, 1});
    CHECK(near(t.apply({1, 1}), {10, 20}));  // base lands on the insertion point
    CHECK(near(t.apply({2, 1}), {10, 22}));  // +x scaled by 2 then turned 90 degrees
    CHECK(near(t.apply({1, 2}), {7, 20}));   // +y scaled by 3 then turned 90 degrees
}

TEST_CASE("block names are validated") {
    Document doc;
    addBlock(doc, "Door", {line(0, 0, 1, 0)});
    CHECK(validateBlockName(doc, "Window").ok());
    CHECK_FALSE(validateBlockName(doc, "").ok());
    CHECK_FALSE(validateBlockName(doc, "   ").ok());
    CHECK_FALSE(validateBlockName(doc, "door").ok());  // case-insensitive duplicate
    CHECK_FALSE(validateBlockName(doc, "a/b").ok());
    CHECK_FALSE(validateBlockName(doc, "*U1").ok());
    CHECK_FALSE(validateBlockName(doc, std::string(256, 'x')).ok());
    CHECK(validateBlockName(doc, "Door", doc.blockByName("Door")->id).ok());  // keeps its own name
}

TEST_CASE("cycle detection, direct and nested") {
    Document doc;
    BlockId a = addBlock(doc, "A", {line(0, 0, 1, 0)});
    BlockId b = addBlock(doc, "B", {insertOf(a, {0, 0})});
    BlockId c = addBlock(doc, "C", {insertOf(b, {0, 0})});
    CHECK(references(doc, c, a));
    CHECK_FALSE(references(doc, a, c));
    CHECK(wouldCreateCycle(doc, a, a));  // a block inside itself
    CHECK(wouldCreateCycle(doc, a, c));  // A would contain C, which contains B, which contains A
    CHECK(wouldCreateCycle(doc, b, c));
    CHECK_FALSE(wouldCreateCycle(doc, c, a));
    auto dep = dependentBlocks(doc, {a});
    CHECK(dep == std::vector<BlockId>{a, b, c});
}

TEST_CASE("insert bounds follow the instance transform, nested too") {
    Document doc;
    BlockId a = addBlock(doc, "A", {line(0, 0, 10, 0), circle(0, 0, 1)});
    BBox2 bb = blockBounds(doc, a);
    CHECK(near(bb.min, {-1, -1}));
    CHECK(near(bb.max, {10, 1}));

    EntityId i = addEntity(doc, insertOf(a, {100, 0}, {1, 1}, kPi / 2));
    BBox2 ib = insertBounds(doc, *doc.find(i));
    CHECK(near(ib.min, {99, -1}, 1e-9));
    CHECK(near(ib.max, {101, 10}, 1e-9));

    BlockId outer = addBlock(doc, "Outer", {insertOf(a, {0, 5})});
    EntityId j = addEntity(doc, insertOf(outer, {0, 0}));
    BBox2 jb = insertBounds(doc, *doc.find(j));
    CHECK(near(jb.min, {-1, 4}));
    CHECK(near(jb.max, {10, 6}));
}

TEST_CASE("snap geometry flattens nested inserts into the insert's space") {
    Document doc;
    BlockId a = addBlock(doc, "A", {line(0, 0, 10, 0)});
    BlockId b = addBlock(doc, "B", {insertOf(a, {0, 5}), circle(0, 0, 2)});
    EntityId i = addEntity(doc, insertOf(b, {100, 100}, {2, 2}));
    auto geo = snapGeometry(doc, *doc.find(i));
    REQUIRE(geo.size() == 2);
    const auto* ln = entityAs<LineData>(geo[0]);
    REQUIRE(ln);
    CHECK(near(ln->a, {100, 110}));
    CHECK(near(ln->b, {120, 110}));
    const auto* c = entityAs<CircleData>(geo[1]);
    REQUIRE(c);
    CHECK(near(c->center, {100, 100}));
    CHECK(c->radius == doctest::Approx(4));
}
