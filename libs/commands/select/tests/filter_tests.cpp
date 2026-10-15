#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select::test;

TEST_CASE("filterEntities ANDs the conditions") {
    Fixture f;
    const LayerId a = f.addLayer("A");
    auto l1 = f.addEntity(LineData{{0, 0}, {1, 0}}, a);
    auto l2 = f.addEntity(LineData{{0, 0}, {2, 0}});
    auto c1 = f.addEntity(CircleData{{0, 0}, 1}, a);
    auto ids = select::filterEntities(
        f.doc, {{select::Property::Type, "LINE", false}, {select::Property::Layer, "A", false}});
    REQUIRE(ids);
    CHECK(ids->size() == 1);
    CHECK(has(*ids, l1));
    auto notLine = select::filterEntities(f.doc, {{select::Property::Type, "LINE", true}});
    REQUIRE(notLine);
    CHECK(has(*notLine, c1));
    CHECK_FALSE(has(*notLine, l2));
    CHECK_FALSE(select::filterEntities(f.doc, {{select::Property::Type, "WIBBLE", false}}));
}

TEST_CASE("FILTER builds a list, applies it, saves it and uses it again") {
    Fixture f;
    const LayerId a = f.addLayer("A");
    auto l1 = f.addEntity(LineData{{0, 0}, {1, 0}}, a);
    f.addEntity(LineData{{0, 0}, {2, 0}});
    f.addEntity(CircleData{{0, 0}, 1}, a);
    f.host.run("FI", {"Add", "Type", "LINE", "Equals", "Add", "Layer", "A", "Equals", "Save",
                      "mine", "Apply"});
    REQUIRE(f.host.selection().size() == 1);
    CHECK(f.host.selection()[0] == l1);
    CHECK(select::savedFilters(f.doc) == std::vector<std::string>{"MINE"});
    f.host.setSelection({});
    f.host.run("FILTER", {"Use", "mine", "Apply"});
    REQUIRE(f.host.selection().size() == 1);
    CHECK(f.host.selection()[0] == l1);
}

TEST_CASE("encode and decode of a filter round-trip - bad values are refused") {
    std::vector<select::Query> q{{select::Property::Layer, "Walls, north", true},
                                 {select::Property::Color, "1", false}};
    auto back = select::decodeFilter(select::encodeFilter(q));
    REQUIRE(back.size() == 2);
    CHECK(back[0].value == "Walls, north");
    CHECK(back[0].negate);
    CHECK(back[1].property == select::Property::Color);
    Fixture f;
    f.host.run("FI", {"Add", "Color", "banana", "Equals", "Apply"});
    CHECK(f.host.logContains("not a valid"));
}
