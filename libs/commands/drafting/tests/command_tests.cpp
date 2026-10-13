#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/draftingcmds/drafting_cmds.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, reg) {
        cmd::registerBuiltinCommands(reg);
        draftingcmds::registerCommands(reg);
    }
    std::string get(const char* key) { return host.setting(key).value_or("<unset>"); }
};
}  // namespace

TEST_CASE("the commands and their aliases are registered; toggles and cycle are hidden") {
    Fixture f;
    for (const char* n : {"OSNAP", "GRID", "SNAP", "ORTHO", "POLAR", "TOGGLEOSNAP", "TOGGLEGRID",
                          "TOGGLESNAP", "TOGGLEORTHO", "TOGGLEPOLAR", "SNAPCYCLE"})
        CHECK(f.reg.find(n) != nullptr);
    CHECK(f.reg.find("os") == f.reg.find("OSNAP"));
    CHECK(f.reg.find("TOGGLEORTHO")->description().empty());
    CHECK_FALSE(f.reg.find("ORTHO")->description().empty());
    CHECK(f.reg.find("ORTHO")->isTransparent());
}

TEST_CASE("ORTHO toggles with Enter and takes ON / OFF") {
    Fixture f;
    CHECK(f.host.run("ORTHO", {""}).finished);
    CHECK(f.get("drafting.ortho") == "1");
    CHECK(f.host.logContains("Ortho on"));
    f.host.run("ORTHO", {""});
    CHECK(f.get("drafting.ortho") == "0");
    f.host.run("ORTHO", {"on"});
    CHECK(f.get("drafting.ortho") == "1");
    f.host.run("ORTHO", {"off"});
    CHECK(f.get("drafting.ortho") == "0");
    f.host.run("ORTHO", {ScriptItem::esc()});
    CHECK(f.get("drafting.ortho") == "0");
}

TEST_CASE("ORTHO and POLAR switch each other off") {
    Fixture f;
    f.host.run("POLAR", {"on"});
    CHECK(f.get("drafting.polar") == "1");
    f.host.run("ORTHO", {"on"});
    CHECK(f.get("drafting.ortho") == "1");
    CHECK(f.get("drafting.polar") == "0");
    f.host.run("TOGGLEPOLAR", {});
    CHECK(f.get("drafting.polar") == "1");
    CHECK(f.get("drafting.ortho") == "0");
}

TEST_CASE("POLAR Increment stores the list and rejects nonsense") {
    Fixture f;
    f.host.run("POLAR", {"increment", "30,45"});
    CHECK(f.get("drafting.polar.increment") == "30,45");
    CHECK(f.get("drafting.polar") == "1");
    f.host.run("POLAR", {"increment", "banana"});
    CHECK(f.get("drafting.polar.increment") == "30,45");
    CHECK(f.host.logContains("Invalid increment"));
}

TEST_CASE("OSNAP Modes and Aperture") {
    Fixture f;
    f.host.run("OSNAP", {"modes", "endpoint, midpoint"});
    CHECK(f.get("drafting.osnap.modes") == "endpoint,midpoint");
    f.host.run("OSNAP", {"modes", "all"});
    CHECK(*drafting::parseSnapModes(f.get("drafting.osnap.modes")) == drafting::kAllSnapModes);
    f.host.run("OSNAP", {"modes", "bogus"});
    CHECK(f.host.logContains("Unknown snap mode"));
    CHECK(*drafting::parseSnapModes(f.get("drafting.osnap.modes")) == drafting::kAllSnapModes);
    f.host.run("OSNAP", {"aperture", "15"});
    CHECK(f.get("drafting.osnap.aperture") == "15");
    auto r = f.host.run("OSNAP", {"aperture", "500"});  // out of range, still waiting
    CHECK(r.rejected == 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.get("drafting.osnap.aperture") == "15");
}

TEST_CASE("GRID and SNAP spacing and style") {
    Fixture f;
    f.host.run("GRID", {"spacing", "2.5"});
    CHECK(f.get("drafting.grid.spacing") == "2.5");
    CHECK(f.get("view.grid") == "1");
    f.host.run("GRID", {"style", "lines"});
    CHECK(f.get("drafting.grid.style") == "lines");
    f.host.run("GRID", {"off"});
    CHECK(f.get("view.grid") == "0");
    f.host.run("SNAP", {"spacing", "5"});
    CHECK(f.get("drafting.snap.spacing") == "5");
    CHECK(f.get("drafting.snap") == "1");
    auto s = drafting::DraftingSettings::read(f.host);
    CHECK(s.gridSpacing == 2.5);
    CHECK(s.gridLines);
    CHECK(s.snapSpacing == 5);
}

TEST_CASE("the F-key toggles flip their setting") {
    Fixture f;
    f.host.run("TOGGLEOSNAP", {});
    CHECK(f.get("drafting.osnap") == "0");  // unset counts as off for the command
    f.host.run("TOGGLEOSNAP", {});
    CHECK(f.get("drafting.osnap") == "1");
    f.host.run("TOGGLEGRID", {});
    CHECK(f.get("view.grid") == "1");
    f.host.run("TOGGLESNAP", {});
    CHECK(f.get("drafting.snap") == "1");
    f.host.run("SNAPCYCLE", {});  // nothing to cycle headless: must not crash
}
