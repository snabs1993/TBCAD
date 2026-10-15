#include <doctest/doctest.h>

#include "../src/dim_internal.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    Fixture() {
        registerBuiltinCommands(reg);
        dimcmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
};

}  // namespace

TEST_CASE("DIMSTYLE New copies the current style, makes it current, and Edit changes it") {
    Fixture f;
    auto r = f.host->run("DIMSTYLE", {"New", "Arch", "Edit", "Arch", "TextHeight", "3.5",
                                      "ArrowType", "Tick", "Prefix", "~_", "Done", "Done"});
    CHECK(r.finished);
    const DimStyle* arch = f.doc.dimStyleByName("Arch");
    REQUIRE(arch);
    CHECK(f.doc.header().currentDimStyle == arch->id);
    CHECK(arch->textHeight == doctest::Approx(3.5));
    CHECK(arch->arrowType == ArrowType::ArchTick);
    CHECK(arch->prefix == "~ ");
    // The standard style is untouched.
    CHECK(f.doc.dimStyle(kDimStyleStandard)->textHeight == doctest::Approx(2.5));
    // The whole session is one undo step.
    CHECK(f.host->undo());
    CHECK(f.doc.dimStyleByName("Arch") == nullptr);
}

TEST_CASE("DIMSTYLE Current, Delete and bad input") {
    Fixture f;
    f.host->run("D", {"New", "A", "Done"});
    f.host->run("D", {"New", "B", "Done"});
    REQUIRE(f.doc.dimStyleByName("B"));
    CHECK(f.doc.header().currentDimStyle == f.doc.dimStyleByName("B")->id);
    f.host->run("D", {"Delete", "B", "Done"});  // current: refused
    CHECK(f.doc.dimStyleByName("B"));
    CHECK(f.host->logContains("cannot be deleted"));
    f.host->run("D", {"Current", "Standard", "Delete", "B", "Done"});
    CHECK(!f.doc.dimStyleByName("B"));
    CHECK(f.doc.header().currentDimStyle == kDimStyleStandard);

    f.host->run("D", {"Edit", "A", "TextHeight", "abc", "Done", "Done"});
    CHECK(f.host->logContains("Not a number"));
    CHECK(f.doc.dimStyleByName("A")->textHeight == doctest::Approx(2.5));
    f.host->run("D", {"New", "Standard", "Done"});
    CHECK(f.host->logContains("already used"));
    f.host->run("D", {"Edit", "Nope", "Done"});
    CHECK(f.host->logContains("No dimension style"));
}

TEST_CASE("every DIMSTYLE property accepts a valid value") {
    Document doc;
    DimStyle s;
    auto ok = [&](const char* k, const char* v) { return dimcmd::applyProperty(doc, s, k, v); };
    CHECK(ok("TextGap", "1").empty());
    CHECK(ok("ExtOffset", "0.5").empty());
    CHECK(ok("ExtExtension", "2").empty());
    CHECK(ok("LineExtension", "1").empty());
    CHECK(ok("RoundOff", "5").empty());
    CHECK(ok("Scale", "100").empty());
    CHECK(s.overallScale == doctest::Approx(100));
    CHECK(ok("Precision", "3").empty());
    CHECK(s.precision == 3);
    CHECK(ok("Placement", "Centered").empty());
    CHECK(s.textPlacement == DimTextPlacement::Centered);
    CHECK(ok("Alignment", "Horizontal").empty());
    CHECK(s.textAlign == DimTextAlign::Horizontal);
    CHECK(ok("Units", "in").empty());
    CHECK(s.units == LengthUnit::Inch);
    CHECK(ok("LeadingZeros", "Suppress").empty());
    CHECK(s.suppressLeadingZeros);
    CHECK(ok("TrailingZeros", "off").empty());
    CHECK(!s.suppressTrailingZeros);
    CHECK(ok("LineColor", "1").empty());
    CHECK(s.dimLineColor == Color::index(1));
    CHECK(ok("ExtColor", "10,20,30").empty());
    CHECK(s.extLineColor == Color::rgb(10, 20, 30));
    CHECK(ok("TextColor", "ByBlock").empty());
    CHECK(ok("TextStyle", "Current").empty());
    CHECK(!ok("TextStyle", "Nope").empty());
    CHECK(!ok("Precision", "12").empty());
    CHECK(!ok("ArrowType", "Round").empty());
    CHECK(!ok("Bogus", "1").empty());
    CHECK(!ok("TextHeight", "0").empty());
    CHECK(!ok("LineColor", "300").empty());
    for (const std::string& k : dimcmd::propertyKeywords())
        if (k != "Prefix" && k != "Suffix")  // free text
            CHECK(!ok(k.c_str(), "@@@").empty());
}

TEST_CASE("DIMSTYLE Rename keeps the id, refuses Standard and duplicates") {
    Fixture f;
    f.host->run("D", {"New", "A", "New", "B", "Done"});
    const auto id = f.doc.dimStyleByName("A")->id;
    f.host->run("D", {"Rename", "A", "C", "Done"});
    CHECK(f.doc.dimStyleByName("A") == nullptr);
    REQUIRE(f.doc.dimStyleByName("C"));
    CHECK(f.doc.dimStyleByName("C")->id == id);
    f.host->run("D", {"Rename", "C", "B", "Done"});
    CHECK(f.host->logContains("already used"));
    f.host->run("D", {"Rename", "Standard", "Z", "Done"});
    CHECK(f.doc.dimStyleByName("Standard"));
    CHECK(f.host->undo());
}

TEST_CASE("DIMSETUP gives only the selected dimensions a style or an overall scale") {
    Fixture f;
    f.host->run("DIMLINEAR", {"0,0", "100,0", "50,20"});
    f.host->run("DIMLINEAR", {"0,50", "100,50", "50,70"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            ids.push_back(e.common.id);
    });
    REQUIRE(ids.size() == 2);
    auto styleOf = [&](EntityId id) { return entityAs<DimensionData>(*f.doc.find(id))->style; };
    CHECK(
        f.host->run("DIMSETUP", {ScriptItem::select({ids[0]}), ScriptItem::enter(), "Scale", "50"})
            .finished);
    const DimStyle* scaled = f.doc.dimStyleByName("Standard-1to50");
    REQUIRE(scaled);
    CHECK(scaled->overallScale == doctest::Approx(50));
    CHECK(styleOf(ids[0]) == scaled->id);
    CHECK(styleOf(ids[1]) != scaled->id);  // untouched
    CHECK(f.doc.dimStyle(kDimStyleStandard)->overallScale == doctest::Approx(1));
    // Both together: the second joins the same copy, no duplicate is made.
    f.host->run("DIMSETUP", {ScriptItem::select(ids), ScriptItem::enter(), "Scale", "50"});
    CHECK(styleOf(ids[1]) == scaled->id);
    CHECK(f.doc.dimStyles().size() == 2);
    // Back to a named style.
    f.host->run("DIMSETUP", {ScriptItem::select(ids), ScriptItem::enter(), "Style", "Standard"});
    CHECK(styleOf(ids[0]) == kDimStyleStandard);
    CHECK(f.host->undo());
}
