#include "support.h"
#include "tbcad/block_cmds/block_list.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("the block commands are registered with their aliases") {
    Fixture f;
    for (const char* n : {"BLOCK", "INSERT", "EXPLODE", "ARRAY"})
        CHECK(f.reg.find(n) != nullptr);
    CHECK(f.reg.find("B") == f.reg.find("BLOCK"));
    CHECK(f.reg.find("I") == f.reg.find("INSERT"));
    CHECK(f.reg.find("X") == f.reg.find("EXPLODE"));
    CHECK(f.reg.find("AR") == f.reg.find("ARRAY"));
}

TEST_CASE("BLOCK converts the selection into a block and one insert; undo restores it") {
    Fixture f;
    EntityId a = f.addLine(10, 10, 20, 10);
    EntityId b = f.addCircle(15, 15, 2);
    f.host->setSelection({a, b});
    auto r = f.host->run("BLOCK", {"Door", "10,10", "C"});
    CHECK(r.finished);
    REQUIRE(f.doc.blocks().size() == 1);
    const BlockDef& def = f.doc.blocks().begin()->second;
    CHECK(def.name == "Door");
    CHECK(def.entities.size() == 2);
    CHECK(def.base == Vec2{10, 10});
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->block == def.id);
    CHECK(ins->position == Vec2{10, 10});
    CHECK(f.host->undoStack().undoLabel() == "Block Door");

    CHECK(f.host->undo());
    CHECK(f.doc.blocks().empty());
    CHECK(f.modelSpace().size() == 2);
    CHECK(f.host->redo());
    CHECK(f.doc.blocks().size() == 1);
}

TEST_CASE("BLOCK can retain or delete the originals") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 1, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Keep", "0,0", "R"});
    CHECK(f.modelSpace().size() == 1);
    CHECK(f.doc.blockByName("Keep")->entities.size() == 1);

    f.host->setSelection({a});
    f.host->run("BLOCK", {"Gone", "0,0", "D"});
    CHECK(f.modelSpace().empty());
}

TEST_CASE("BLOCK refuses bad and duplicate names") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 1, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Door", "0,0", "R"});
    f.host->clearLog();
    // A duplicate name that is not redefined, then a bad name, then a good one.
    f.host->setSelection({a});
    auto r = f.host->run("BLOCK", {"door", "N", "a/b", "Window", "0,0", "R"});
    CHECK(r.finished);
    CHECK(f.doc.blocks().size() == 2);
    CHECK(f.doc.blockByName("Window") != nullptr);
    CHECK(f.host->logContains("cannot contain"));
}

TEST_CASE("BLOCK redefines an existing block and rejects a cycle") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 1, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"A", "0,0", "C"});
    BlockId blockA = f.doc.blockByName("A")->id;
    EntityId insA = f.modelSpace()[0];

    // Redefine A from the insert of A itself: A would contain A.
    f.host->clearLog();
    f.host->setSelection({insA});
    auto r = f.host->run("BLOCK", {"A", "Y", "0,0"});
    CHECK(r.finished);
    CHECK(f.host->logContains("cannot contain itself"));
    CHECK(f.doc.block(blockA)->entities.size() == 1);
    CHECK(entityAs<LineData>(*f.doc.find(f.doc.block(blockA)->entities[0])));

    // Redefine A with new geometry: every insert of A follows.
    EntityId c = f.addCircle(0, 0, 5);
    f.host->setSelection({c});
    f.host->run("BLOCK", {"A", "Y", "0,0", "R"});
    REQUIRE(f.doc.block(blockA)->entities.size() == 1);
    CHECK(entityAs<CircleData>(*f.doc.find(f.doc.block(blockA)->entities[0])));
}

TEST_CASE("Esc leaves the drawing alone") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 1, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Door", cmd::ScriptItem::esc()});
    CHECK(f.doc.blocks().empty());
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("block list rows are sorted, filtered and count their uses") {
    Fixture f;
    EntityId a = f.addLine(0, 0, 1, 0);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"window", "0,0", "C"});
    f.host->setSelection({f.addCircle(0, 0, 1)});
    f.host->run("BLOCK", {"Door", "0,0", "C"});
    f.host->run("INSERT", {"Door", "50,0"});
    auto rows = blockRows(f.doc);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "Door");
    CHECK(rows[0].references == 2);
    CHECK(rows[1].name == "window");
    CHECK(blockRows(f.doc, "WIN").size() == 1);
    CHECK(blockRows(f.doc, "zzz").empty());
}
