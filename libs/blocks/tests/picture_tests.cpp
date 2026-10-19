#include "support.h"
#include "tbcad/blocks/picture.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {
BlockId pictureBlock(Document& doc, const char* file) {
    Transaction tx = doc.begin("picture");
    BlockId b = tx.addBlock(BlockDef{.name = "IMG", .xrefPath = file}).value();
    PolylineData frame;
    frame.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{200, 0}, Vec2{200, 100}, Vec2{0, 100}})
        frame.v.push_back({p, 0, 0, 0});
    Entity e;
    e.data = std::move(frame);
    REQUIRE(tx.add(std::move(e), b));
    tx.commit();
    return b;
}
}  // namespace

TEST_CASE("picture corners scale the picture about the opposite corner") {
    Document doc;
    BlockId b = pictureBlock(doc, "photo.png");
    Entity ins = insertOf(b, {10, 20}, {0.5, 0.5});
    auto hs = pictureHandles(doc, ins);
    REQUIRE(hs.size() == 4);
    CHECK(near(hs[0].world, {10, 20}));
    CHECK(near(hs[2].world, {110, 70}));

    // Drag the top-right corner along its diagonal to double the size about (10,20).
    auto big = pictureDrag(doc, ins, 2, {210, 120});
    REQUIRE(big);
    const auto* d = entityAs<InsertData>(big.value());
    CHECK(near(d->position, {10, 20}));
    CHECK(near(d->scale, {1, 1}));
    // Drag the bottom-left one: the top-right corner stays.
    auto small = pictureDrag(doc, ins, 0, {60, 45});
    REQUIRE(small);
    const auto* s = entityAs<InsertData>(small.value());
    CHECK(near(s->scale, {0.25, 0.25}));
    CHECK(near(pictureHandles(doc, small.value())[2].world, {110, 70}));
    // Off the diagonal the corner follows its projection; past the fixed corner it stays tiny.
    auto off = pictureDrag(doc, ins, 2, {110, 20});
    REQUIRE(off);
    CHECK(entityAs<InsertData>(off.value())->scale.x > 0);
    auto flip = pictureDrag(doc, ins, 2, {-500, -500});
    REQUIRE(flip);
    CHECK(entityAs<InsertData>(flip.value())->scale.x > 0);
}

TEST_CASE("only picture inserts have corner grips") {
    Document doc;
    BlockId plain = addBlock(doc, "P", {line(0, 0, 1, 0)});
    CHECK(pictureHandles(doc, insertOf(plain, {0, 0})).empty());
    BlockId xr = pictureBlock(doc, "drawing.dxf");
    CHECK(pictureHandles(doc, insertOf(xr, {0, 0})).empty());
    CHECK_FALSE(pictureDrag(doc, insertOf(plain, {0, 0}), 0, {1, 1}));
}
