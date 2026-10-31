#include "helpers.h"
#include "tbcad/collab/ids.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

TEST_CASE("ids: blocks are disjoint and the host's own ids start above them") {
    Document doc;
    IdRanges ranges(doc, 100);
    IdRange a = ranges.grant(1);
    IdRange b = ranges.grant(2);
    IdRange a2 = ranges.grant(1);
    CHECK(a.size() == 100);
    CHECK(a.limit <= b.first);
    CHECK(b.limit <= a2.first);
    CHECK(doc.nextIds().entity >= a2.limit);
    CHECK(ranges.owns(1, EntityId{a.first}));
    CHECK(ranges.owns(1, EntityId{a2.first + 5}));
    CHECK_FALSE(ranges.owns(1, EntityId{b.first}));
    CHECK_FALSE(ranges.owns(3, EntityId{a.first}));
    ranges.release(1);
    CHECK_FALSE(ranges.owns(1, EntityId{a.first}));

    // The host draws above every block.
    Transaction tx = doc.begin("x");
    EntityId e = must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
    CHECK(e.value >= a2.limit);
    tx.commit();
}

TEST_CASE("ids: a document with a window draws inside it and stops at the end") {
    Document doc;
    REQUIRE(doc.setEntityIdWindow(1000, 1003).ok());
    CHECK(doc.entityIdsLeft() == 3);
    {
        Transaction tx = doc.begin("a");
        CHECK(must(tx.add(line(kLayerZero, {0, 0}, {1, 1}))).value == 1000);
        CHECK(must(tx.add(line(kLayerZero, {0, 0}, {1, 1}))).value == 1001);
        tx.commit();
    }
    {
        Transaction tx = doc.begin("b");
        CHECK(must(tx.add(line(kLayerZero, {0, 0}, {1, 1}))).value == 1002);
        auto over = tx.add(line(kLayerZero, {0, 0}, {1, 1}));
        CHECK_FALSE(over.ok());
        tx.commit();
    }
    CHECK(doc.entityIdsLeft() == 0);
    CHECK(doc.nextIds().entity >= 1003);
    doc.clearEntityIdWindow();
    CHECK(doc.entityIdsLeft() > 1000000);
    CHECK_FALSE(doc.setEntityIdWindow(0, 5).ok());
    CHECK_FALSE(doc.setEntityIdWindow(10, 10).ok());
}

TEST_CASE("ids: edits of entities outside the window are recorded with their before image") {
    Document doc;
    // An entity that lives above the window, as another peer's entities do.
    Document src;
    REQUIRE(src.setEntityIdWindow(5000, 5010).ok());
    {
        Transaction tx = src.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        tx.commit();
    }
    REQUIRE(doc.setEntityIdWindow(100, 110).ok());
    struct Rec : DocumentObserver {
        std::vector<ChangeSet> sets;
        void onChanged(const ChangeSet& c) override { sets.push_back(c); }
    } rec;
    doc.addObserver(&rec);
    {
        Entity e = line(kLayerZero, {0, 0}, {1, 1});
        e.common.id = EntityId{5000};
        ChangeSet cs;
        auto snaps = std::make_shared<std::vector<Entity>>();
        snaps->push_back(e);
        cs.snapshots = snaps;
        cs.entities.push_back({kNoSnapshot, 0});
        REQUIRE(doc.applyChecked(cs).ok());
    }
    {
        Transaction tx = doc.begin("move");
        REQUIRE(tx.modify(EntityId{5000}, [](Entity& e) {
                      std::get<LineData>(e.data).b = {9, 9};
                  }).ok());
        (void)must(tx.add(line(kLayerZero, {2, 2}, {3, 3})));
        ChangeSet cs = tx.commit();
        REQUIRE(cs.entities.size() == 2);
        std::size_t modified = 0, added = 0;
        for (const EntityChange& c : cs.entities) {
            modified += (c.before != kNoSnapshot && c.after != kNoSnapshot) ? 1 : 0;
            added += (c.before == kNoSnapshot && c.after != kNoSnapshot) ? 1 : 0;
        }
        CHECK(modified == 1);
        CHECK(added == 1);
        // And it undoes cleanly.
        doc.apply(cs, Direction::Undo);
        CHECK(doc.contains(EntityId{5000}));
        CHECK_FALSE(doc.contains(EntityId{100}));
    }
}

TEST_CASE("replica: pending edits can be read back in order while unanswered") {
    tbcad::collab::testing::Session s;
    auto& g = s.join();
    for (int i = 0; i < 3; ++i) {
        Transaction tx = g.doc->begin("line");
        (void)tx.add(tbcad::collab::testing::line(kLayerZero, {0, double(i)}, {1, double(i)}));
        tx.commit();
    }
    const auto edits = g.replica->pendingEdits();
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].op.clientSeq < edits[1].op.clientSeq);
    CHECK(edits[1].op.clientSeq < edits[2].op.clientSeq);
    CHECK(edits[2].applied.entities.size() == 1);
}
