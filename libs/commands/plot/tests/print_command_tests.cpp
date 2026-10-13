#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/plot/plot.h"
#include "tbcad/print/print.h"

using namespace tbcad;

TEST_CASE("PRINTER is registered and explains itself when there is no printing backend") {
    Document doc;
    cmd::Registry reg;
    plot::registerCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(reg.find("PRINTER") != nullptr);
    CHECK(reg.find("PRN") == reg.find("PRINTER"));
    CHECK(reg.find("PRINT") == reg.find("PLOT"));  // PRINT still opens the plot command
#ifndef _WIN32
    // The command never opens a dialog here, so it is safe to run it.
    auto r = host.run("PRINTER", {});
    CHECK(r.finished);
    CHECK(host.logContains("only available in the Windows version"));
#endif
}
