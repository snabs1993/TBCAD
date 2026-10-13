#include "support.h"

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

TEST_CASE("INSERT without blocks says so; Esc inserts nothing") {
    Fixture f;
    f.host->run("INSERT", {});
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
