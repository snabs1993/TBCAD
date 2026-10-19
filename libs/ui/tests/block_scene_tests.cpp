#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/ui/scene_sync.h"
#include "tbcad/ui/selection.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;

namespace {

BlockId makeBlock(Document& doc, int circles) {
    Transaction tx = doc.begin("block");
    BlockId b = tx.addBlock(BlockDef{.name = "Sym"}).value();
    for (int i = 0; i < circles; ++i)
        (void)tx.add(Entity{{}, CircleData{{i * 3.0, 0}, 1.0}}, b);
    (void)tx.add(Entity{{}, LineData{{0, 0}, {circles * 3.0, 0}}}, b);
    tx.commit();
    return b;
}

void addInserts(Document& doc, BlockId b, int n) {
    Transaction tx = doc.begin("inserts");
    const int side = 100;
    for (int i = 0; i < n; ++i) {
        Entity e;
        e.data = Indirect<InsertData>(
            InsertData{b, {(i % side) * 200.0, (i / side) * 100.0}, {1, 1}, (i % 4) * kPi / 2});
        (void)tx.add(std::move(e));
    }
    tx.commit();
}

double seconds(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

}  // namespace

TEST_CASE("the scene shows block contents and follows block edits") {
    blocks::registerInsertDisplay();
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    BlockId b = makeBlock(doc, 2);
    addInserts(doc, b, 3);
    CHECK(scene.entityCount() == 3);
    double before = scene.bounds().width();
    {
        Transaction tx = doc.begin("grow");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {500, 0}}}, b);
        tx.commit();
    }
    CHECK(scene.bounds().width() > before + 100);
}

TEST_CASE("edits outside blocks keep the blocks' geometry, a block edit builds it again") {
    blocks::registerInsertDisplay();
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    BlockId b = makeBlock(doc, 2);
    addInserts(doc, b, 3);
    const std::size_t built = sync.blockBuilds();
    CHECK(built >= 1);
    EntityId first{};
    doc.forEachEntity([&](const Entity& e) {
        if (!first && kind(e) == EntityKind::Insert)
            first = e.common.id;
    });
    {
        Transaction tx = doc.begin("model only");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {10, 10}}});
        (void)tx.erase(first);
        tx.commit();
    }
    CHECK(sync.blockBuilds() == built);
    CHECK(scene.entityCount() == 3);
    {
        Transaction tx = doc.begin("block");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {0, 50}}}, b);
        tx.commit();
    }
    CHECK(sync.blockBuilds() > built);
    CHECK(scene.bounds().height() >= 50);
}

TEST_CASE("the spatial index follows block edits when the refresher is installed") {
    blocks::registerInsertDisplay();
    Document doc;
    BlockId b = makeBlock(doc, 1);
    addInserts(doc, b, 1);
    EntityId ins = doc.modelSpace()[0];
    index::SpatialIndex idx;
    index::DocumentIndexer indexer(doc, idx,
                                   [&doc](const Entity& e) { return geom::bounds(e, doc); });
    blocks::BlockIndexRefresher refresher(doc, indexer);
    double w0 = idx.boxOf(ins).width();
    {
        Transaction tx = doc.begin("grow");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {900, 0}}}, b);
        tx.commit();
    }
    CHECK(idx.boxOf(ins).width() > w0 + 800);
}

TEST_CASE("perf: 10,000 inserts of one block rebuild quickly thanks to the shared block cache") {
    blocks::registerInsertDisplay();
    Document doc;
    BlockId b = makeBlock(doc, 20);
    addInserts(doc, b, 10000);

    // Uncached: every insert tessellates the block itself.
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.1;
    auto t0 = std::chrono::steady_clock::now();
    std::size_t items = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) != EntityKind::Insert)
            return;
        DisplayList l;
        geom::buildDisplay(doc, e, ctx, l);
        items += l.items.size();
    });
    const double uncached = seconds(t0);

    // SceneSync wraps its builds in a BlockDisplayScope.
    render::Scene scene;
    SceneSync sync(doc, scene);
    t0 = std::chrono::steady_clock::now();
    sync.rebuildAll();
    const double cached = seconds(t0);

    CHECK(scene.entityCount() == 10000);
    CHECK(items == 10000u * 21u);
    std::printf(
        "[blocks perf] 10k inserts x 21 entities: display only %.3f s, SceneSync.rebuildAll "
        "(display + scene upload) %.3f s\n",
        uncached, cached);
    CHECK(cached < 30.0);

    // The display step alone, with the cache scope, must beat the uncached build clearly.
    blocks::BlockDisplayCache cache;
    cache.sync(doc.revision());
    t0 = std::chrono::steady_clock::now();
    {
        blocks::BlockDisplayScope scope(cache);
        doc.forEachEntity([&](const Entity& e) {
            if (kind(e) != EntityKind::Insert)
                return;
            DisplayList l;
            geom::buildDisplay(doc, e, ctx, l);
        });
    }
    const double withCache = seconds(t0);
    std::printf("[blocks perf] display with shared cache %.3f s (builds: %zu)\n", withCache,
                cache.builds());
    CHECK(cache.builds() == 1);
    CHECK(withCache < uncached);
}

TEST_CASE("redefining a block (erase and re-add in one step) redraws every insert at once") {
    blocks::registerInsertDisplay();
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    index::SpatialIndex idx;
    index::DocumentIndexer indexer(doc, idx,
                                   [&doc](const Entity& e) { return geom::bounds(e, doc); });
    blocks::BlockIndexRefresher refresher(doc, indexer);
    BlockId b = makeBlock(doc, 1);  // a 3 mm wide symbol
    addInserts(doc, b, 1);
    EntityId ins = doc.modelSpace()[0];
    const double w0 = scene.bounds().width();
    {
        Transaction tx = doc.begin("redefine");
        const std::vector<EntityId> old = doc.block(b)->entities;
        for (EntityId id : old)
            (void)tx.erase(id);
        (void)tx.add(Entity{{}, LineData{{0, 0}, {300, 0}}}, b);
        (void)tx.add(Entity{{}, LineData{{0, 0}, {0, 1000}}}, b);
        tx.commit();
    }
    CHECK(scene.bounds().width() > w0 + 250);
    CHECK(scene.bounds().height() > 900);
    CHECK(idx.boxOf(ins).width() > 250);
    CHECK(idx.boxOf(ins).height() > 900);
}

TEST_CASE(
    "a window around a whole associative array selects it, a window around one item does not") {
    blocks::registerInsertDisplay();
    ui::test::Indexed d;
    BlockId blk;
    {
        Transaction tx = d.doc.begin("array");
        blk = tx.addBlock(BlockDef{.name = "*A1", .anonymous = true}).value();
        (void)tx.add(Entity{{}, CircleData{{0, 0}, 1}}, blk);
        InsertData ins{blk, {}, {1, 1}, 0};
        ArrayInfo info;
        info.rows = 2;
        info.columns = 2;
        info.rowSpacing = 10;
        info.columnSpacing = 10;
        ins.array = info;
        Entity e;
        e.data = Indirect<InsertData>(std::move(ins));
        (void)tx.add(std::move(e));
        tx.commit();
    }
    const EntityId arr = d.doc.modelSpace()[0];
    CHECK(geom::bounds(*d.doc.find(arr), d.doc).max.x == doctest::Approx(11));
    BBox2 all;
    all.expand({-5, -5});
    all.expand({20, 20});
    CHECK(ui::selectInBox(d.doc, d.idx, all, false) == std::vector<EntityId>{arr});
    BBox2 one;
    one.expand({-5, -5});
    one.expand({5, 5});
    CHECK(ui::selectInBox(d.doc, d.idx, one, false).empty());
    CHECK(ui::selectInBox(d.doc, d.idx, one, true) == std::vector<EntityId>{arr});
}

namespace {

std::vector<Vec2> sceneVertices(const render::Scene& s, render::BatchKind kind) {
    std::vector<Vec2> out;
    for (const auto& [tk, tile] : s.tiles())
        for (const auto& [bk, batch] : tile.batches)
            if (bk.kind == kind)
                for (const render::GpuVertex& v : batch.verts)
                    out.push_back({tile.origin.x + double(v.hx) + double(v.lx),
                                   tile.origin.y + double(v.hy) + double(v.ly)});
    std::sort(out.begin(), out.end(),
              [](Vec2 a, Vec2 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    return out;
}

}  // namespace

TEST_CASE("references with attributes and arrays draw the same as the full display list") {
    blocks::registerInsertDisplay();
    Document doc;
    render::Scene fast, plain;
    SceneSync sync(doc, fast);
    BlockId b;
    {
        Transaction tx = doc.begin("block");
        b = tx.addBlock(BlockDef{.name = "Tag"}).value();
        (void)tx.add(Entity{{}, CircleData{{0, 0}, 4.0}}, b);
        (void)tx.add(Entity{{}, LineData{{-5, 0}, {5, 0}}}, b);
        TextData t;
        t.position = {6, -1};
        t.height = 2;
        t.utf8 = "X";
        t.attdef = AttDefInfo{.tag = "NO", .defaultValue = "1"};
        (void)tx.add(Entity{{}, std::move(t)}, b);
        tx.commit();
    }
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("refs");
        for (int i = 0; i < 4; ++i) {
            InsertData ins{b, {i * 40.0, 10.0}, {1, 1}, i * 0.4};
            ins.attribs = blocks::defaultAttribs(doc, b);
            ins.attribs[0].value = "No." + std::to_string(i);
            if (i >= 2) {  // an associative array of 3 x 2 items
                ArrayInfo a;
                a.rows = 2;
                a.columns = 3;
                a.rowSpacing = 15;
                a.columnSpacing = 20;
                ins.array = a;
            }
            Entity e;
            e.data = Indirect<InsertData>(std::move(ins));
            ids.push_back(tx.add(std::move(e)).value());
        }
        tx.commit();
    }
    {
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = sync.pixelSize();
        blocks::BlockDisplayCache cache;
        blocks::BlockDisplayScope scope(cache);
        for (EntityId id : ids) {
            DisplayList list;
            geom::buildDisplay(doc, *doc.find(id), ctx, list);
            plain.upsert(id, list, static_cast<std::uint32_t>(id.value), 1.0 / sync.pixelSize());
        }
    }
    CHECK(fast.entityCount() == plain.entityCount());
    CHECK(sceneVertices(fast, render::BatchKind::Stroke).size() > 1000);
    for (render::BatchKind kind : {render::BatchKind::Stroke, render::BatchKind::Fill}) {
        auto a = sceneVertices(fast, kind), c = sceneVertices(plain, kind);
        REQUIRE(a.size() == c.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            CHECK(distance(a[i], c[i]) < 1e-6);
    }
}
