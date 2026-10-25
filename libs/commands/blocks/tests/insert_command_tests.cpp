#include "support.h"
#include "tbcad/cmd/script_host.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

BlockId makeDoorBlock(Fixture& f) {
    EntityId a = f.addLine(0, 0, 10, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Door", "0,0", "R"});
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Door2", "0,0", "D"});
    return f.doc.blockByName("Door")->id;
}

}  // namespace

TEST_CASE("INSERT places an instance with scale and rotation") {
    Fixture f;
    BlockId door = makeDoorBlock(f);
    auto r = f.host->run("INSERT", {"Door", "S", "2", "R", "90", "100,50"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->block == door);
    CHECK(ins->position == Vec2{100, 50});
    CHECK(ins->scale == Vec2{2, 2});
    CHECK(ins->rotation == doctest::Approx(kPi / 2));
    CHECK(f.host->undoStack().undoLabel() == "Insert Door");
    CHECK(f.host->undo());
    CHECK(f.modelSpace().empty());
}

TEST_CASE("INSERT supports separate X and Y scale and mirrored scale") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("INSERT", {"door", "X", "3", "Y", "-1", "0,0"});
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->scale == Vec2{3, -1});
}

TEST_CASE("INSERT asks again for an unknown block and lists them with ?") {
    Fixture f;
    makeDoorBlock(f);
    f.host->clearLog();
    auto r = f.host->run("INSERT", {"Nope", "?", "Door", "0,0"});
    CHECK(r.finished);
    CHECK(f.host->logContains("No block named"));
    CHECK(f.host->logContains("Blocks: Door"));
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("INSERT remembers the last block as the default") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("INSERT", {"Door", "0,0"});
    f.host->run("INSERT", {"", "10,0"});  // Enter takes the default
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("INSERT without blocks says so - Esc inserts nothing") {
    Fixture f;
    f.host->run("INSERT", {cmd::ScriptItem::esc()});
    CHECK(f.host->logContains("no blocks"));
    makeDoorBlock(f);
    f.host->run("INSERT", {"Door", cmd::ScriptItem::esc()});
    CHECK(f.modelSpace().empty());
}

TEST_CASE("INSERT shows a preview while the point is chosen") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("INSERT", {"Door"});
    cmd::RawInput in{{30, 0}, {30, 0}, 1};
    f.host->runner().pointerMoved(in);
    CHECK_FALSE(f.host->preview().items.empty());
}

TEST_CASE("INSERT Multiple keeps placing copies until Enter and undoes as one step") {
    Fixture f;
    makeDoorBlock(f);
    auto r = f.host->run("INSERT",
                         {"Door", "S", "2", "M", "0,0", "10,0", "20,0", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.modelSpace().size() == 3);
    CHECK(f.host->logContains("inserted (3)"));
    CHECK(f.host->undo());  // the whole series is one command
    CHECK(f.modelSpace().empty());
}

TEST_CASE("INSERT Explode places the parts and undoes in one step") {
    Fixture f;
    makeDoorBlock(f);
    auto r = f.host->run("INSERT", {"Door", "S", "2", "E", "100,50"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const Entity* e = f.doc.find(f.modelSpace()[0]);
    REQUIRE(e);
    CHECK(kind(*e) == EntityKind::Line);
    CHECK(f.host->undoStack().undoLabel() == "Insert Door exploded");
    CHECK(f.host->undo());
    CHECK(f.modelSpace().empty());
}

TEST_CASE("BLOCKREPLACE swaps the block and keeps the placement") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 10, 0);
    EntityId c = f.addCircle(0, 0, 3);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Old", "0,0", "D"});
    f.host->setSelection({c});
    f.host->run("BLOCK", {"New", "0,0", "D"});
    f.host->run("INSERT", {"Old", "50,60"});
    REQUIRE(f.modelSpace().size() == 1);
    EntityId ins = f.modelSpace().back();
    const BlockDef* nb = f.doc.blockByName("New");
    REQUIRE(nb != nullptr);
    const BlockId newId = nb->id;
    f.host->setSelection({ins});
    f.host->run("BLOCKREPLACE", {"Nope"});
    CHECK(f.host->logContains("no block named"));
    f.host->setSelection({ins});
    CHECK(f.host->run("BLOCKREPLACE", {"New"}).finished);
    const auto* d = entityAs<InsertData>(*f.doc.find(ins));
    REQUIRE(d != nullptr);
    CHECK(d->block == newId);
    CHECK(near(d->position, {50, 60}));
    CHECK(f.host->undo());
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->block != newId);
}

#include "tbcad/text/text.h"

TEST_CASE(
    "annotative text inside a block follows the annotation scale; extents match the drawing") {
    text::registerDisplay();
    Fixture f;
    BlockId blk;
    {
        Transaction tx = f.doc.begin("block");
        blk = tx.addBlock(BlockDef{.name = "Tag"}).value();
        Entity t;
        MTextData m;
        m.height = 2.5;
        m.utf8 = "ห้อง A-101";
        t.data = Indirect<MTextData>(m);
        t.common.annotative = true;
        REQUIRE(tx.add(std::move(t), blk).ok());
        REQUIRE(tx.modifyHeader([](Header& h) { h.custom[kAnnoScaleKey] = "100"; }).ok());
        tx.commit();
    }
    Entity ins;
    ins.data = Indirect<InsertData>(InsertData{blk, {1000, 500}, {2, 2}, 0});
    EntityId id = f.add(std::move(ins));
    const Entity* e = f.doc.find(id);
    REQUIRE(e);

    auto drawn = [&](double annoScale) {
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = 1e-3;
        ctx.annoScale = annoScale;
        DisplayList out;
        geom::buildDisplay(f.doc, *e, ctx, out);
        BBox2 b;
        for (const auto& it : out.items)
            if (const auto* fl = std::get_if<DisplayFill>(&it))
                for (const auto& r : fl->rings)
                    for (const Vec2& p : r)
                        b.expand(p);
        return b;
    };
    const BBox2 shown = drawn(0), bounds = geom::bounds(*e, f.doc);
    REQUIRE_FALSE(shown.empty());
    CHECK(bounds.min.x <= shown.min.x + 1e-6);
    CHECK(bounds.min.y <= shown.min.y + 1e-6);
    CHECK(bounds.max.x >= shown.max.x - 1e-6);
    CHECK(bounds.max.y >= shown.max.y - 1e-6);
    // At 1:100 and insert scale 2 the capital height is 2.5 * 100 * 2 = 500.
    CHECK(shown.height() > 400);
    // A viewport at 1:50 draws it half as big.
    const BBox2 half = drawn(50);
    CHECK(half.height() < shown.height() * 0.6);
    CHECK(half.height() > shown.height() * 0.4);
}

TEST_CASE("MINSERT places one insert shown as a rows x columns array") {
    Fixture f;
    BlockId door = makeDoorBlock(f);
    auto r = f.host->run("MINSERT", {"Door", "R", "30", "10,20", "3", "4", "50", "-25"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->block == door);
    REQUIRE(ins->array);
    CHECK(ins->array->rows == 3);
    CHECK(ins->array->columns == 4);
    CHECK(ins->array->rowSpacing == doctest::Approx(50));
    CHECK(ins->array->columnSpacing == doctest::Approx(-25));
    CHECK(f.host->undo());
    CHECK(f.modelSpace().empty());
}

TEST_CASE("MINSERT with one row and one column is a plain insert") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("MINSERT", {"Door", "0,0", "1", "1"});
    REQUIRE(f.modelSpace().size() == 1);
    CHECK_FALSE(entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]))->array);
}

namespace {

struct WindowHost : cmd::ScriptHost {
    using cmd::ScriptHost::ScriptHost;
    std::vector<std::string> opened;
    bool dialogsAvailable() const override { return true; }
    void openPanel(std::string_view id) override { opened.emplace_back(id); }
};

}  // namespace

TEST_CASE("INSERT in a window opens the Blocks palette when the drawing has blocks") {
    Fixture f;
    makeDoorBlock(f);
    WindowHost host(f.doc, f.reg);
    CHECK(host.run("INSERT", {"Door", "0,0"}).finished);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0] == "blocks");

    Fixture empty;  // no blocks: nothing to pick from
    WindowHost bare(empty.doc, empty.reg);
    bare.run("INSERT", {});
    CHECK(bare.opened.empty());
}

TEST_CASE("INSERT Basepoint puts a chosen point of the block on the insertion point") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("INSERT", {"Door", "Basepoint", "10,0", "R", "90", "100,50"});
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    // the block's (10,0) lands on 100,50, so its origin sits 10 below (rotated 90 degrees)
    CHECK(ins->position.x == doctest::Approx(100));
    CHECK(ins->position.y == doctest::Approx(40));
}

TEST_CASE("INSERT PScale works as Scale") {
    Fixture f;
    makeDoorBlock(f);
    f.host->run("INSERT", {"Door", "PScale", "2", "0,0"});
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->scale == Vec2{2, 2});
}
