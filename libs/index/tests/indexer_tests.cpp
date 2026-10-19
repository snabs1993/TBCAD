#include <doctest/doctest.h>

#include <algorithm>
#include <mutex>
#include <random>

#include "tbcad/core/core.h"
#include "tbcad/index/index.h"
#include "test_util.h"

using namespace tbcad;
using namespace tbcad::index;

namespace {

BBox2 boundsOf(const Entity& e) {
    BBox2 b;
    if (const auto* l = entityAs<LineData>(e)) {
        b.expand(l->a);
        b.expand(l->b);
    } else if (const auto* c = entityAs<CircleData>(e)) {
        b.expand({c->center.x - c->radius, c->center.y - c->radius});
        b.expand({c->center.x + c->radius, c->center.y + c->radius});
    } else if (const auto* p = entityAs<PointData>(e)) {
        b.expand(p->p);
    }
    return b;  // other kinds: empty -> not indexed
}

Entity line(double x1, double y1, double x2, double y2) {
    return Entity{{}, LineData{{x1, y1}, {x2, y2}}};
}

// What the index should hold, computed from scratch.
test::Model expected(const Document& doc) {
    test::Model m;
    doc.forEachEntity([&](const Entity& e) {
        if (doc.ownerBlock(e.common.id))
            return;
        BBox2 b = boundsOf(e);
        if (!b.empty())
            m[e.common.id.value] = b;
    });
    return m;
}

void checkSame(const Document& doc, const SpatialIndex& idx) {
    test::Model m = expected(doc);
    REQUIRE(idx.size() == m.size());
    REQUIRE(idx.validate());
    for (const auto& [id, b] : m) {
        BBox2 got = idx.boxOf(EntityId{id});
        REQUIRE(!got.empty());
        REQUIRE(got.min == b.min);
        REQUIRE(got.max == b.max);
    }
    std::mt19937_64 rng(m.size() + 1);
    std::uniform_real_distribution<double> u(-100, 1100);
    for (int q = 0; q < 50; ++q) {
        double x = u(rng), y = u(rng);
        BBox2 w{{x, y}, {x + 200, y + 200}};
        REQUIRE(test::indexQuery(idx, w) == test::bruteQuery(m, w));
    }
}

}  // namespace

TEST_CASE("indexer follows commit, undo and redo") {
    Document doc;
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> u(0, 1000);
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("seed");
        for (int i = 0; i < 300; ++i) {
            auto r = tx.add(i % 5 == 0 ? Entity{{}, CircleData{{u(rng), u(rng)}, 5}}
                                       : line(u(rng), u(rng), u(rng), u(rng)));
            REQUIRE(r);
            ids.push_back(r.value());
        }
        tx.commit();
    }
    SpatialIndex idx;
    DocumentIndexer indexer(doc, idx, boundsOf);
    checkSame(doc, idx);
    CHECK(idx.size() == 300);

    std::vector<ChangeSet> history;
    for (int step = 0; step < 40; ++step) {
        Transaction tx = doc.begin("edit");
        for (int k = 0; k < 6; ++k) {
            double r = u(rng) / 1000;
            EntityId pick = ids[rng() % ids.size()];
            if (r < 0.3 && doc.contains(pick)) {
                REQUIRE(tx.erase(pick));
            } else if (r < 0.7 && doc.contains(pick)) {
                double dx = u(rng);
                REQUIRE(tx.modify(pick, [&](Entity& e) {
                    if (auto* l = entityAs<LineData>(e)) {
                        l->a.x += dx;
                    } else if (auto* c = entityAs<CircleData>(e)) {
                        c->radius += 1;
                    }
                }));
            } else {
                auto nid = tx.add(line(u(rng), u(rng), u(rng), u(rng)));
                REQUIRE(nid);
                ids.push_back(nid.value());
            }
        }
        history.push_back(tx.commit());
        checkSame(doc, idx);
    }
    // undo everything, then redo everything, checking at every step
    for (std::size_t i = history.size(); i-- > 0;) {
        doc.apply(history[i], Direction::Undo);
        checkSame(doc, idx);
    }
    CHECK(idx.size() == 300);
    for (const ChangeSet& cs : history) {
        doc.apply(cs, Direction::Redo);
        checkSame(doc, idx);
    }
}

TEST_CASE("rolled-back transactions do not touch the index") {
    Document doc;
    SpatialIndex idx;
    DocumentIndexer indexer(doc, idx, boundsOf);
    {
        Transaction tx = doc.begin("temp");
        REQUIRE(tx.add(line(0, 0, 1, 1)));
    }  // destroyed without commit
    CHECK(idx.size() == 0);
    {
        Transaction tx = doc.begin("real");
        REQUIRE(tx.add(line(0, 0, 1, 1)));
        tx.commit();
    }
    CHECK(idx.size() == 1);
}

TEST_CASE("block definition entities are not indexed - unsupported kinds are skipped") {
    Document doc;
    SpatialIndex idx;
    DocumentIndexer indexer(doc, idx, boundsOf);
    BlockId blk;
    {
        Transaction tx = doc.begin("block");
        BlockDef def;
        def.name = "B";
        auto b = tx.addBlock(def);
        REQUIRE(b);
        blk = b.value();
        REQUIRE(tx.add(line(0, 0, 5, 5), blk));
        REQUIRE(tx.add(line(10, 10, 15, 15)));
        tx.commit();
    }
    CHECK(idx.size() == 1);
    checkSame(doc, idx);
}

TEST_CASE("a large transaction is handled and the indexer unregisters on destruction") {
    Document doc;
    SpatialIndex idx;
    {
        DocumentIndexer indexer(doc, idx, boundsOf);
        Transaction tx = doc.begin("bulk");
        for (int i = 0; i < 5000; ++i)
            REQUIRE(tx.add(line(i, 0, i + 1, 1)));
        tx.commit();
        CHECK(idx.size() == 5000);
        checkSame(doc, idx);
    }
    Transaction tx = doc.begin("after");  // would crash if the dead observer were still called
    REQUIRE(tx.add(line(0, 0, 1, 1)));
    tx.commit();
    CHECK(idx.size() == 5000);

    DocumentIndexer again(doc, idx, boundsOf);  // reloads from the document
    CHECK(idx.size() == 5001);
    again.rebuild();
    CHECK(idx.size() == 5001);
}

TEST_CASE("erasing a third of a big drawing keeps the index right, and so does its undo") {
    Document doc;
    SpatialIndex idx;
    DocumentIndexer indexer(doc, idx, boundsOf);
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("bulk");
        for (int i = 0; i < 6000; ++i) {
            auto r = tx.add(line(i, 0, i + 1, 1));
            REQUIRE(r);
            ids.push_back(r.value());
        }
        tx.commit();
    }
    ChangeSet erased;
    {
        Transaction tx = doc.begin("erase");
        for (std::size_t i = 0; i < ids.size(); i += 3)
            REQUIRE(tx.erase(ids[i]));
        erased = tx.commit();
    }
    CHECK(idx.size() == 4000);
    checkSame(doc, idx);
    doc.apply(erased, Direction::Undo);
    CHECK(idx.size() == 6000);
    checkSame(doc, idx);
}

TEST_CASE("erasing everything and undoing it again and again keeps the boxes right") {
    Document doc;
    SpatialIndex idx;
    DocumentIndexer indexer(doc, idx, boundsOf);
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("bulk");
        for (int i = 0; i < 3000; ++i) {
            auto r = tx.add(line(i, 0, i + 1, 1 + i % 7));
            REQUIRE(r);
            ids.push_back(r.value());
        }
        tx.commit();
    }
    ChangeSet erased;
    {
        Transaction tx = doc.begin("erase all");
        for (EntityId id : ids)
            REQUIRE(tx.erase(id));
        erased = tx.commit();
    }
    CHECK(idx.size() == 0);
    // The boxes the index held are kept with the change: coming back must give the same ones.
    for (int round = 0; round < 3; ++round) {
        doc.apply(erased, Direction::Undo);
        CHECK(idx.size() == 3000);
        checkSame(doc, idx);
        doc.apply(erased, Direction::Redo);
        CHECK(idx.size() == 0);
        checkSame(doc, idx);
    }
}

TEST_CASE("a big load measured on several threads gives the same index") {
    Document doc;
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> u(0, 1000);
    {
        Transaction tx = doc.begin("seed");
        for (int i = 0; i < 40000; ++i)
            REQUIRE(tx.add(i % 7 == 0 ? Entity{{}, CircleData{{u(rng), u(rng)}, 5}}
                                      : line(u(rng), u(rng), u(rng), u(rng))));
        tx.commit();
    }
    int scopes = 0;
    std::mutex m;
    SpatialIndex idx;
    DocumentIndexer indexer(
        doc, idx, boundsOf, {},
        [&]() -> std::shared_ptr<void> {
            std::lock_guard<std::mutex> lock(m);
            ++scopes;
            return {};
        },
        4);
    CHECK(scopes >= 2);
    CHECK(idx.size() == 40000);
    checkSame(doc, idx);
}
