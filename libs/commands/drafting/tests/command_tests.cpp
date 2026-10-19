#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
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

TEST_CASE("the commands and their aliases are registered - toggles and cycle are hidden") {
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

TEST_CASE("SNAP Aspect, Base and Type, and POLAR Additional and Relative") {
    Fixture f;
    f.host.run("SNAP", {"aspect", "5", "2"});
    auto s = drafting::DraftingSettings::read(f.host);
    CHECK(s.snapSpacing == 5);
    CHECK(s.snapSpacingY == 2);
    CHECK(s.snap);
    f.host.run("SNAP", {"base", "3,4"});
    s = drafting::DraftingSettings::read(f.host);
    CHECK(s.snapBase.x == doctest::Approx(3));
    CHECK(s.snapBase.y == doctest::Approx(4));
    f.host.run("SNAP", {"type", "polar", "7"});
    s = drafting::DraftingSettings::read(f.host);
    CHECK(s.polarSnap);
    CHECK(s.polarDistance == 7);
    f.host.run("SNAP", {"type", "grid"});
    CHECK_FALSE(drafting::DraftingSettings::read(f.host).polarSnap);
    // plain Spacing sets both directions again
    f.host.run("SNAP", {"spacing", "4"});
    s = drafting::DraftingSettings::read(f.host);
    CHECK(s.snapSpacing == 4);
    CHECK(s.snapSpacingY == 4);
    // grid spacing 0 follows the snap spacing
    f.host.run("GRID", {"spacing", "0"});
    CHECK(drafting::DraftingSettings::read(f.host).gridSpacing == 4);

    f.host.run("POLAR", {"additional", "17,127.5"});
    s = drafting::DraftingSettings::read(f.host);
    CHECK(s.polar);
    REQUIRE(s.polarAdditionalDeg.size() == 2);
    CHECK(s.polarAdditionalDeg[1] == 127.5);
    f.host.run("POLAR", {"additional", "nonsense"});
    CHECK(f.get("drafting.polar.additional") == "17,127.5");  // refused, kept
    f.host.run("POLAR", {"additional", ""});
    CHECK(drafting::DraftingSettings::read(f.host).polarAdditionalDeg.empty());
    f.host.run("POLAR", {"relative", "relative"});
    CHECK(drafting::DraftingSettings::read(f.host).polarRelative);
    f.host.run("POLAR", {"relative", "absolute"});
    CHECK_FALSE(drafting::DraftingSettings::read(f.host).polarRelative);
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

TEST_CASE("UCS: origin, rotate, points, save, restore, delete and list") {
    Fixture f;
    CHECK(f.reg.find("UCS") != nullptr);
    f.host.run("UCS", {"Origin", "10,20"});
    CHECK(f.get("drafting.ucs") == "@10,20,0");
    f.host.run("UCS", {"Rotate", "90"});
    auto u = drafting::currentUcs(&f.doc, &f.host);
    CHECK(u.origin == Vec2{10, 20});
    CHECK(u.angle == doctest::Approx(kPi / 2));
    // Typed points follow it.
    f.host.run("UCS", {"Save", "Wing"});
    CHECK(f.get("drafting.ucs") == "Wing");
    CHECK(drafting::ucsList(f.doc).size() == 1);
    f.host.run("UCS", {"World"});
    CHECK(f.get("drafting.ucs") == "World");
    CHECK(drafting::currentUcs(&f.doc, &f.host).isWorld());
    f.host.run("UCS", {"Restore", "wing"});
    CHECK(f.get("drafting.ucs") == "Wing");
    f.host.run("UCS", {"List"});
    CHECK(f.host.logContains("* Wing"));
    f.host.run("UCS", {"Points", "0,0", "0,10"});
    CHECK(drafting::currentUcs(&f.doc, &f.host).angle == doctest::Approx(kPi / 2));
    f.host.run("UCS", {"Restore", "Nope"});
    CHECK(f.host.logContains("No UCS named"));
    f.host.run("UCS", {"Delete", "Wing"});
    CHECK(drafting::ucsList(f.doc).empty());
}

TEST_CASE("UCS Object aligns to a line and DYNUCS toggles") {
    Fixture f;
    EntityId lineId;
    {
        auto tx = f.doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {10, 10}};
        auto id = tx.add(e);
        REQUIRE(id.ok());
        lineId = id.value();
        (void)tx.commit();
    }
    f.host.run("UCS", {"Object", ScriptItem::pick(lineId, {5, 5})});
    auto u = drafting::currentUcs(&f.doc, &f.host);
    CHECK(u.angle == doctest::Approx(kPi / 4));
    CHECK(u.origin == Vec2{0, 0});
    f.host.run("DYNUCS", {});
    CHECK(f.get("drafting.dynucs") == "1");
    f.host.run("DYNUCS", {});
    CHECK(f.get("drafting.dynucs") == "0");
}

TEST_CASE("ISODRAFT and ISOPLANE set the isometric plane, Enter goes round Left, Top, Right") {
    Fixture f;
    CHECK(f.reg.find("ISOPLANE") != nullptr);
    f.host.run("ISODRAFT", {"ON"});
    CHECK(f.get("drafting.isodraft") == "1");
    CHECK(drafting::DraftingSettings::read(f.host).isoPlane == drafting::IsoPlane::Top);
    f.host.run("ISOPLANE", {""});  // next after Top
    CHECK(f.get("drafting.isoplane") == "Right");
    f.host.run("ISOPLANE", {""});
    CHECK(f.get("drafting.isoplane") == "Left");
    f.host.run("ISOPLANE", {"top"});
    CHECK(drafting::DraftingSettings::read(f.host).isoPlane == drafting::IsoPlane::Top);
    f.host.run("ISODRAFT", {"OFF"});
    CHECK(drafting::DraftingSettings::read(f.host).isoPlane == drafting::IsoPlane::Off);
}
