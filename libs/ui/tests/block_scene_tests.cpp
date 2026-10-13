#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/ui/scene_sync.h"
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
