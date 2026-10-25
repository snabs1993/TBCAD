#include <doctest/doctest.h>

#include "tbcad/cmd/help.h"

using namespace tbcad::cmd;

namespace {
const char* kText = R"(# title is ignored

## LINE
Draws segments.
Second line of summary.
Usage: Pick points.
Options: Close, Undo
Example: LINE 0,0 10,0

## circle
Draws a circle.
Usage: Pick centre then radius.
)";
}

TEST_CASE("help pages are parsed into fields") {
    HelpCatalog h;
    h.parse(kText);
    REQUIRE(h.entries().size() == 2);
    const HelpEntry* line = h.find("line");
    REQUIRE(line);
    CHECK(line->summary == "Draws segments. Second line of summary.");
    CHECK(line->usage == "Pick points.");
    CHECK(line->options == "Close, Undo");
    CHECK(line->example == "LINE 0,0 10,0");
    CHECK(h.find("CIRCLE") != nullptr);
    CHECK(h.find("ARC") == nullptr);
    CHECK(helpLines(*line).size() == 4);
}

TEST_CASE("help search finds by name first, then by text") {
    HelpCatalog h;
    h.parse(kText);
    auto r = h.search("circle");
    REQUIRE(r.size() == 1);
    CHECK(r[0]->name == "CIRCLE");
    r = h.search("radius");  // only in CIRCLE's usage
    REQUIRE(r.size() == 1);
    CHECK(r[0]->name == "CIRCLE");
    CHECK(h.search("close undo").size() == 1);
    CHECK(h.search("nothing here").empty());
    CHECK(h.search("").size() == 2);
}

TEST_CASE("a later page of the same name replaces the earlier one") {
    HelpCatalog h;
    h.parse(kText);
    h.parse("## LINE\nNew.\n");
    CHECK(h.entries().size() == 2);
    CHECK(h.find("LINE")->summary == "New.");
    CHECK(h.find("LINE")->usage.empty());
}
