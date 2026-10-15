#include "support.h"
#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

namespace {

struct Counts {
    int polylines = 0, fills = 0;
};
Counts count(const DisplayList& l) {
    Counts c;
    for (const auto& it : l.items) {
        if (std::holds_alternative<DisplayPolyline>(it))
            ++c.polylines;
        else if (std::holds_alternative<DisplayFill>(it))
            ++c.fills;
    }
    return c;
}

}  // namespace

TEST_CASE("the display generator is registered and draws lines, arrows and text") {
    registerDisplay();
    CHECK(geom::DisplayRegistry::has(EntityKind::Dimension));
    CHECK(geom::DisplayRegistry::has(EntityKind::Leader));
    Document doc;
    EntityId id = add(doc, entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10}))));
    DisplayList out;
    geom::buildDisplay(doc, *doc.find(id), geom::DisplayContext{}, out);
    Counts c = count(out);
    CHECK(c.polylines == 3);  // two extension lines and the dimension line
    CHECK(c.fills >= 3);      // two arrowheads and at least one glyph
}

TEST_CASE("ByBlock style colours take the dimension's own colour - style colours override") {
    registerDisplay();
    Document doc;
    Entity e = entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10})));
    e.common.color = Color::index(1);  // red
    EntityId id = add(doc, std::move(e));
    DisplayList out;
    geom::buildDisplay(doc, *doc.find(id), geom::DisplayContext{}, out);
    for (const auto& it : out.items)
        if (const auto* p = std::get_if<DisplayPolyline>(&it))
            CHECK(p->style.color == paletteColor(1));

    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.extLineColor = Color::index(3); }));
    tx.commit();
    DisplayList out2;
    geom::buildDisplay(doc, *doc.find(id), geom::DisplayContext{}, out2);
    int green = 0;
    for (const auto& it : out2.items)
        if (const auto* p = std::get_if<DisplayPolyline>(&it))
            green += p->style.color == paletteColor(3) ? 1 : 0;
    CHECK(green == 2);
}

TEST_CASE("leader and angular dimensions produce display items") {
    registerDisplay();
    Document doc;
    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}};
    ld.textPos = {12, 10};
    ld.utf8 = "A";
    EntityId l = add(doc, entityOf(Indirect<LeaderData>(ld)));
    DisplayList out;
    geom::buildDisplay(doc, *doc.find(l), geom::DisplayContext{}, out);
    CHECK(count(out).polylines == 2);
    CHECK(count(out).fills >= 2);

    EntityId a = add(
        doc,
        entityOf(Indirect<DimensionData>(angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15}))));
    DisplayList out2;
    geom::buildDisplay(doc, *doc.find(a), geom::DisplayContext{}, out2);
    CHECK(count(out2).polylines >= 3);  // arc and two extension lines
}

TEST_CASE("bounds cover the dimension line, extension lines and text") {
    registerDisplay();
    Document doc;
    EntityId id = add(doc, entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10}))));
    BBox2 b = geom::bounds(*doc.find(id), doc);
    CHECK(b.min.x <= 0.0);
    CHECK(b.max.x >= 100.0);
    CHECK(b.max.y >= 11.25);
}
