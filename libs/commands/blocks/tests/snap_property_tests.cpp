// Object snap on inserted geometry: every snap point lies on the flattened content, the cached
// geometry follows edits, and hovering over a big block is fast.
#include <chrono>
#include <cstdio>
#include <random>

#include "support.h"
#include "tbcad/drafting/drafting.h"
#include "tbcad/geom/entity.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/index/spatial_index.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;
using Clock = std::chrono::steady_clock;

namespace {

struct World : Fixture {
    index::SpatialIndex idx;
    index::DocumentIndexer indexer{doc, idx,
                                   [this](const Entity& e) { return geom::bounds(e, doc); }};

    BlockId makeBlock(const char* name) {
        auto tx = doc.begin("block");
        BlockDef def;
        def.name = name;
        auto b = tx.addBlock(def);
        REQUIRE(b.ok());
        tx.commit();
        return b.value();
    }
    EntityId addIn(BlockId b, EntityData data) {
        auto tx = doc.begin("in block");
        auto id = tx.add(Entity{{}, std::move(data)}, b);
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    EntityId insert(BlockId b, Vec2 pos, Vec2 scale, double rot) {
        InsertData i;
        i.block = b;
        i.position = pos;
        i.scale = scale;
        i.rotation = rot;
        return add(Entity{{}, Indirect<InsertData>(i)});
    }
};

// Distance from `p` to the nearest curve of the flattened content of `insert`.
double distanceToContent(const Document& doc, const Entity& insert, Vec2 p) {
    double best = 1e300;
    for (const Entity& part : blocks::snapGeometry(doc, insert)) {
        std::vector<geom::CurveRef> curves;
        geom::curvesOf(part, curves);
        for (const auto& c : curves)
            best = std::min(best, c.distanceTo(p));
        if (const auto* pt = entityAs<PointData>(part))
            best = std::min(best, distance(pt->p, p));
    }
    return best;
}

void fillBlocks(World& w, BlockId leaf, BlockId nest) {
    w.addIn(leaf, LineData{{0, 0}, {10, 0}});
    w.addIn(leaf, CircleData{{5, 3}, 2});
    w.addIn(leaf, ArcData{{0, 0}, 4, 0.2, 2.0});
    PolylineData pl;
    pl.v = {{{0, 5}, 0.4, 0, 0}, {{6, 5}, 0, 0, 0}, {{6, 9}, -0.3, 0, 0}};
    w.addIn(leaf, pl);
    w.addIn(nest, LineData{{0, 0}, {0, 20}});
    InsertData a;
    a.block = leaf;
    a.position = {3, 2};
    a.scale = {2, 0.5};
    a.rotation = 0.7;
    w.addIn(nest, Indirect<InsertData>(a));
    a.position = {20, 2};
    a.scale = {-1, 1};
    a.rotation = 0;
    w.addIn(nest, Indirect<InsertData>(a));
}

}  // namespace

TEST_CASE(
    "snap points of inserts lie on the flattened content, near the origin and at survey "
    "coordinates") {
    for (const Vec2 origin : {Vec2{0, 0}, Vec2{1.0e6, 2.5e6}, Vec2{-3.0e8, 4.0e8}}) {
        World w;
        const BlockId leaf = w.makeBlock("LEAF");
        const BlockId nest = w.makeBlock("NEST");
        fillBlocks(w, leaf, nest);
        std::mt19937_64 rng(5);
        std::uniform_real_distribution<double> pos(0, 600), ang(0, kTwoPi), sc(0.4, 2.5);
        std::vector<EntityId> inserts;
        for (int i = 0; i < 60; ++i) {
            Vec2 s{sc(rng) * (rng() % 3 == 0 ? -1 : 1), sc(rng) * (rng() % 3 == 0 ? -1 : 1)};
            inserts.push_back(
                w.insert(i % 2 ? leaf : nest, origin + Vec2{pos(rng), pos(rng)}, s, ang(rng)));
        }
        const double tol = 1e-7 * (1 + std::abs(origin.x) + std::abs(origin.y)) + 1e-6;
        std::size_t checked = 0;
        for (int round = 0; round < 400; ++round) {
            // A cursor near a random insert's content.
            const Entity& ins = *w.doc.find(inserts[rng() % inserts.size()]);
            const BBox2 box = geom::bounds(ins, w.doc);
            drafting::SnapRequest rq;
            rq.cursor = {box.min.x + (box.max.x - box.min.x) *
                                         std::uniform_real_distribution<double>(0, 1)(rng),
                         box.min.y + (box.max.y - box.min.y) *
                                         std::uniform_real_distribution<double>(0, 1)(rng)};
            rq.base = origin + Vec2{pos(rng), pos(rng)};
            rq.apertureMm = 12;
            rq.modes = drafting::kAllSnapModes;
            for (const auto& c : drafting::findSnapCandidates(w.doc, w.idx, rq)) {
                // A centre, an insertion point and an apparent intersection are not on the curve.
                if (c.mode == drafting::SnapMode::Center ||
                    c.mode == drafting::SnapMode::Insertion ||
                    c.mode == drafting::SnapMode::ApparentIntersection)
                    continue;
                for (EntityId id : {c.entity, c.entity2}) {
                    if (!id)
                        continue;
                    const Entity* e = w.doc.find(id);
                    REQUIRE(e);
                    if (entityAs<InsertData>(*e)) {
                        CHECK(distanceToContent(w.doc, *e, c.point) < tol);
                    }
                }
                ++checked;
            }
        }
        CHECK(checked > 200);
    }
}

TEST_CASE("cached insert snap geometry follows moves, block edits, undo and redo") {
    World w;
    const BlockId leaf = w.makeBlock("LEAF");
    const EntityId line = w.addIn(leaf, LineData{{0, 0}, {10, 0}});
    const EntityId ins = w.insert(leaf, {100, 100}, {1, 1}, 0);
    auto endpoints = [&] {
        drafting::SnapGeometry g;
        REQUIRE(drafting::SnapGeometryRegistry::instance().collect(w.doc, *w.doc.find(ins), g));
        return g.endpoints;
    };
    auto has = [](const std::vector<Vec2>& v, Vec2 p) {
        for (Vec2 q : v)
            if (near(p, q))
                return true;
        return false;
    };
    CHECK(has(endpoints(), {110, 100}));
    CHECK(has(endpoints(), {110, 100}));  // from the cache
    w.host->setSelection({ins});
    REQUIRE(w.host->run("MOVE", {"100,100", "150,120"}).finished);
    CHECK(has(endpoints(), {160, 120}));
    CHECK_FALSE(has(endpoints(), {110, 100}));
    {
        auto tx = w.doc.begin("edit block");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {30, 0}; }));
        tx.commit();
    }
    CHECK(has(endpoints(), {180, 120}));
    {
        auto tx = w.doc.begin("edit block back");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {10, 0}; }));
        tx.commit();
    }
    CHECK(has(endpoints(), {160, 120}));
    CHECK_FALSE(has(endpoints(), {180, 120}));
    REQUIRE(w.host->undo());
    CHECK(has(endpoints(), {110, 100}));
    REQUIRE(w.host->redo());
    CHECK(has(endpoints(), {160, 120}));
}

TEST_CASE("hovering over a big block among thousands of inserts stays fast") {
    World w;
    const BlockId big = w.makeBlock("BIG");
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> p(0, 1000);
    {
        auto tx = w.doc.begin("fill block");
        for (int i = 0; i < 2000; ++i) {
            const Vec2 a{p(rng), p(rng)};
            REQUIRE(
                tx.add(Entity{{}, LineData{a, a + Vec2{p(rng) * 0.02, p(rng) * 0.02}}}, big).ok());
        }
        tx.commit();
    }
    {
        auto tx = w.doc.begin("inserts");
        for (int i = 0; i < 3000; ++i) {
            InsertData ins;
            ins.block = big;
            ins.position = {(i % 60) * 1500.0, (i / 60) * 1500.0};
            ins.rotation = (i % 7) * 0.3;
            REQUIRE(tx.add(Entity{{}, Indirect<InsertData>(ins)}).ok());
        }
        tx.commit();
    }
    drafting::SnapRequest rq;
    rq.apertureMm = 15;
    rq.modes = drafting::kAllSnapModes;
    // The first visit of each insert flattens its block; later moves over it hit the cache.
    double first = 0, again = 0;
    std::size_t found = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::mt19937_64 mouse(9);
        const auto t0 = Clock::now();
        for (int i = 0; i < 100; ++i) {
            const int k = static_cast<int>(mouse() % 3000);
            rq.cursor = {(k % 60) * 1500.0 + 300, (k / 60) * 1500.0 + 300};
            rq.base = rq.cursor + Vec2{50, 50};
            found += drafting::findSnapCandidates(w.doc, w.idx, rq).size();
        }
        (pass ? again : first) =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / 100;
    }
    std::printf(
        "[osnap perf] 3000 inserts of a 2000-entity block: first visit %.3f ms, repeat %.3f ms per "
        "move\n",
        first, again);
    CHECK(found > 0);
#if defined(NDEBUG)
    CHECK(again < 2.0);
#endif
}
