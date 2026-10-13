// The application's file round trip under ScriptHost: no window, no AppCore, only the command
// framework and the .tbcad library, which is what the window code builds on.
#include <doctest/doctest.h>

#include "test_support.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using namespace tbcad::ui::test;

TEST_CASE("script a command, save, reopen: same canonical dump") {
    auto registry = makeRegistry();
    Document doc;
    cmd::ScriptHost host(doc, *registry);
    auto r = host.run("TESTLINE", {"0,0", "100,50"});
    REQUIRE(r.finished);
    host.run("TESTLINE", {"10,10", "20,30"});
    REQUIRE(doc.modelSpaceCount() == 2);

    auto dir = tempDir("scripthost");
    auto file = dir / "a.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());

    io::IoReport report;
    auto back = io::readTbcad(file, report);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    // Open, run a command on the reopened document, undo it.
    cmd::ScriptHost host2(*back.value(), *registry);
    host2.run("TESTLINE", {"1,1", "2,2"});
    CHECK(back.value()->modelSpaceCount() == 3);
    CHECK(host2.undo());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));
}
