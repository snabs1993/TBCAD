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
    auto r = f.host.run("ST", {"Narrow", "Sarabun", "0.8", "15", "5"});
    CHECK(r.finished);
    const TextStyle* s = f.doc.textStyleByName("Narrow");
    REQUIRE(s);
    CHECK(s->widthFactor == doctest::Approx(0.8));
    CHECK(s->oblique == doctest::Approx(15 * kPi / 180));
    CHECK(s->height == doctest::Approx(5));
    CHECK(s->fontFamily.empty());  // the bundled font needs no family
    CHECK(f.doc.header().currentTextStyle == s->id);
    CHECK(f.host.undoStack().undoLabel() == "STYLE");
    f.host.undo();
    CHECK(f.doc.textStyleByName("Narrow") == nullptr);
    CHECK(f.doc.header().currentTextStyle == kTextStyleStandard);
}

TEST_CASE("STYLE changes an existing style - font files and families are stored") {
    Fixture f;
    f.host.run("STYLE", {"Standard", "Some Family", "1", "0", "0"});
    const TextStyle* s = f.doc.textStyleByName("Standard");
    REQUIRE(s);
    CHECK(s->fontFamily == "Some Family");
    CHECK(f.host.logContains("Font not found"));  // reported, still stored

    f.host.run("STYLE", {"Standard", "D:\\fonts\\My.ttf", "1", "0", "0"});
    s = f.doc.textStyleByName("Standard");
    CHECK(s->fontFile == "D:\\fonts\\My.ttf");
    CHECK(s->fontFamily.empty());
}

TEST_CASE("STYLE ? lists the styles - Esc changes nothing") {
    Fixture f;
    f.host.run("STYLE", {"?"});
    CHECK(f.host.logContains("Standard"));
    f.host.run("STYLE", {"X", cmd::ScriptItem::esc()});
    CHECK(f.doc.textStyleByName("X") == nullptr);
}

TEST_CASE("text style drives the layout: width factor changes the text width") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "10", "0", "TBCAD"});
    f.host.run("STYLE", {"Half", "Sarabun", "0.5", "0", "0"});
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

TEST_CASE("a style with a fixed height sets the height of TEXT and MTEXT - 0 keeps it variable") {
    Fixture f;
    f.host.run("ST", {"Fixed", "Sarabun", "1", "0", "7"});
    // TEXT asks no height: start point, rotation, then the text.
    f.host.run("TEXT", {"0,0", "0", "label"});
    const TextData* t = nullptr;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<TextData>(e))
            t = d;
    });
    REQUIRE(t);
    CHECK(t->height == doctest::Approx(7));
    // MTEXT takes it too, unless the Height option is used.
    f.host.run("MTEXT", {"0,50", "40,20", "boxed"});
    f.host.run("MTEXT", {"H", "3", "0,90", "40,60", "chosen"});
    std::vector<double> heights;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* m = entityAs<MTextData>(e))
            heights.push_back(m->height);
    });
    REQUIRE(heights.size() == 2);
    CHECK(heights[0] == doctest::Approx(7));
    CHECK(heights[1] == doctest::Approx(3));
    // Back to variable: the height question returns.
    f.host.run("ST", {"Fixed", "Sarabun", "1", "0", "0"});
    CHECK(f.doc.textStyleByName("Fixed")->height == 0);
    f.host.run("TEXT", {"0,200", "4", "0", "free"});
    double last = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<TextData>(e))
            last = d->height;
    });
    CHECK(last == doctest::Approx(4));
}

TEST_CASE("-STYLE Edit sets the fixed height") {
    Fixture f;
    f.host.run("-STYLE", {"Edit", "Standard", "Height", "3.5", "Done", "Done"});
    CHECK(f.doc.textStyle(kTextStyleStandard)->height == doctest::Approx(3.5));
    f.host.run("-STYLE", {"Edit", "Standard", "Height", "-1", "Done", "Done"});
    CHECK(f.doc.textStyle(kTextStyleStandard)->height == doctest::Approx(3.5));
}

TEST_CASE("a shape font that is not installed is reported by FONTCHECK and stands in") {
    Fixture f;
    f.host.run("STYLE", {"Plan", "romans.shx", "1", "0", "0"});
    const TextStyle* s = f.doc.textStyleByName("Plan");
    REQUIRE(s);
    CHECK(s->fontFile == "romans.shx");
    f.host.run("FONTCHECK", {});
    CHECK(f.host.logContains("Style Plan"));
    CHECK(f.host.logContains("romans.shx not found"));
    // Choosing a normal font again forgets the shape font.
    f.host.run("STYLE", {"Plan", "Sarabun", "1", "0", "0"});
    CHECK(f.doc.textStyleByName("Plan")->fontFile.empty());
    f.host.run("FONTCHECK", {});
    CHECK(f.host.logContains("All fonts used"));
}

TEST_CASE("text in an annotative style is annotative") {
    Fixture f;
    f.host.run("ST", {"Note", "Sarabun", "1", "0", "0"});
    {
        auto tx = f.doc.begin("anno");
        REQUIRE(tx.modifyTextStyle(f.doc.textStyleByName("Note")->id, [](TextStyle& t) {
                      t.annotative = true;
                  }).ok());
        tx.commit();
    }
    f.host.run("TEXT", {"0,0", "2.5", "0", "A"});
    int anno = 0, plain = 0;
    f.doc.forEachEntity([&](const Entity& e) { (e.common.annotative ? anno : plain)++; });
    CHECK(anno == 1);
    CHECK(plain == 0);
}
