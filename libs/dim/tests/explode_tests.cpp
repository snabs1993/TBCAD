#include "support.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

TEST_CASE("explode a linear dimension into lines, solid arrowheads and text") {
    Document doc;
    Entity e = entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10})));
    e.common.color = Color::index(2);
    EntityId id = add(doc, std::move(e));
    auto r = explodeDimension(doc, *doc.find(id));
    REQUIRE(r.ok());
    int lines = 0, hatches = 0, texts = 0;
    for (const Entity& x : r.value()) {
        CHECK(x.common.id == EntityId{});
        CHECK(x.common.layer == doc.find(id)->common.layer);
        CHECK(x.common.color == Color::index(2));
        switch (kind(x)) {
            case EntityKind::Line:
                ++lines;
                break;
            case EntityKind::Hatch:
                ++hatches;
                CHECK(entityAs<HatchData>(x)->solid);
                break;
            case EntityKind::Text:
                ++texts;
                CHECK(entityAs<TextData>(x)->utf8 == "100.00");
                break;
            default:
                FAIL("unexpected kind");
        }
    }
    CHECK(lines == 3);
    CHECK(hatches == 2);
    CHECK(texts == 1);
}

TEST_CASE("explode an angular dimension keeps its arc as an arc - a leader gives MText") {
    Document doc;
    EntityId a = add(
        doc,
        entityOf(Indirect<DimensionData>(angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15}))));
    auto r = explodeDimension(doc, *doc.find(a));
    REQUIRE(r.ok());
    int arcs = 0;
    for (const Entity& x : r.value())
        arcs += kind(x) == EntityKind::Arc ? 1 : 0;
    CHECK(arcs == 1);

    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}};
    ld.textPos = {12, 10};
    ld.utf8 = "Note";
    EntityId l = add(doc, entityOf(Indirect<LeaderData>(ld)));
    auto lr = explodeDimension(doc, *doc.find(l));
    REQUIRE(lr.ok());
    int mtexts = 0;
    for (const Entity& x : lr.value())
        mtexts += kind(x) == EntityKind::MText ? 1 : 0;
    CHECK(mtexts == 1);

    Entity line;
    line.data = LineData{{0, 0}, {1, 1}};
    CHECK(!explodeDimension(doc, line).ok());
}
