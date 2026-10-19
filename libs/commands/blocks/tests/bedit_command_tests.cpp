#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

// A block "Sym" of one 10 mm line, inserted at the origin and at (100,50).
struct BEditFixture : Fixture {
    EntityId second{};
    BEditFixture() {
        EntityId line = addLine(0, 0, 10, 0);
        host->setSelection({line});
        host->run("BLOCK", {"Sym", "0,0", "C"});
        const BlockDef& def = doc.blocks().begin()->second;
        Entity e;
        e.data = Indirect<InsertData>(InsertData{def.id, {100, 50}, {1, 1}, 0});
        second = add(std::move(e));
    }
    const BlockDef& def() const { return doc.blocks().begin()->second; }
    std::size_t lines() const {
        std::size_t n = 0;
        for (EntityId id : modelSpace())
            if (kind(*doc.find(id)) == EntityKind::Line)
                ++n;
        return n;
    }
};

}  // namespace

TEST_CASE(
    "BEDIT copies the block next to its insert; BCLOSE writes the edit back to every insert") {
    BEditFixture f;
    REQUIRE(f.lines() == 0);
    auto r = f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    CHECK(r.finished);
    REQUIRE(f.lines() == 1);
    EntityId copy;
    for (EntityId id : f.modelSpace())
        if (kind(*f.doc.find(id)) == EntityKind::Line)
            copy = id;
    const auto* ln = entityAs<LineData>(*f.doc.find(copy));
    CHECK(near(ln->a, {100, 50}));
    CHECK(near(ln->b, {110, 50}));

    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(copy, [](Entity& e) { entityAs<LineData>(e)->b = {130, 50}; }).ok());
        tx.commit();
    }
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.lines() == 0);  // the working copies are gone
    REQUIRE(f.def().entities.size() == 1);
    const auto* back = entityAs<LineData>(*f.doc.find(f.def().entities[0]));
    CHECK(near(back->a, {0, 0}));
    CHECK(near(back->b, {30, 0}));  // back in block space
    // BEDIT again is possible once the first edit is closed.
    f.host->run("BEDIT", {"Name", "Sym"});
    CHECK(f.lines() == 1);
}

TEST_CASE("BCLOSE Discard drops the copies and leaves the block alone - BSAVE keeps editing") {
    BEditFixture f;
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    f.host->run("BCLOSE", {"Discard"});
    CHECK(f.lines() == 0);
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {10, 0}));

    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    EntityId copy;
    for (EntityId id : f.modelSpace())
        if (kind(*f.doc.find(id)) == EntityKind::Line)
            copy = id;
    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(copy, [](Entity& e) { entityAs<LineData>(e)->b = {160, 50}; }).ok());
        tx.commit();
    }
    f.host->run("BSAVE", {});
    CHECK(f.lines() == 1);  // still editing
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {60, 0}));
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    CHECK(f.host->logContains("already being edited"));
}

TEST_CASE("BSAVE and BCLOSE without BEDIT explain themselves") {
    BEditFixture f;
    f.host->run("BSAVE", {});
    CHECK(f.host->logContains("No block is being edited"));
}

TEST_CASE("BSAVE keeps objects drawn during BEDIT and BEDIT takes a typed block name") {
    BEditFixture f;
    f.host->run("BEDIT", {"Sym"});  // the block name typed at the prompt
    REQUIRE(f.lines() == 1);
    f.addLine(0, 20, 5, 20);  // drawn while editing
    f.host->run("BSAVE", {});
    CHECK(f.host->logContains("updated from 2 objects"));
    CHECK(f.def().entities.size() == 2);
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.lines() == 0);
    CHECK(f.def().entities.size() == 2);
}

TEST_CASE("REFEDIT and REFCLOSE work like BEDIT and BCLOSE, closing undoes in one step") {
    BEditFixture f;
    f.host->run("REFEDIT", {cmd::ScriptItem::pick(f.second)});
    REQUIRE(f.lines() == 1);
    EntityId copy;
    for (EntityId id : f.modelSpace())
        if (kind(*f.doc.find(id)) == EntityKind::Line)
            copy = id;
    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(copy, [](Entity& e) { entityAs<LineData>(e)->b = {150, 50}; }).ok());
        tx.commit();
    }
    f.host->run("REFCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.lines() == 0);
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {50, 0}));
    CHECK(f.host->undo());
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {10, 0}));
}

TEST_CASE("REFSET Add moves a drawing object into the block on BCLOSE, Remove keeps it out") {
    BEditFixture f;
    EntityId extra = f.addLine(200, 0, 210, 0);
    EntityId drawn = f.addLine(300, 0, 310, 0);
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    f.host->setSelection({});
    f.host->run("REFSET", {"Add", cmd::ScriptItem::select({extra}), cmd::ScriptItem::enter()});
    f.host->setSelection({});
    f.host->run("REFSET", {"Remove", cmd::ScriptItem::select({drawn}), cmd::ScriptItem::enter()});
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.def().entities.size() == 2);  // original line + the added one
    CHECK(f.doc.find(extra) == nullptr);  // moved into the block
    CHECK(f.doc.find(drawn) != nullptr);  // untouched
    CHECK(f.host->undo());
    CHECK(f.def().entities.size() == 1);
    CHECK(f.doc.find(extra) != nullptr);
}

TEST_CASE("REFSET without BEDIT explains itself") {
    BEditFixture f;
    f.host->run("REFSET", {});
    CHECK(f.host->logContains("No block is being edited"));
}

TEST_CASE("Editing a block that contains another block updates both levels, and undo restores") {
    BEditFixture f;
    // Outer block "Outer" holds an insert of Sym.
    Entity in;
    in.data = Indirect<InsertData>(InsertData{f.def().id, {5, 5}, {1, 1}, 0});
    EntityId nested = f.add(std::move(in));
    f.host->setSelection({nested});
    f.host->run("BLOCK", {"Outer", "0,0", "C"});
    const BlockDef* outer = f.doc.blockByName("Outer");
    REQUIRE(outer);
    Entity oi;
    oi.data = Indirect<InsertData>(InsertData{outer->id, {1000, 0}, {1, 1}, 0});
    EntityId outerIns = f.add(std::move(oi));

    // Edit the inner block through the old insert; the outer block's insert follows.
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    EntityId copy;
    for (EntityId id : f.modelSpace())
        if (f.doc.find(id) && kind(*f.doc.find(id)) == EntityKind::Line)
            copy = id;
    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(copy, [](Entity& e) { entityAs<LineData>(e)->b = {180, 50}; }).ok());
        tx.commit();
    }
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {80, 0}));
    CHECK(f.doc.find(outerIns) != nullptr);
    CHECK(f.host->undo());
    CHECK(near(entityAs<LineData>(*f.doc.find(f.def().entities[0]))->b, {10, 0}));
}

TEST_CASE("BSAVE keeps the draw order of the edited objects in the block") {
    BEditFixture f;
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.second)});
    // A second line drawn on top, then sent to the back with DRAWORDER.
    EntityId top = f.addLine(100, 60, 110, 60);
    f.host->run("DRAWORDER", {cmd::ScriptItem::select({top}), cmd::ScriptItem::enter(), "Back"});
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    REQUIRE(f.def().entities.size() == 2);
    const auto* first = entityAs<LineData>(*f.doc.find(f.def().entities[0]));
    CHECK(near(first->a, {0, 10}));  // the new line is now at the back
}
