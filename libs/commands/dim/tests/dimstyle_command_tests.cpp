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
    CHECK(ok("AltUnits", "On").empty());
    CHECK(s.altUnits);
    CHECK(ok("AltUnit", "ft").empty());
    CHECK(s.altUnit == LengthUnit::Foot);
    CHECK(ok("AltScale", "2").empty());
    CHECK(ok("AltPrecision", "4").empty());
    CHECK(s.altPrecision == 4);
    CHECK(ok("AltSuffix", "ft").empty());
    CHECK(s.altSuffix == "ft");
    CHECK(ok("Tolerance", "Limits").empty());
    CHECK(s.tolerance == DimTolerance::Limits);
    CHECK(ok("TolUpper", "0.1").empty());
    CHECK(ok("TolLower", "0.2").empty());
    CHECK(ok("TolPrecision", "3").empty());
    CHECK(!ok("AltScale", "0").empty());
    CHECK(!ok("TolUpper", "-1").empty());
    CHECK(!ok("Tolerance", "Wide").empty());
    CHECK(!ok("AltUnit", "yd").empty());
    CHECK(ok("AlignmentOutside", "Horizontal").empty());
    CHECK(s.textAlignOutside == DimTextAlign::Horizontal);
    CHECK(ok("TextInside", "Forced").empty());
    CHECK(s.textInsideForced);
    CHECK(ok("LineInside", "Auto").empty());
    CHECK_FALSE(s.forceLineInside);
    CHECK(ok("DimLine2", "Hide").empty());
    CHECK(s.suppressDim2);
    CHECK(ok("Justify", "Ext2").empty());
    CHECK(s.textJustify == 2);
    CHECK(ok("AngleFormat", "Dms").empty());
    CHECK(s.angleFormat == DimAngleFormat::Dms);
    CHECK(ok("AngularPrecision", "4").empty());
    CHECK(s.angularPrecision == 4);
    CHECK(ok("AngularPrecision", "Same").empty());
    CHECK(s.angularPrecision == -1);
    CHECK(ok("DecimalMark", "Comma").empty());
    CHECK(s.commaDecimal);
    CHECK(ok("BaselineSpacing", "7").empty());
    CHECK(s.baselineSpacing == doctest::Approx(7));
    CHECK(ok("BaselineSpacing", "Auto").empty());
    CHECK(s.baselineSpacing == 0);
    CHECK(ok("CenterMark", "-3").empty());
    CHECK(s.centerMark == doctest::Approx(-3));
    CHECK(ok("TextFill", "Color").empty());
    CHECK(s.textFill == 2);
    CHECK(ok("TextFillColor", "251").empty());
    CHECK(s.textFillColor == 251);
    CHECK(ok("TextFrame", "Yes").empty());
    CHECK(s.textFrame);
    for (const std::string& k : dimcmd::propertyKeywords())
        if (k != "Prefix" && k != "Suffix" && k != "AltPrefix" && k != "AltSuffix")  // free text
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

TEST_CASE("the first dimension seen from far away gets a scaled style so its text can be read") {
    Fixture f;
    f.host->setView({{0, 0}, 10.0, {900, 600}});  // 10 mm per pixel: a plan in millimetres
    f.host->run("DIMLINEAR", {"0,0", "6000,0", "3000,500"});
    f.host->run("DIMLINEAR", {"0,1000", "6000,1000", "3000,1500"});
    const DimStyle* scaled = f.doc.dimStyleByName("Standard-1to50");
    REQUIRE(scaled);  // 12 px * 10 mm / 2.5 mm = 48, nearest 1-2-5 step is 50
    CHECK(scaled->overallScale == doctest::Approx(50));
    CHECK(f.doc.header().currentDimStyle == scaled->id);
    CHECK(f.doc.dimStyles().size() == 2);  // the second dimension reuses it
    // A normal zoom leaves the Standard style alone.
    Fixture g;
    g.host->run("DIMLINEAR", {"0,0", "100,0", "50,20"});
    CHECK(g.doc.dimStyles().size() == 1);
}

TEST_CASE("without a view a big drawing still gets readable dimension text") {
    Fixture f;  // a script: no view, but a 20 m plan
    {
        auto tx = f.doc.begin("plan");
        Entity a, b;
        a.data = LineData{{0, 0}, {20000, 0}};
        b.data = LineData{{0, 0}, {0, 12000}};
        REQUIRE(tx.add(std::move(a)));
        REQUIRE(tx.add(std::move(b)));
        tx.commit();
    }
    f.host->run("DIMLINEAR", {"0,0", "20000,0", "10000,-800"});
    const DimStyle* scaled = f.doc.dimStyleByName("Standard-1to50");
    REQUIRE(scaled);  // 20000 / 160 / 2.5 = 50
    CHECK(f.doc.header().currentDimStyle == scaled->id);
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

TEST_CASE("MLEADERALIGN lines landing ends up in a column or a row") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    f.host->run("MLEADER", {"0,20", "30,25", "b"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 2);
    f.host->run("MLEADERALIGN", {ScriptItem::select(ids), ScriptItem::enter(),
                                 ScriptItem::pick(ids[0], {5, 5}), "Vertical"});
    const auto* a = entityAs<MLeaderData>(*f.doc.find(ids[0]));
    const auto* b = entityAs<MLeaderData>(*f.doc.find(ids[1]));
    CHECK(b->landing.x == doctest::Approx(a->landing.x));
    CHECK(b->lines[0].back().x == doctest::Approx(a->landing.x));
    CHECK(b->contentPos.x > b->landing.x);
    CHECK(b->lines[0].front().x == doctest::Approx(0));  // the arrow stays put
    CHECK(f.host->undo());
    CHECK(entityAs<MLeaderData>(*f.doc.find(ids[1]))->landing.x == doctest::Approx(30));
}

TEST_CASE("MLEADERALIGN Spacing lines leaders up at an even distance, the reference stays") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    f.host->run("MLEADER", {"0,20", "30,22", "b"});
    f.host->run("MLEADER", {"0,40", "50,47", "c"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 3);
    // Reference: the middle one (landing y 22); others at y 10 and 47.
    f.host->run("MLEADERALIGN", {ScriptItem::select(ids), ScriptItem::enter(),
                                 ScriptItem::pick(ids[1], {5, 21}), "Spacing", "8", "Vertical"});
    const auto* a = entityAs<MLeaderData>(*f.doc.find(ids[0]));
    const auto* b = entityAs<MLeaderData>(*f.doc.find(ids[1]));
    const auto* c = entityAs<MLeaderData>(*f.doc.find(ids[2]));
    CHECK(a->landing.x == doctest::Approx(b->landing.x));
    CHECK(c->landing.x == doctest::Approx(b->landing.x));
    CHECK(b->landing.y == doctest::Approx(22));
    CHECK(a->landing.y == doctest::Approx(14));
    CHECK(c->landing.y == doctest::Approx(30));
    CHECK(f.host->undo());
}

TEST_CASE("MLEADERALIGN Distribute spreads the landings evenly between two points") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    f.host->run("MLEADER", {"0,20", "30,22", "b"});
    f.host->run("MLEADER", {"0,40", "50,47", "c"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 3);
    f.host->run("MLEADERALIGN", {ScriptItem::select(ids), ScriptItem::enter(),
                                 ScriptItem::pick(ids[0], {5, 5}), "Distribute", "60,0", "60,40"});
    for (int i = 0; i < 3; ++i) {
        const auto* m = entityAs<MLeaderData>(*f.doc.find(ids[static_cast<std::size_t>(i)]));
        CHECK(m->landing.x == doctest::Approx(60));
        CHECK(m->landing.y == doctest::Approx(20.0 * i));
        CHECK(m->lines[0].back().y == doctest::Approx(20.0 * i));
    }
    CHECK(f.host->undo());
    CHECK(entityAs<MLeaderData>(*f.doc.find(ids[2]))->landing.x == doctest::Approx(50));
}

TEST_CASE("MLEADERALIGN Parallel turns each leader line parallel to the reference") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});   // 45 degrees
    f.host->run("MLEADER", {"0,20", "30,20", "b"});  // flat, 30 long
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 2);
    f.host->run("MLEADERALIGN", {ScriptItem::select(ids), ScriptItem::enter(),
                                 ScriptItem::pick(ids[0], {5, 5}), "Parallel"});
    const auto* b = entityAs<MLeaderData>(*f.doc.find(ids[1]));
    const Vec2 v = b->landing - b->lines[0].front();
    CHECK(v.x == doctest::Approx(v.y));                   // now at 45 degrees
    CHECK(length(v) == doctest::Approx(30));              // the same length
    CHECK(b->lines[0].front().y == doctest::Approx(20));  // the arrow stays put
    CHECK(b->contentPos.x > b->landing.x);
}

TEST_CASE("MLEADERCOLLECT merges notes into the first multileader") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    f.host->run("MLEADER", {"0,20", "30,25", "b"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 2);
    f.host->run("MLEADERCOLLECT", {ScriptItem::select(ids), ScriptItem::enter()});
    std::size_t n = 0;
    f.doc.forEachEntity([&](const Entity&) { ++n; });
    CHECK(n == 1);
    const auto* m = entityAs<MLeaderData>(*f.doc.find(ids[0]));
    REQUIRE(m);
    CHECK(m->lines.size() == 2);
    CHECK(m->utf8 == "a\\Pb");
    CHECK(m->lines[1].back().x == doctest::Approx(m->landing.x));
    CHECK(m->lines[1].front().y == doctest::Approx(20));  // the arrow stays put
    CHECK(f.host->undo());
    n = 0;
    f.doc.forEachEntity([&](const Entity&) { ++n; });
    CHECK(n == 2);
}

TEST_CASE("MLEADER and LEADER from far away get a scaled style like the first dimension") {
    Fixture f;
    f.host->setView({{0, 0}, 10.0, {900, 600}});
    f.host->run("MLEADER", {"0,0", "Add", "4000,2000", "3000,2000", "beam"});
    const DimStyle* scaled = f.doc.dimStyleByName("Standard-1to50");
    REQUIRE(scaled);
    CHECK(scaled->overallScale == doctest::Approx(50));
    CHECK(f.doc.header().currentDimStyle == scaled->id);
    bool leader = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<MLeaderData>(e)) {
            leader = true;
            CHECK(l->style == scaled->id);
            // The dogleg is one scaled arrow long.
            CHECK(std::abs(l->contentPos.x - l->landing.x) == doctest::Approx(2.5 * 50));
        }
    });
    CHECK(leader);
    // Esc rolls the style back with the rest of the command.
    Fixture g;
    g.host->setView({{0, 0}, 10.0, {900, 600}});
    g.host->run("MLEADER", {"0,0", ScriptItem::esc()});
    CHECK(g.doc.dimStyles().size() == 1);
}

TEST_CASE("MLEADEREDIT adds and removes leader lines") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    f.host->run("MLEADEREDIT", {ScriptItem::pick(id, {5, 5}), "Add", "0,20", ""});
    REQUIRE(entityAs<MLeaderData>(*f.doc.find(id))->lines.size() == 2);
    CHECK(entityAs<MLeaderData>(*f.doc.find(id))->lines[1].front().y == doctest::Approx(20));
    f.host->run("MLEADEREDIT", {ScriptItem::pick(id, {5, 5}), "Remove", "0,19", ""});
    const auto* m = entityAs<MLeaderData>(*f.doc.find(id));
    REQUIRE(m->lines.size() == 1);
    CHECK(m->lines[0].front().y == doctest::Approx(0));
    f.host->run("MLEADEREDIT", {ScriptItem::pick(id, {5, 5}), "Remove"});
    CHECK(f.host->logContains("at least one"));
}

TEST_CASE("AIMLEADEREDITADD and AIMLEADEREDITREMOVE skip the Add/Remove question") {
    Fixture f;
    f.host->run("MLEADER", {"0,0", "10,10", "a"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    f.host->run("AIMLEADEREDITADD", {ScriptItem::pick(id, {5, 5}), "0,20", "0,30", ""});
    CHECK(entityAs<MLeaderData>(*f.doc.find(id))->lines.size() == 3);
    f.host->run("AIMLEADEREDITREMOVE", {ScriptItem::pick(id, {5, 5}), "0,29", ""});
    CHECK(entityAs<MLeaderData>(*f.doc.find(id))->lines.size() == 2);
}

TEST_CASE("DIMSTYLE Compare lists the differences between two styles") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Arch", "Edit", "Arch", "TextHeight", "3.5", "Prefix",
                             "R=", "LineColor", "1", "Done", "Done"});
    f.host->clearLog();
    auto r = f.host->run("DIMSTYLE", {"Compare", "Standard", "Arch", "Done"});
    CHECK(r.finished);
    CHECK(f.host->logContains("TextHeight: 2.5 | 3.5"));
    CHECK(f.host->logContains("Prefix: none | R="));
    CHECK(f.host->logContains("LineColor: ByBlock | 1"));
    CHECK(f.host->logContains("3 difference(s)."));
    CHECK_FALSE(f.host->logContains("ArrowSize"));
    f.host->clearLog();
    f.host->run("DIMSTYLE", {"Compare", "Standard", "Standard", "Done"});
    CHECK(f.host->logContains("The styles are the same."));
    f.host->clearLog();
    f.host->run("DIMSTYLE", {"Compare", "Standard", "Nope", "Done"});
    CHECK(f.host->logContains("No dimension style named \"Nope\"."));
}

TEST_CASE("DIMOVERRIDE changes one property of the selected dimensions only") {
    Fixture f;
    f.host->run("DIMLINEAR", {"0,0", "10,0", "5,5"});
    f.host->run("DIMLINEAR", {"0,20", "10,20", "5,25"});
    f.host->run("DIMLINEAR", {"0,40", "10,40", "5,45"});
    std::vector<EntityId> dims;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dims.push_back(e.common.id);
    });
    REQUIRE(dims.size() == 3);
    const std::size_t styles = f.doc.dimStyles().size();

    f.host->setSelection({dims[0], dims[1]});
    auto r = f.host->run("DIMOVERRIDE", {"Prefix", "R="});
    CHECK(r.finished);
    // Both selected dimensions share one new style; the third keeps Standard.
    CHECK(f.doc.dimStyles().size() == styles + 1);
    const auto style = [&](EntityId id) { return entityAs<DimensionData>(*f.doc.find(id))->style; };
    CHECK(style(dims[0]) == style(dims[1]));
    CHECK(style(dims[0]) != style(dims[2]));
    const DimStyle* over = f.doc.dimStyle(style(dims[0]));
    REQUIRE(over);
    CHECK(over->name == "Standard$1");
    CHECK(over->prefix == "R=");
    CHECK(over->textHeight == doctest::Approx(2.5));
    CHECK(f.host->undoStack().undoLabel() == "Dimension override");

    // A bad value changes nothing.
    f.host->clearLog();
    f.host->setSelection({dims[2]});
    f.host->run("DIMOVERRIDE", {"TextHeight", "-4"});
    CHECK(f.doc.dimStyles().size() == styles + 1);
    CHECK(f.host->logContains("greater than zero"));

    // Same override on the third dimension reuses the style; Clear goes back to Standard.
    f.host->setSelection({dims[2]});
    f.host->run("DIMOVERRIDE", {"Prefix", "R="});
    CHECK(style(dims[2]) == style(dims[0]));
    CHECK(f.doc.dimStyles().size() == styles + 1);
    f.host->setSelection({dims[0], dims[1], dims[2]});
    f.host->run("DIMOVERRIDE", {"Clear"});
    for (EntityId d : dims)
        CHECK(f.doc.dimStyle(style(d))->name == "Standard");

    // Undo walks back one step at a time.
    CHECK(f.host->undo());
    CHECK(f.doc.dimStyle(style(dims[2]))->name == "Standard$1");
}

TEST_CASE("DIMSTYLE child styles - the parent stays current and dimensions of that kind use it") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Arch", "New", "Arch$4", "Edit", "Arch$4", "TextHeight", "5",
                             "Done", "Done"});
    CHECK(f.host->logContains("Child style Arch$4 of Arch added."));
    const DimStyle* arch = f.doc.dimStyleByName("Arch");
    const DimStyle* radius = f.doc.dimStyleByName("Arch$4");
    REQUIRE(arch);
    REQUIRE(radius);
    CHECK(f.doc.header().currentDimStyle == arch->id);
    CHECK(dim::childStyleCode(DimKind::Radius) == 4);
    CHECK(dim::childStyleCode(DimKind::Linear) == 0);
    CHECK(dim::childStyleName("Arch", 4) == "Arch$4");
    DimensionData d;
    d.style = arch->id;
    d.kind = DimKind::Radius;
    CHECK(dim::drawStyleOf(f.doc, d).id == radius->id);
    d.kind = DimKind::Linear;  // no $0 child: the parent
    CHECK(dim::drawStyleOf(f.doc, d).id == arch->id);
    CHECK(dim::leaderStyleOf(f.doc, arch->id).id == arch->id);
    // A child of a style that does not exist is just a style with a $ in its name.
    f.host->run("DIMSTYLE", {"New", "Ghost$0", "Done"});
    CHECK(f.doc.header().currentDimStyle == f.doc.dimStyleByName("Ghost$0")->id);
}

TEST_CASE("DIMSTYLE FixedExt and FixedExtLength") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Fx", "Edit", "Fx", "FixedExt", "On", "FixedExtLength", "4",
                             "Done", "Done"});
    const DimStyle* s = f.doc.dimStyleByName("Fx");
    REQUIRE(s);
    CHECK(s->fixedExtLines);
    CHECK(s->fixedExtLength == doctest::Approx(4));
    bool listed = false;
    for (const auto& [k, v] : dimcmd::describeStyle(f.doc, *s))
        listed |= k == "FixedExt" && v == "On";
    CHECK(listed);
}

TEST_CASE("DIMSTYLE LeaderArrow sets the arrow leaders draw with") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Ld", "Edit", "Ld", "ArrowType", "Tick", "LeaderArrow", "Dot",
                             "Done", "Done"});
    const DimStyle* s = f.doc.dimStyleByName("Ld");
    REQUIRE(s);
    CHECK(s->arrowType == ArrowType::ArchTick);
    CHECK(s->leaderArrow == ArrowType::Dot);
    // A leader of the style draws a dot (an arrow ring), not a tick.
    LeaderData l{{{0, 0}, {10, 10}, {20, 10}}, s->id, "", {}};
    const dim::Layout L = dim::layoutLeader(f.doc, l);
    CHECK(L.arrows.size() == 1);
}

TEST_CASE("DIMSTYLE LineWeight and ExtWeight set DIMLWD and DIMLWE") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Lw", "Edit", "Lw", "LineWeight", "0.35", "ExtWeight",
                             "ByLayer", "Done", "Done"});
    const DimStyle* s = f.doc.dimStyleByName("Lw");
    REQUIRE(s);
    CHECK(s->dimLineWeight == LineWeight::hundredths(35));
    CHECK(s->extLineWeight == LineWeight::byLayer());
    bool listed = false;
    for (const auto& [k, v] : dimcmd::describeStyle(f.doc, *s))
        listed |= k == "LineWeight" && v == "0.35";
    CHECK(listed);
}

TEST_CASE("DIMSTYLE Linetype and ExtLinetype1/2 set DIMLTYPE and DIMLTEX1/2") {
    Fixture f;
    {
        Transaction tx = f.doc.begin("setup");
        REQUIRE(tx.addLinetype({.name = "CENTER", .pattern = {12, -3, 3, -3}}).ok());
        tx.commit();
    }
    f.host->run("DIMSTYLE", {"New", "Lt", "Edit", "Lt", "Linetype", "center", "ExtLinetype1",
                             "ByLayer", "ExtLinetype2", "NOPE", "Done", "Done"});
    const DimStyle* s = f.doc.dimStyleByName("Lt");
    REQUIRE(s);
    CHECK(s->dimLinetype == "CENTER");  // the drawing's spelling
    CHECK(s->ext1Linetype == "ByLayer");
    CHECK(s->ext2Linetype.empty());  // an unknown linetype is refused
    bool listed = false;
    for (const auto& [k, v] : dimcmd::describeStyle(f.doc, *s))
        listed |= k == "ExtLinetype2" && v == "ByBlock";
    CHECK(listed);
}

TEST_CASE("DIMSTYLE AngleZeros, TolZeros and AltZeros set DIMAZIN, DIMTZIN and DIMALTZ") {
    Fixture f;
    f.host->run("DIMSTYLE", {"New", "Z", "Edit", "Z", "TrailingZeros", "Suppress", "AngleZeros",
                             "Show", "TolZeros", "Both", "AltZeros", "Bad", "Done", "Done"});
    const DimStyle* s = f.doc.dimStyleByName("Z");
    REQUIRE(s);
    CHECK(s->angularZeros == 0);
    CHECK(s->tolZeros == 3);
    CHECK(s->altZeros == -1);
    // Lengths drop trailing zeros; angles keep theirs.
    CHECK(dim::formatNumber(*s, 12.5, false) == "12.5");
    CHECK(dim::formatNumber(*s, kPi / 2, true) == "90.00\xC2\xB0");
}
