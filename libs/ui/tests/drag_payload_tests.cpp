#include <doctest/doctest.h>

#include "tbcad/ui/drag_payload.h"

using namespace tbcad::ui;

TEST_CASE("content drag payload round trips and becomes import lines") {
    ContentDrag d{"C:/lib/doors.tbcad", "Block", {"DOOR", "WINDOW"}};
    const ContentDrag back = decodeContent(encodeContent(d));
    CHECK(back.file == d.file);
    CHECK(back.kind == "Block");
    CHECK(back.names == d.names);

    const auto lines = contentDropLines(back);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "CONTENTIMPORT");
    CHECK(lines[1] == d.file);
    CHECK(lines[2] == "Block");
    CHECK(lines[3] == "DOOR|WINDOW");
    CHECK(lines[4] == "INSERT DOOR");

    CHECK(contentDropLines({"f", "Layer", {"A"}}).size() == 4);  // no INSERT for a layer
    CHECK(contentDropLines({"", "Block", {"A"}}).empty());
}

TEST_CASE("tool drag payload keeps empty Enter lines and drops the origin line") {
    const std::string data = encodeTool("Doors", "Single", {"INSERT DOOR", "", "0.5"});
    CHECK(data.substr(0, data.find('\n')) == "Doors\tSingle");
    const auto lines = toolDropLines(data);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "INSERT DOOR");
    CHECK(lines[1].empty());
    CHECK(lines[2] == "0.5");
}
