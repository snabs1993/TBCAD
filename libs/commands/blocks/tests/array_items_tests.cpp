// Single items of associative arrays: ARRAYREMOVE (Ctrl+click), ARRAYEDIT REPlace, Source, RESet.
#include "../src/commands.h"
#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

// Two rows of three circles (radius 1): columns 5 apart, rows 10 apart; item r * 3 + c sits at
// (5c, 10r).
struct ArrayFixture : Fixture {
    EntityId arr{};
    ArrayFixture() {
        EntityId c = addCircle(0, 0, 1);
        host->setSelection({c});
        host->run("ARRAY", {"R", "Rows", "2", "Columns", "3", "Spacing", "5", "10", ""});
        for (EntityId id : modelSpace())
            if (entityAs<InsertData>(*doc.find(id)))
                arr = id;
    }
    const ArrayInfo* info() const {
        const Entity* e = doc.find(arr);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        return ins && ins->array ? &*ins->array : nullptr;
    }
    std::vector<Entity> exploded() const {
        auto ex = blocks::explode(doc, *doc.find(arr));
        return ex.ok() ? ex.value().entities : std::vector<Entity>{};
    }
    bool hasCircleAt(Vec2 p) const {
        for (const Entity& e : exploded())
            if (const auto* c = entityAs<CircleData>(e); c && near(c->center, p, 1e-9))
                return true;
        return false;
    }
};

}  // namespace

TEST_CASE("ARRAYREMOVE takes single items out of an associative array, one undo step") {
    ArrayFixture f;
    REQUIRE(f.info());
    REQUIRE(f.exploded().size() == 6);
    auto r = f.host->run("ARRAYREMOVE", {"5,0", "10,10", "2.5,0", ""});
    CHECK(r.finished);
    REQUIRE(f.info());
    CHECK(f.info()->removed == std::vector<int>{1, 5});
    CHECK(f.exploded().size() == 4);
    CHECK_FALSE(f.hasCircleAt({5, 0}));
    CHECK_FALSE(f.hasCircleAt({10, 10}));
    CHECK(f.hasCircleAt({10, 0}));
    CHECK(f.host->undoStack().undoLabel() == "Remove array items");
    // the extents shrink with the items: the corner item (10,10) is gone, (5,10) stays
    const BBox2 box = blocks::insertBounds(f.doc, *f.doc.find(f.arr));
    CHECK(box.max.x == doctest::Approx(11));
    CHECK(f.host->undo());
    CHECK(f.info()->removed.empty());
    CHECK(f.exploded().size() == 6);
}

TEST_CASE("ARRAYREMOVE of every item erases the array") {
    ArrayFixture f;
    f.host->run("ARRAYREMOVE", {"0,0", "5,0", "10,0", "0,10", "5,10", "10,10", ""});
    CHECK(f.doc.find(f.arr) == nullptr);
    CHECK(f.host->undo());
    CHECK(f.doc.find(f.arr));
}

TEST_CASE("Ctrl+click finds the array item under the cursor") {
    ArrayFixture f;
    auto hit = block_cmds::detail::arrayItemNear(f.doc, {10.5, 10.2}, 0.1);
    REQUIRE(hit);
    CHECK(hit->first == f.arr);
    CHECK(hit->second == 5);
    CHECK_FALSE(block_cmds::detail::arrayItemNear(f.doc, {2.5, 5}, 0.1));
    // a removed item is not found again
    f.host->run("ARRAYREMOVE", {"10,10", ""});
    CHECK_FALSE(block_cmds::detail::arrayItemNear(f.doc, {10.5, 10.2}, 0.1));
}

TEST_CASE("ARRAYEDIT REPlace shows chosen items as other objects, RESet brings them back") {
    ArrayFixture f;
    EntityId bar = f.addLine(100, -1, 100, 1);  // centroid (100, 0)
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "REPlace", cmd::ScriptItem::pick(bar),
                              "", "", "10,0", "0,10", "", "X"});
    REQUIRE(f.info());
    REQUIRE(f.info()->swaps.size() == 2);
    CHECK(f.info()->swaps[0].item == 2);
    CHECK(f.info()->swaps[1].item == 3);
    CHECK(f.doc.find(bar));  // the replacement objects stay where they were
    const auto parts = f.exploded();
    CHECK(parts.size() == 6);
    bool lineAt10 = false, lineAt0 = false;
    for (const Entity& e : parts)
        if (const auto* l = entityAs<LineData>(e)) {
            lineAt10 = lineAt10 || (near(l->a, {10, -1}, 1e-9) && near(l->b, {10, 1}, 1e-9));
            lineAt0 = lineAt0 || (near(l->a, {0, 9}, 1e-9) && near(l->b, {0, 11}, 1e-9));
        }
    CHECK(lineAt10);
    CHECK(lineAt0);
    CHECK_FALSE(f.hasCircleAt({10, 0}));
    CHECK(f.host->undoStack().undoLabel() == "Edit array");

    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "RESet", "X"});
    CHECK(f.info()->swaps.empty());
    CHECK(f.hasCircleAt({10, 0}));
    CHECK(f.host->undo());
    CHECK(f.info()->swaps.size() == 2);
    CHECK(f.host->undo());
    CHECK(f.info()->swaps.empty());
}

TEST_CASE("ARRAYEDIT REPlace Source objects replaces every item still showing the source") {
    ArrayFixture f;
    f.host->run("ARRAYREMOVE", {"5,0", ""});
    EntityId bar = f.addLine(100, -1, 100, 1);
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "REP", cmd::ScriptItem::pick(bar), "",
                              "", "S", "X"});
    REQUIRE(f.info());
    CHECK(f.info()->swaps.size() == 5);
    CHECK(f.info()->removed == std::vector<int>{1});  // a removed item stays removed
    for (const Entity& e : f.exploded())
        CHECK(kind(e) == EntityKind::Line);
}

TEST_CASE("removed items keep their row and column when ARRAYEDIT changes the columns") {
    ArrayFixture f;
    f.host->run("ARRAYREMOVE", {"10,10", ""});  // row 1, column 2
    REQUIRE(f.info()->removed == std::vector<int>{5});
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "Columns", "4", "5", "X"});
    CHECK(f.info()->removed == std::vector<int>{6});  // row 1, column 2 of four
    CHECK_FALSE(f.hasCircleAt({10, 10}));
    CHECK(f.hasCircleAt({15, 10}));
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "Columns", "2", "5", "X"});
    CHECK(f.info()->removed.empty());  // that column is gone
}

TEST_CASE("ARRAYEDIT Source edits the source objects in place and BCLOSE updates every item") {
    ArrayFixture f;
    auto r = f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "Source"});
    CHECK(r.finished);
    EntityId copy{};
    for (EntityId id : f.modelSpace())
        if (entityAs<CircleData>(*f.doc.find(id)))
            copy = id;
    REQUIRE(copy);
    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(copy, [](Entity& e) { entityAs<CircleData>(e)->radius = 2; }).ok());
        tx.commit();
    }
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    const auto parts = f.exploded();
    REQUIRE(parts.size() == 6);
    for (const Entity& e : parts) {
        const auto* c = entityAs<CircleData>(e);
        REQUIRE(c);
        CHECK(c->radius == doctest::Approx(2));
    }
}

TEST_CASE("ARRAYEDIT Base moves the base grip and leaves the items in place, a copied array too") {
    ArrayFixture f;
    // a copy of the array shares its block; it must not move
    EntityId twin;
    {
        Transaction tx = f.doc.begin("copy");
        Entity e = *f.doc.find(f.arr);
        e.common.id = {};
        entityAs<InsertData>(e)->position = {0, 100};
        twin = tx.add(std::move(e)).value();
        tx.commit();
    }
    const BBox2 before = blocks::insertBounds(f.doc, *f.doc.find(f.arr));
    const BBox2 twinBefore = blocks::insertBounds(f.doc, *f.doc.find(twin));
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(f.arr), "Base", "10,10", "X"});
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.arr));
    CHECK(near(ins->position, {10, 10}));
    CHECK(near(geom::gripPoints(*f.doc.find(f.arr))[0], {10, 10}));
    const BBox2 after = blocks::insertBounds(f.doc, *f.doc.find(f.arr));
    CHECK(near(after.min, before.min, 1e-9));
    CHECK(near(after.max, before.max, 1e-9));
    const BBox2 twinAfter = blocks::insertBounds(f.doc, *f.doc.find(twin));
    CHECK(near(twinAfter.min, twinBefore.min, 1e-9));
    CHECK(near(twinAfter.max, twinBefore.max, 1e-9));
    CHECK(f.hasCircleAt({10, 10}));
    CHECK(f.host->undo());
    CHECK(near(entityAs<InsertData>(*f.doc.find(f.arr))->position, {0, 0}));
}

TEST_CASE("path array grips change the spacing and the count along the path") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    EntityId path = f.addLine(0, 0, 100, 0);
    f.host->setSelection({c});
    // 5 items divided over the 100 mm line: 25 apart
    f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(path), "", "", "5", "N"});
    EntityId arr{};
    for (EntityId id : f.modelSpace())
        if (const auto* i = entityAs<InsertData>(*f.doc.find(id)); i && i->array)
            arr = id;
    REQUIRE(arr);
    Entity e = *f.doc.find(arr);
    const auto g = geom::gripPoints(e);
    REQUIRE(g.size() == 3);
    CHECK(near(g[1], {25, 0}, 1e-9));
    CHECK(near(g[2], {100, 0}, 1e-9));
    auto spaced = geom::moveGrip(e, 1, {10, 3});  // the second item 10 along the path
    REQUIRE(spaced.ok());
    const ArrayInfo& s = *entityAs<InsertData>(spaced.value())->array;
    CHECK(s.pathSpacing == doctest::Approx(10));
    CHECK(s.count == 5);
    auto counted = geom::moveGrip(e, 2, {50, -2});  // 25 apart up to 50: 3 items
    REQUIRE(counted.ok());
    CHECK(entityAs<InsertData>(counted.value())->array->count == 3);
}
