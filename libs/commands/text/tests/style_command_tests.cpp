#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/text/text.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};
}  // namespace

TEST_CASE("STYLE creates a style and makes it current") {
    Fixture f;
    auto r = f.host.run("ST", {"Narrow", "Sarabun", "0.8", "15"});
    CHECK(r.finished);
    const TextStyle* s = f.doc.textStyleByName("Narrow");
    REQUIRE(s);
    CHECK(s->widthFactor == doctest::Approx(0.8));
    CHECK(s->oblique == doctest::Approx(15 * kPi / 180));
    CHECK(s->fontFamily.empty());  // the bundled font needs no family
    CHECK(f.doc.header().currentTextStyle == s->id);
    CHECK(f.host.undoStack().undoLabel() == "STYLE");
    f.host.undo();
    CHECK(f.doc.textStyleByName("Narrow") == nullptr);
    CHECK(f.doc.header().currentTextStyle == kTextStyleStandard);
}

TEST_CASE("STYLE changes an existing style; font files and families are stored") {
    Fixture f;
    f.host.run("STYLE", {"Standard", "Some Family", "1", "0"});
    const TextStyle* s = f.doc.textStyleByName("Standard");
    REQUIRE(s);
    CHECK(s->fontFamily == "Some Family");
    CHECK(f.host.logContains("Font not found"));  // reported, still stored

    f.host.run("STYLE", {"Standard", "D:\\fonts\\My.ttf", "1", "0"});
    s = f.doc.textStyleByName("Standard");
    CHECK(s->fontFile == "D:\\fonts\\My.ttf");
    CHECK(s->fontFamily.empty());
}

TEST_CASE("STYLE ? lists the styles; Esc changes nothing") {
    Fixture f;
    f.host.run("STYLE", {"?"});
    CHECK(f.host.logContains("Standard"));
    f.host.run("STYLE", {"X", cmd::ScriptItem::esc()});
    CHECK(f.doc.textStyleByName("X") == nullptr);
}

TEST_CASE("text style drives the layout: width factor changes the text width") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "10", "0", "TBCAD"});
    f.host.run("STYLE", {"Half", "Sarabun", "0.5", "0"});
    f.host.run("TEXT", {"0,0", "10", "0", "TBCAD"});
    std::vector<double> widths;
    for (EntityId id : f.doc.modelSpace()) {
        text::TextLayout l = text::layoutEntity(f.doc, *f.doc.find(id));
        widths.push_back(l.toWorld.apply({l.lines[0].width, 0}).x);
    }
    REQUIRE(widths.size() == 2);
    // Text without its own style follows the current style, so both now use "Half".
    double full = text::measureWidth(f.doc.textStyle(kTextStyleStandard), "TBCAD", 10);
    CHECK(widths[0] == doctest::Approx(full / 2));
    CHECK(widths[1] == doctest::Approx(full / 2));
}
