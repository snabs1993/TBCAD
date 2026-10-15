#include <doctest/doctest.h>

#include "tbcad/geom/geom.h"
#include "tbcad/table/table.h"

using namespace tbcad;
using namespace tbcad::table;

namespace {

TableData grid(std::size_t rows, std::size_t cols, const std::string& title = "") {
    return make(rows, cols, 40, 10, title, {100, 200}, 3);
}

}  // namespace

TEST_CASE("make: sizes, title row merged across the width") {
    TableData t = grid(4, 3, "Parts");
    CHECK(rowCount(t) == 4);
    CHECK(colCount(t) == 3);
    CHECK(totalWidth(t) == doctest::Approx(120));
    CHECK(totalHeight(t) == doctest::Approx(40));
    CHECK(t.titleRow);
    CHECK(cellAt(t, 0, 0).colSpan == 3);
    CHECK(cellAt(t, 0, 1).covered);
    CHECK(cellAt(t, 0, 0).utf8 == "Parts");
    CHECK(anchorOf(t, 0, 2) == CellRef{0, 0});
}

TEST_CASE("frame: origin is the top-left corner, rows go down, rotation turns it") {
    TableData t = grid(2, 2);
    CHECK(toWorld(t, {0, 0}).x == doctest::Approx(100));
    CHECK(toWorld(t, {10, 5}).y == doctest::Approx(195));
    t.rotation = kPi / 2;
    Vec2 p = toWorld(t, {10, 0});
    CHECK(p.x == doctest::Approx(100));
    CHECK(p.y == doctest::Approx(210));
    Vec2 back = toLocal(t, p);
    CHECK(back.x == doctest::Approx(10));
    CHECK(back.y == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("hit finds the cell, or the anchor of a merge") {
    TableData t = grid(3, 3);
    auto h = hit(t, {100 + 45, 200 - 15});
    REQUIRE(h);
    CHECK(*h == CellRef{1, 1});
    CHECK_FALSE(hit(t, {0, 0}));
    REQUIRE(merge(t, {0, 0}, {1, 1}));
    CHECK(*hit(t, {100 + 45, 200 - 15}) == CellRef{0, 0});
    Rect r = cellRect(t, 1, 1);
    CHECK(r.x1 == doctest::Approx(80));
    CHECK(r.y1 == doctest::Approx(20));
}

TEST_CASE("merge joins texts, refuses a range that cuts another merge, unmerge restores") {
    TableData t = grid(3, 3);
    setText(t, 0, 0, "a");
    setText(t, 0, 1, "b");
    REQUIRE(merge(t, {0, 0}, {0, 1}));
    CHECK(cellAt(t, 0, 0).utf8 == "a b");
    CHECK(cellAt(t, 0, 0).colSpan == 2);
    CHECK_FALSE(merge(t, {0, 1}, {1, 1}));  // would cut the first merge
    CHECK(merge(t, {0, 0}, {1, 1}));        // swallows it
    CHECK(unmerge(t, 1, 1));
    CHECK_FALSE(cellAt(t, 1, 1).covered);
    CHECK_FALSE(unmerge(t, 2, 2));
    CHECK_FALSE(merge(t, {1, 1}, {1, 1}));
}

TEST_CASE("insert and delete rows and columns keep cells in place") {
    TableData t = grid(2, 2);
    setText(t, 0, 0, "A");
    setText(t, 0, 1, "B");
    setText(t, 1, 0, "C");
    setText(t, 1, 1, "D");
    REQUIRE(insertRow(t, 1));
    CHECK(rowCount(t) == 3);
    CHECK(cellAt(t, 1, 0).utf8.empty());
    CHECK(cellAt(t, 2, 1).utf8 == "D");
    REQUIRE(insertCol(t, 1, 15));
    CHECK(colCount(t) == 3);
    CHECK(t.colWidths[1] == doctest::Approx(15));
    CHECK(cellAt(t, 0, 2).utf8 == "B");
    CHECK(cellAt(t, 2, 2).utf8 == "D");
    REQUIRE(deleteCol(t, 1));
    REQUIRE(deleteRow(t, 1));
    CHECK(rowCount(t) == 2);
    CHECK(colCount(t) == 2);
    CHECK(cellAt(t, 1, 1).utf8 == "D");
    CHECK_FALSE(deleteRow(t, 5));
    REQUIRE(deleteRow(t, 0));
    CHECK_FALSE(deleteRow(t, 0));  // the last row stays
}

TEST_CASE("inserting through a merge splits it - a title row stays first") {
    TableData t = grid(3, 3, "T");
    REQUIRE(insertRow(t, 0));  // goes after the title
    CHECK(cellAt(t, 0, 0).utf8 == "T");
    CHECK(rowCount(t) == 4);
    TableData m = grid(3, 3);
    REQUIRE(merge(m, {0, 0}, {1, 0}));
    REQUIRE(insertRow(m, 1));
    CHECK(cellAt(m, 0, 0).rowSpan == 1);
    CHECK_FALSE(cellAt(m, 2, 0).covered);
}

TEST_CASE("layout: grid lines skip merged interiors, one text per filled cell") {
    TableData t = grid(2, 2);
    CHECK(layout(t).lines.size() == 6);  // 3 horizontal + 3 vertical
    setText(t, 0, 0, "x");
    REQUIRE(merge(t, {0, 0}, {0, 1}));
    Layout L = layout(t);
    CHECK(L.lines.size() == 6);
    // the middle vertical line only covers the second row now
    for (const auto& [a, b] : L.lines) {
        if (std::abs(a.x - 140) < 1e-9 && std::abs(b.x - 140) < 1e-9)
            CHECK(std::abs(a.y - b.y) == doctest::Approx(10));
    }
    REQUIRE(L.texts.size() == 1);
    CHECK(L.texts[0].width == doctest::Approx(80 - 2 * t.margin));
}

TEST_CASE("bounds, display and explode") {
    TableData t = grid(2, 2);
    setText(t, 1, 1, "hi");
    BBox2 b = bounds(t);
    CHECK(b.min.x == doctest::Approx(100));
    CHECK(b.max.x == doctest::Approx(180));
    CHECK(b.min.y == doctest::Approx(180));
    CHECK(b.max.y == doctest::Approx(200));

    Document doc;
    Entity e;
    e.data = Indirect<TableData>(t);
    auto parts = explodeTable(doc, e);
    REQUIRE(parts.ok());
    int lines = 0, texts = 0;
    for (const Entity& p : parts.value()) {
        if (kind(p) == EntityKind::Line)
            ++lines;
        if (kind(p) == EntityKind::MText)
            ++texts;
    }
    CHECK(lines == 6);
    CHECK(texts == 1);

    registerDisplay();
    CHECK(geom::DisplayRegistry::has(EntityKind::Table));
    DisplayList dl;
    geom::DisplayContext ctx;
    geom::buildDisplay(doc, e, ctx, dl);
    CHECK(dl.items.size() >= 6);
}

TEST_CASE("the document accepts a table and rejects a broken one") {
    Document doc;
    {
        Transaction tx = doc.begin("t");
        Entity e;
        e.data = Indirect<TableData>(grid(2, 2));
        CHECK(tx.add(std::move(e)).ok());
        TableData bad = grid(2, 2);
        bad.cells.pop_back();
        Entity f;
        f.data = Indirect<TableData>(bad);
        CHECK_FALSE(tx.add(std::move(f)).ok());
        TableData over = grid(2, 2);
        over.cells[0].colSpan = 3;
        Entity g;
        g.data = Indirect<TableData>(over);
        CHECK_FALSE(tx.add(std::move(g)).ok());
        tx.commit();
    }
}

TEST_CASE("serialize and parse round-trip, including awkward text") {
    TableData t = grid(3, 3, "Title; with | odd : chars % \xE0\xB8\xAA");
    setText(t, 1, 1, "line\nbreak {\\C1;red}");
    REQUIRE(merge(t, {1, 0}, {2, 0}));
    t.rotation = 0.3;
    t.margin = 1.25;
    const std::string s = serialize(t);
    for (char c : s)
        CHECK((static_cast<unsigned char>(c) < 128 && c != '\n'));
    auto back = parse(s);
    REQUIRE(back);
    CHECK(back->cells.size() == t.cells.size());
    CHECK(back->colWidths == t.colWidths);
    CHECK(back->rowHeights == t.rowHeights);
    CHECK(back->rotation == t.rotation);
    CHECK(back->margin == t.margin);
    CHECK(back->titleRow);
    for (std::size_t i = 0; i < t.cells.size(); ++i) {
        CHECK(back->cells[i].utf8 == t.cells[i].utf8);
        CHECK(back->cells[i].colSpan == t.cells[i].colSpan);
        CHECK(back->cells[i].rowSpan == t.cells[i].rowSpan);
        CHECK(back->cells[i].covered == t.cells[i].covered);
    }
}

TEST_CASE("parse rejects damaged text") {
    const std::string good = serialize(grid(2, 2));
    CHECK(parse(good));
    CHECK_FALSE(parse(""));
    CHECK_FALSE(parse("tbcad-table 2|0|0"));
    CHECK_FALSE(parse(good.substr(0, good.size() / 2)));
    std::string bad = good;
    bad.replace(bad.find("2:") == std::string::npos ? 0 : bad.find("1:1:0"), 5, "9:9:0");
    CHECK_FALSE(parse(bad));
}

TEST_CASE("transforms scale and turn a table, and refuse a mirror") {
    Entity e;
    e.data = Indirect<TableData>(grid(2, 2));
    auto scaled = geom::transformed(e, Transform2::scale(2, 2));
    REQUIRE(scaled.ok());
    const auto* t = entityAs<TableData>(scaled.value());
    CHECK(t->colWidths[0] == doctest::Approx(80));
    CHECK(t->rowHeights[0] == doctest::Approx(20));
    CHECK(t->textHeight == doctest::Approx(6));
    CHECK(t->origin.x == doctest::Approx(200));
    auto turned = geom::transformed(e, Transform2::rotate(kPi / 2));
    REQUIRE(turned.ok());
    CHECK(entityAs<TableData>(turned.value())->rotation == doctest::Approx(kPi / 2));
    CHECK_FALSE(geom::transformed(e, Transform2::scale(1, -1)).ok());
    CHECK_FALSE(geom::transformed(e, Transform2::scale(2, 1)).ok());
}

TEST_CASE("grips: the origin moves it, the far corner rescales the grid") {
    Entity e;
    e.data = Indirect<TableData>(grid(2, 2));
    auto grips = geom::gripPoints(e);
    REQUIRE(grips.size() == 2);
    CHECK(grips[0].x == doctest::Approx(100));
    CHECK(grips[1].x == doctest::Approx(180));
    CHECK(grips[1].y == doctest::Approx(180));
    auto moved = geom::moveGrip(e, 0, {10, 10});
    REQUIRE(moved.ok());
    CHECK(entityAs<TableData>(moved.value())->origin.x == doctest::Approx(10));
    auto resized = geom::moveGrip(e, 1, {200, 160});
    REQUIRE(resized.ok());
    const auto* t = entityAs<TableData>(resized.value());
    CHECK(t->colWidths[0] == doctest::Approx(50));
    CHECK(t->rowHeights[1] == doctest::Approx(20));
    CHECK_FALSE(geom::moveGrip(e, 1, {50, 300}).ok());
}
