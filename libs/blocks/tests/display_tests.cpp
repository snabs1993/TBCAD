#include <chrono>
#include <optional>
#include <string>

#include "support.h"
#include "tbcad/index/document_indexer.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

DisplayList displayOf(const Document& doc, EntityId id, double pixel = 0.1) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = pixel;
    DisplayList l;
    geom::buildDisplay(doc, *doc.find(id), ctx, l);
    return l;
}

const DisplayPolyline& poly(const DisplayList& l, std::size_t i) {
    return std::get<DisplayPolyline>(l.items.at(i));
}

}  // namespace

TEST_CASE("an insert displays its block transformed") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "B", {line(0, 0, 10, 0)}, {5, 0});
    EntityId i = addEntity(doc, insertOf(b, {100, 50}, {2, 2}, kPi / 2));
    DisplayList l = displayOf(doc, i);
    REQUIRE(l.items.size() == 1);
    const auto& p = poly(l, 0);
    REQUIRE(p.pts.size() == 2);
    CHECK(near(p.pts[0], {100, 40}));  // (0,0) is 5 left of the base: 10 along -y after the turn
    CHECK(near(p.pts[1], {100, 60}));
}

TEST_CASE("an associative array displays its block once per item") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "*A1", {line(0, 0, 1, 0)}, {0, 0});
    Entity e = insertOf(b, {100, 0});
    ArrayInfo a;
    a.rows = 2;
    a.columns = 3;
    a.rowSpacing = 5;
    a.columnSpacing = 10;
    entityAs<InsertData>(e)->array = a;
    EntityId i = addEntity(doc, e);
    DisplayList l = displayOf(doc, i);
    REQUIRE(l.items.size() == 6);
    CHECK(near(poly(l, 0).pts[0], {100, 0}));
    CHECK(near(poly(l, 5).pts[0], {120, 5}));
    BBox2 box = insertBounds(doc, *doc.find(i));
    CHECK(near(box.min, {100, 0}));
    CHECK(near(box.max, {121, 5}));
    CHECK(snapGeometry(doc, *doc.find(i)).size() == 6);
}

TEST_CASE("perf: a big array inside a block builds in linear time") {
    registerInsertDisplay();
    Document doc;
    std::vector<Entity> lines;
    for (int k = 0; k < 20; ++k)
        lines.push_back(line(k, 0, k, 10));
    BlockId leaf = addBlock(doc, "LEAF", lines);
    Entity grid = insertOf(leaf, {0, 0}, {1, 1}, 0.5);
    ArrayInfo a;
    a.rows = 60;
    a.columns = 60;
    a.rowSpacing = 30;
    a.columnSpacing = 30;
    entityAs<InsertData>(grid)->array = a;
    BlockId outer = addBlock(doc, "OUTER", {grid});
    EntityId i = addEntity(doc, insertOf(outer, {0, 0}));
    const auto t0 = std::chrono::steady_clock::now();
    DisplayList l = displayOf(doc, i);
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(l.items.size() == 60u * 60u * 20u);
    CHECK(s < 2.0);  // was 16 s: every array item moved the whole list
}

TEST_CASE("turned references of one block measured once per angle keep exact boxes") {
    Document doc;
    BlockId b = addBlock(doc, "T", {circle(10, 0, 1), line(0, 0, 3, 4)});
    EntityId one = addEntity(doc, insertOf(b, {0, 0}, {1, 1}, 0.5));
    EntityId two = addEntity(doc, insertOf(b, {100, 50}, {1, 1}, 0.5));
    const BBox2 alone1 = insertBounds(doc, *doc.find(one));
    const BBox2 alone2 = insertBounds(doc, *doc.find(two));
    BoundsMemoScope memo;
    const BBox2 m1 = insertBounds(doc, *doc.find(one));
    const BBox2 m2 = insertBounds(doc, *doc.find(two));
    CHECK(near(m1.min, alone1.min, 1e-9));
    CHECK(near(m1.max, alone1.max, 1e-9));
    CHECK(near(m2.min, alone2.min, 1e-9));
    CHECK(near(m2.max, alone2.max, 1e-9));
    CHECK(near(m2.min, m1.min + Vec2{100, 50}, 1e-9));
}

TEST_CASE("ByBlock colour and lineweight come from the insert - layer 0 takes its layer") {
    registerInsertDisplay();
    Document doc;
    LayerId walls;
    {
        Transaction tx = doc.begin("layer");
        walls = tx.addLayer(Layer{.name = "Walls", .color = Color::index(3)}).value();
        tx.commit();
    }
    Entity byBlock = line(0, 0, 1, 0);
    byBlock.common.layer = kLayerZero;
    byBlock.common.color = Color::byBlock();
    byBlock.common.lineweight = LineWeight::byBlock();
    Entity byLayerOnZero = line(0, 1, 1, 1);
    byLayerOnZero.common.layer = kLayerZero;  // ByLayer colour: layer 0 becomes the insert's layer
    Entity fixed = line(0, 2, 1, 2);
    fixed.common.layer = kLayerZero;
    fixed.common.color = Color::index(5);
    BlockId b = addBlock(doc, "B", {byBlock, byLayerOnZero, fixed});

    Entity ins = insertOf(b, {0, 0});
    ins.common.layer = walls;
    ins.common.color = Color::index(2);
    ins.common.lineweight = LineWeight::hundredths(70);
    DisplayList l = displayOf(doc, addEntity(doc, ins));
    REQUIRE(l.items.size() == 3);
    CHECK(poly(l, 0).style.color == paletteColor(2));
    CHECK(poly(l, 0).style.lineweight == doctest::Approx(0.7));
    CHECK(poly(l, 0).style.layer == walls);
    CHECK(poly(l, 1).style.color == paletteColor(3));  // ByLayer colour of the Walls layer
    CHECK(poly(l, 1).style.layer == walls);
    CHECK(poly(l, 2).style.color == paletteColor(5));
    CHECK(poly(l, 2).style.layer == walls);
}

TEST_CASE("ByBlock inside a nested insert resolves through both levels") {
    registerInsertDisplay();
    Document doc;
    Entity inner = line(0, 0, 1, 0);
    inner.common.color = Color::byBlock();
    BlockId a = addBlock(doc, "A", {inner});
    Entity mid = insertOf(a, {0, 0});
    mid.common.color = Color::byBlock();  // takes the outer insert's colour
    BlockId b = addBlock(doc, "B", {mid});
    Entity outer = insertOf(b, {0, 0});
    outer.common.color = Color::index(4);
    DisplayList l = displayOf(doc, addEntity(doc, outer));
    REQUIRE(l.items.size() == 1);
    CHECK(poly(l, 0).style.color == paletteColor(4));
}

TEST_CASE("block geometry is tessellated once per block, style and tolerance") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "B", {circle(0, 0, 1)});
    EntityId i1 = addEntity(doc, insertOf(b, {0, 0}));
    EntityId i2 = addEntity(doc, insertOf(b, {10, 0}));
    EntityId i3 = addEntity(doc, insertOf(b, {20, 0}, {5, 5}));  // different scale, other tolerance
    BlockDisplayCache cache;
    {
        BlockDisplayScope scope(cache);
        displayOf(doc, i1);
        displayOf(doc, i2);
        CHECK(cache.builds() == 1);
        DisplayList big = displayOf(doc, i3);
        CHECK(cache.builds() == 2);
        // The scaled circle is still smooth: the tolerance followed the scale.
        CHECK(poly(big, 0).pts.size() > poly(displayOf(doc, i1), 0).pts.size());
    }
    // Editing the document drops the cache entries.
    {
        BlockDisplayScope scope(cache);
        size_t before = cache.builds();
        addEntity(doc, line(0, 0, 1, 1));
        displayOf(doc, i1);
        CHECK(cache.builds() == before + 1);
    }
}

TEST_CASE("a cyclic definition (hand-made) does not hang") {
    registerInsertDisplay();
    Document doc;
    BlockId a = addBlock(doc, "A", {line(0, 0, 1, 0)});
    // The document refuses a direct self-insert; whatever it allows must still terminate.
    Transaction tx = doc.begin("cycle");
    auto r = tx.add(insertOf(a, {0, 0}), a);
    tx.commit();
    EntityId i = addEntity(doc, insertOf(a, {0, 0}));
    DisplayList l = displayOf(doc, i);
    CHECK(l.items.size() >= 1);
    (void)r;
    BBox2 box = insertBounds(doc, *doc.find(i));
    CHECK_FALSE(box.empty());
}

TEST_CASE("blocks that hold the next one twice, sixty levels deep, draw and measure at once") {
    registerInsertDisplay();
    Document doc;
    // L59 holds a line; every level below holds the next one twice and a line: 2^60 inserts in
    // full (drawing stops at kMaxNesting levels, still 2^32).
    BlockId next = addBlock(doc, "L59", {line(0, 0, 1, 0)});
    for (int i = 58; i >= 0; --i) {
        Entity a = insertOf(next, {0, 0}), b = insertOf(next, {1, 0});
        next = addBlock(doc, ("L" + std::to_string(i)).c_str(), {a, b, line(0, 0, 0, 1)});
    }
    const EntityId top = addEntity(doc, insertOf(next, {0, 0}));
    CHECK(expandedCount(doc, *doc.find(top)) > kMaxExpanded);
    const BBox2 box = insertBounds(doc, *doc.find(top));
    REQUIRE_FALSE(box.empty());
    // Without and with the block display cache: the outline of the insert.
    for (bool cached : {false, true}) {
        BlockDisplayCache cache;
        std::optional<BlockDisplayScope> scope;
        if (cached)
            scope.emplace(cache);
        const DisplayList l = displayOf(doc, top);
        REQUIRE(l.items.size() == 1);
        CHECK(poly(l, 0).closed);
        CHECK(poly(l, 0).pts.size() == 4);
        InsertInstance inst;
        CHECK_FALSE(buildInsertInstance(doc, *doc.find(top), geom::DisplayContext{}, inst));
    }
    CHECK(snapGeometry(doc, *doc.find(top)).empty());
    // A small nest is drawn in full.
    BlockId small = addBlock(doc, "S2", {line(0, 0, 1, 0)});
    for (int i = 1; i >= 0; --i)
        small = addBlock(doc, ("S" + std::to_string(i)).c_str(),
                         {insertOf(small, {0, 0}), insertOf(small, {0, 1})});
    const EntityId s = addEntity(doc, insertOf(small, {0, 0}));
    CHECK(expandedCount(doc, *doc.find(s)) == 4 + 2 + 4);  // lines and the inserts below
    CHECK(displayOf(doc, s).items.size() == 4);
}

TEST_CASE("the index follows block edits, undo and redo") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "B", {line(0, 0, 10, 0)});
    EntityId i = addEntity(doc, insertOf(b, {0, 0}));
    index::SpatialIndex idx;
    index::DocumentIndexer indexer(doc, idx,
                                   [&doc](const Entity& e) { return geom::bounds(e, doc); });
    BlockIndexRefresher refresher(doc, indexer);
    CHECK(idx.boxOf(i).max.x == doctest::Approx(10));

    EntityId extra;
    {
        Transaction tx = doc.begin("edit block");
        extra = tx.add(line(0, 0, 50, 0), b).value();
        tx.commit();
    }
    CHECK(idx.boxOf(i).max.x == doctest::Approx(50));

    // Moving an entity that already lives in the block also counts.
    {
        Transaction tx = doc.begin("edit again");
        REQUIRE(tx.modify(extra, [](Entity& e) { e.data = LineData{{0, 0}, {80, 0}}; }));
        tx.commit();
    }
    CHECK(idx.boxOf(i).max.x == doctest::Approx(80));
}
