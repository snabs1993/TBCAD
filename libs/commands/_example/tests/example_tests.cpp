#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"

using namespace tbcad;

TEST_CASE("the generated registration picks up the _example module") {
    cmd::Registry reg;
    registerAllCommands(reg);
    REQUIRE(reg.find("EXAMPLELINE") != nullptr);
    CHECK(reg.find("exl") == reg.find("EXAMPLELINE"));
    CHECK(reg.find("UNDO") != nullptr);  // framework built-ins come with it
}

TEST_CASE("EXAMPLELINE draws a line, Esc leaves the drawing alone, undo/redo work") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    auto esc = host.run("EXAMPLELINE", {"0,0", cmd::ScriptItem::esc()});
    CHECK(esc.finished);
    CHECK(doc.entityCount() == 0);
    CHECK_FALSE(host.undoStack().canUndo());

    auto ok = host.run("EXL", {"0,0", "@100,0"});
    CHECK(ok.finished);
    REQUIRE(doc.entityCount() == 1);
    CHECK(host.undoStack().undoLabel() == "EXAMPLELINE");

    CHECK(host.undo());
    CHECK(doc.entityCount() == 0);
    CHECK(host.redo());
    CHECK(doc.entityCount() == 1);
}
