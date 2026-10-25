#include <doctest/doctest.h>

#include <chrono>
#include <string>

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
    t.headerRow = true;
    t.titleStyle = {5.5, 1, 8, 1};
    t.dataStyle = {0, 3, 0, 2};
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
    CHECK(back->headerRow);
    CHECK(back->titleStyle == t.titleStyle);
    CHECK(back->headerStyle.isDefault());
    CHECK(back->dataStyle == t.dataStyle);
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
    REQUIRE(grips.size() == 4);  // origin, corner, one column border, one row border
    CHECK(grips[0].x == doctest::Approx(100));
    CHECK(grips[1].x == doctest::Approx(180));
    CHECK(grips[1].y == doctest::Approx(180));
    CHECK(grips[2].x == doctest::Approx(140));  // column border on the top edge
    CHECK(grips[2].y == doctest::Approx(200));
    CHECK(grips[3].x == doctest::Approx(100));  // row border on the left edge
    CHECK(grips[3].y == doctest::Approx(190));
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

TEST_CASE(
    "CSV: quotes, doubled quotes, line ends, BOM and semicolons are read; export quotes back") {
    auto rows = table::parseCsv(
        "\xEF\xBB\xBF"
        "Door,\"W, mm\",Note\r\nD1,900,\"say \"\"hi\"\"\"\n\nD2,800,\n");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == std::vector<std::string>{"Door", "W, mm", "Note"});
    CHECK(rows[1] == std::vector<std::string>{"D1", "900", "say \"hi\""});
    CHECK(rows[2] == std::vector<std::string>{"D2", "800", ""});

    auto semi = table::parseCsv("a;b;c\n1;2;3\n");
    REQUIRE(semi.size() == 2);
    CHECK(semi[1] == std::vector<std::string>{"1", "2", "3"});

    TableData t = table::fromRows(rows, {0, 0}, 2.5, nullptr);
    CHECK(table::rowCount(t) == 3);
    CHECK(table::colCount(t) == 3);
    const std::string csv = table::toCsv(t);
    CHECK(table::parseCsv(csv) == rows);
}

TEST_CASE("formulas: sums, averages, cell references and arithmetic") {
    TableData t = grid(5, 3);
    setText(t, 0, 0, "10");
    setText(t, 1, 0, "20.5");
    setText(t, 2, 0, "4");
    setText(t, 3, 0, "=SUM(A1:A3)");
    setText(t, 0, 1, "=A1*2+1");
    setText(t, 1, 1, "=AVERAGE(A1:A3)");
    setText(t, 2, 1, "=MAX(A1:A3, 100) - MIN(A1:A3)");
    setText(t, 3, 1, "=ROUND(B2, 2)");
    setText(t, 4, 0, "=COUNT(A1:A4)");
    CHECK(displayText(t, 3, 0) == "34.5");
    CHECK(displayText(t, 0, 1) == "21");
    CHECK(displayText(t, 1, 1) == "11.5");
    CHECK(displayText(t, 2, 1) == "96");
    CHECK(displayText(t, 3, 1) == "11.5");
    CHECK(displayText(t, 4, 0) == "4");   // A1..A3 and the formula in A4
    CHECK(displayText(t, 0, 0) == "10");  // plain text is shown as typed
    setText(t, 0, 2, "=(1+2)^2/3");
    CHECK(displayText(t, 0, 2) == "3");
}

TEST_CASE("formulas: mistakes show their code, loops are caught") {
    TableData t = grid(3, 3);
    setText(t, 0, 0, "=1/0");
    setText(t, 0, 1, "=Z99");
    setText(t, 0, 2, "=FOO(1)");
    setText(t, 1, 0, "=B2");
    setText(t, 1, 1, "=A2");
    setText(t, 1, 2, "=1+");
    setText(t, 2, 0, "hello");
    setText(t, 2, 1, "=A3+1");
    CHECK(displayText(t, 0, 0) == "#DIV/0");
    CHECK(displayText(t, 0, 1) == "#REF");
    CHECK(displayText(t, 0, 2) == "#NAME");
    CHECK(displayText(t, 1, 0) == "#LOOP");
    CHECK(displayText(t, 1, 2) == "#ERR");
    CHECK(displayText(t, 2, 1) == "#VALUE");
}

TEST_CASE("formulas are drawn, exported and merged cells refer to their anchor") {
    TableData t = grid(2, 2);
    setText(t, 0, 0, "3");
    setText(t, 0, 1, "4");
    setText(t, 1, 0, "=A1+B1");
    bool found = false;
    for (const TextItem& item : layout(t).texts)
        found = found || item.utf8 == "7";
    CHECK(found);
    CHECK(toCsv(t).find("7") != std::string::npos);
    CHECK(cellName(0, 0) == "A1");
    CHECK(cellName(11, 27) == "AB12");
}

TEST_CASE("formulas survive the text form used in DXF") {
    TableData t = grid(3, 2);
    setText(t, 0, 0, "5");
    setText(t, 1, 0, "6");
    setText(t, 2, 0, "=SUM(A1:A2)*2");
    auto back = parse(serialize(t));
    REQUIRE(back);
    CHECK(cellAt(*back, 2, 0).utf8 == "=SUM(A1:A2)*2");
    CHECK(displayText(*back, 2, 0) == "22");
}

TEST_CASE("cell alignment is a paragraph code that formulas and CSV look through") {
    TableData t = grid(2, 2);
    setText(t, 0, 0, "7");
    setText(t, 0, 1, "=A1*2");
    REQUIRE(setCellAlign(t, 0, 1, CellAlign::Right));
    CHECK(cellAlign(cellAt(t, 0, 1)) == CellAlign::Right);
    CHECK(cellBody(cellAt(t, 0, 1).utf8) == "=A1*2");
    CHECK(displayText(t, 0, 1) == "14");
    REQUIRE(setCellAlign(t, 0, 0, CellAlign::Center));
    CHECK(displayText(t, 1, 1).empty());
    setText(t, 1, 1, "=A1+B1");  // alignment code on A1 does not disturb numbers
    CHECK(displayText(t, 1, 1) == "21");
    CHECK(toCsv(t).find("pxq") == std::string::npos);
    bool placed = false;
    for (const TextItem& item : layout(t).texts)
        placed = placed || (item.utf8 == "14" && item.attach == Attachment::MiddleRight);
    CHECK(placed);
    CHECK(setCellAlign(t, 0, 1, std::nullopt));
    CHECK_FALSE(cellAlign(cellAt(t, 0, 1)).has_value());
    CHECK_FALSE(setCellAlign(t, 9, 9, CellAlign::Left));
    // The code survives the DXF text form.
    setCellAlign(t, 0, 1, CellAlign::Right);
    auto back = parse(serialize(t));
    REQUIRE(back);
    CHECK(cellAlign(cellAt(*back, 0, 1)) == CellAlign::Right);
}

TEST_CASE("formulas: a cell used twice by the next one is worked out once") {
    // =A1+A1 down a column doubles each step: 2^50 without caching would never finish.
    TableData t = grid(51, 1);
    setText(t, 0, 0, "1");
    for (std::size_t r = 1; r < 51; ++r)
        setText(t, r, 0, "=A" + std::to_string(r) + "+A" + std::to_string(r));
    const auto start = std::chrono::steady_clock::now();
    CHECK(displayText(t, 50, 0) == "1125899906842624");
    // A grid of formulas that all sum a column that is itself formulas.
    TableData g = grid(200, 6);
    for (std::size_t r = 0; r < 200; ++r)
        setText(g, r, 0, std::to_string(r));
    for (std::size_t r = 0; r < 200; ++r)
        for (std::size_t c = 1; c < 6; ++c)
            setText(g, r, c,
                    "=SUM(" + std::string(1, char('A' + c - 1)) +
                        "1:" + std::string(1, char('A' + c - 1)) + "200)");
    for (std::size_t r = 0; r < 200; ++r)
        CHECK_FALSE(displayText(g, r, 1).empty());
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(secs < 20.0);
}

TEST_CASE(
    "cell styles: header fill and colour, data height and alignment, round trip in text form") {
    TableData t = grid(3, 2, "Parts");  // title row + 2 rows
    t.headerRow = true;
    t.titleStyle.fill = 5;
    t.headerStyle.fill = 9;
    t.headerStyle.color = 1;
    t.headerStyle.textHeight = 4;
    t.dataStyle.align = 2;
    cellAt(t, 1, 0).utf8 = "Item";
    cellAt(t, 2, 0).utf8 = "12";
    CHECK(rowRole(t, 0) == 0);
    CHECK(rowRole(t, 1) == 1);
    CHECK(rowRole(t, 2) == 2);
    const Layout L = layout(t);
    CHECK(L.fills.size() == 1 + 2);  // the merged title cell, the two header cells
    int nine = 0;
    for (const Fill& f : L.fills)
        nine += f.color == 9;
    CHECK(nine == 2);
    bool header = false, data = false;
    for (const TextItem& item : L.texts) {
        if (item.utf8 == "\\H4;\\C1;Item") {
            header = true;
            CHECK(item.attach == Attachment::MiddleLeft);
        }
        if (item.utf8 == "12") {
            data = true;
            CHECK(item.attach == Attachment::MiddleRight);
        }
    }
    CHECK(header);
    CHECK(data);

    auto back = parse(serialize(t));
    REQUIRE(back);
    CHECK(back->headerRow);
    CHECK(back->titleStyle == t.titleStyle);
    CHECK(back->headerStyle == t.headerStyle);
    CHECK(back->dataStyle == t.dataStyle);
    // A table without cell styles keeps the older, shorter text form.
    CHECK(serialize(grid(2, 2)).find("|1,") == std::string::npos);
    CHECK(parse(serialize(grid(2, 2))));
}

TEST_CASE("parseCsv reads cells copied from Excel (tab separated)") {
    auto rows = table::parseCsv("Item\tQty, each\r\nBolt\t4\r\n");
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].size() == 2);
    CHECK(rows[0][1] == "Qty, each");
    CHECK(rows[1][0] == "Bolt");
}

TEST_CASE("parseCsv reads Excel files saved as Thai ANSI (874) or Unicode text (UTF-16)") {
    // "ก,ข" in code page 874 (Excel on a Thai PC, "CSV (Comma delimited)")
    const auto ansi = table::parseCsv("\xA1,\xA2\r\n1,2\r\n");
    REQUIRE(ansi.size() == 2);
    CHECK(ansi[0][0] == "\xE0\xB8\x81");
    CHECK(ansi[0][1] == "\xE0\xB8\x82");
    // Western code page 1252: u umlaut
    const auto west = table::parseCsv("M\xFCller;1\r\n");
    REQUIRE(west.size() == 1);
    CHECK(west[0][0] == "M\xC3\xBCller");
    // UTF-16 little endian with a byte order mark, tab separated ("Unicode Text")
    const std::string_view u16(
        "\xFF\xFE"
        "a\0\t\0b\0\r\0\n\0"
        "\x01\x0E\t\0"
        "2\0",
        18);
    const auto wide = table::parseCsv(u16);
    REQUIRE(wide.size() == 2);
    CHECK(wide[0][1] == "b");
    CHECK(wide[1][0] == "\xE0\xB8\x81");
    CHECK(wide[1][1] == "2");
    // valid UTF-8 is left alone
    CHECK(table::parseCsv("\xE0\xB8\x81")[0][0] == "\xE0\xB8\x81");
}
