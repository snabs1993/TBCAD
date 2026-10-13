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

TEST_CASE("-STYLE New copies the current style, makes it current; one undo step") {
    Fixture f;
    f.host.run("-STYLE",
               {"New", "Arch", "Edit", "Arch", "Width", "0.8", "Oblique", "15", "Done", "Done"});
    const TextStyle* s = f.doc.textStyleByName("Arch");
    REQUIRE(s);
    CHECK(f.doc.header().currentTextStyle == s->id);
    CHECK(s->widthFactor == doctest::Approx(0.8));
    CHECK(s->oblique == doctest::Approx(15 * kPi / 180));
    CHECK(f.doc.textStyle(kTextStyleStandard)->widthFactor == doctest::Approx(1.0));
    CHECK(f.host.undo());
    CHECK(f.doc.textStyleByName("Arch") == nullptr);
}

TEST_CASE("-STYLE Rename, Current and Delete") {
    Fixture f;
    f.host.run("-ST", {"New", "A", "New", "B", "Rename", "A", "C", "Done"});
    CHECK(f.doc.textStyleByName("A") == nullptr);
    REQUIRE(f.doc.textStyleByName("C"));
    f.host.run("-ST", {"Delete", "B", "Done"});  // current: refused
    CHECK(f.doc.textStyleByName("B"));
    CHECK(f.host.logContains("current style cannot be deleted"));
    f.host.run("-ST", {"Current", "Standard", "Delete", "B", "Done"});
    CHECK(f.doc.textStyleByName("B") == nullptr);
    CHECK(f.doc.header().currentTextStyle == kTextStyleStandard);
}

TEST_CASE("-STYLE protects Standard and rejects bad input") {
    Fixture f;
    f.host.run("-ST", {"Rename", "Standard", "Done"});
    CHECK(f.host.logContains("cannot be renamed"));
    f.host.run("-ST", {"Delete", "Standard", "Done"});
    CHECK(f.host.logContains("cannot be deleted"));
    f.host.run("-ST", {"New", "Standard", "Done"});
    CHECK(f.host.logContains("already used"));
    f.host.run("-ST", {"Edit", "Standard", "Width", "0", "Oblique", "90", "Bogus", "Done", "Done"});
    CHECK(f.host.logContains("between 0.01 and 100"));
    CHECK(f.host.logContains("between -85 and 85"));
    CHECK(f.doc.textStyle(kTextStyleStandard)->widthFactor == doctest::Approx(1.0));
    f.host.run("-ST", {"Edit", "Nope", "Done"});
    CHECK(f.host.logContains("No text style"));
}

TEST_CASE("-STYLE refuses to delete a style in use") {
    Fixture f;
    f.host.run("-ST", {"New", "Used", "Current", "Standard", "Done"});
    const TextStyle* used = f.doc.textStyleByName("Used");
    REQUIRE(used);
    const auto id = used->id;
    f.host.run("DIMSTYLE", {"Edit", "Standard", "TextStyle", "Used", "Done", "Done"});
    f.host.run("-ST", {"Delete", "Used", "Done"});
    CHECK(f.doc.textStyle(id));
    CHECK(f.host.logContains("still use"));
}

TEST_CASE("-STYLE Edit Font warns when missing and 'default' clears it") {
    Fixture f;
    f.host.run("-ST", {"New", "F", "Edit", "F", "Font", "No Such Family 123", "Done", "Done"});
    CHECK(f.host.logContains("Font not found"));
    CHECK(f.doc.textStyleByName("F")->fontFamily == "No Such Family 123");
    f.host.run("-ST", {"Edit", "F", "Font", "default", "Done", "Done"});
    CHECK(f.doc.textStyleByName("F")->fontFamily.empty());
}

TEST_CASE("TEXTSTYLES and -STYLE are registered") {
    Fixture f;
    CHECK(f.reg.find("TEXTSTYLES"));
    CHECK(f.reg.find("-STYLE"));
    CHECK(f.reg.find("-ST"));
}
