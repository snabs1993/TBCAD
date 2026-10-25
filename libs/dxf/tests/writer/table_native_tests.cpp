// Tables as ACAD_TABLE: the reader takes the cells of a table written the way the DXF reference
// lays them out, and the writer (ExportOptions::nativeTables, R2007) writes that layout beside the
// block that draws the table.
#include <string>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/core/core.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/table/table.h"

namespace tbcad::dxf {
namespace {

// A 3 x 2 AutoCAD table (title row merged across, then two rows), cell heights 2.5, at (100, 200).
const char* kAcadTable =
    "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1021\n  0\nENDSEC\n"
    "  0\nSECTION\n  2\nENTITIES\n"
    "  0\nACAD_TABLE\n  5\n100\n330\n1F\n100\nAcDbEntity\n  8\n0\n100\nAcDbBlockReference\n"
    "  2\n*T1\n 10\n100.0\n 20\n200.0\n 30\n0.0\n"
    "100\nAcDbTable\n280\n     0\n342\n21\n343\n22\n 11\n1.0\n 21\n0.0\n 31\n0.0\n"
    " 90\n     0\n 91\n     3\n 92\n     2\n 93\n     0\n 94\n     0\n 95\n     0\n 96\n     0\n"
    "141\n8.0\n141\n6.0\n141\n6.0\n142\n30.0\n142\n40.0\n"
    "171\n     1\n172\n     0\n173\n     0\n174\n     0\n175\n     2\n176\n     1\n178\n     0\n"
    "145\n0.0\n170\n     5\n140\n3.0\n301\nCELL_VALUE\n 90\n     4\n 91\n     0\n  1\nParts\n"
    "304\nACVALUE_END\n"
    "171\n     1\n173\n     1\n175\n     1\n176\n     1\n170\n     4\n140\n2.5\n"
    "301\nCELL_VALUE\n 90\n     4\n  1\n\n304\nACVALUE_END\n"
    "171\n     1\n175\n     1\n176\n     1\n170\n     4\n140\n2.5\n"
    "301\nCELL_VALUE\n 90\n     4\n  1\nName\n304\nACVALUE_END\n"
    "171\n     1\n175\n     1\n176\n     1\n170\n     6\n140\n2.5\n"
    "301\nCELL_VALUE\n 90\n     4\n  1\nQty\n304\nACVALUE_END\n"
    "171\n     1\n175\n     1\n176\n     1\n170\n     4\n140\n2.5\n"
    "301\nCELL_VALUE\n 90\n     4\n  1\nBeam\n304\nACVALUE_END\n"
    "171\n     1\n175\n     1\n176\n     1\n170\n     6\n140\n2.5\n"
    "301\nCELL_VALUE\n 90\n     4\n  1\n12\n304\nACVALUE_END\n"
    "  0\nENDSEC\n  0\nEOF\n";

const TableData* firstTable(const Document& doc, std::size_t* count = nullptr) {
    const TableData* found = nullptr;
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Table) {
            found = entityAs<TableData>(e);
            ++n;
        }
    });
    if (count)
        *count = n;
    return found;
}

std::unique_ptr<Document> sampleDocument() {
    auto docPtr = std::make_unique<Document>();
    Document& doc = *docPtr;
    auto tx = doc.begin("table");
    TableData t = table::make(3, 2, 40, 6, "Parts", {100, 200}, 2.5);
    table::setText(t, 1, 0, "Beam");
    table::setText(t, 1, 1, "=1+2");
    table::setText(t, 2, 0, "Line one\nLine two");
    Entity e;
    e.data = Indirect<TableData>(std::move(t));
    REQUIRE(tx.add(std::move(e)).ok());
    tx.commit();
    return docPtr;
}

ExportResult exportWith(const Document& doc, bool native, DxfVersion v = DxfVersion::R2007) {
    ExportOptions o;
    o.version = v;
    o.nativeTables = native;
    Report r;
    Result<ExportResult> out = exportDxf(doc, o, r);
    REQUIRE(out.ok());
    return std::move(out.value());
}

bool has(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("an AutoCAD ACAD_TABLE is read into an editable table from its cells") {
    Report report;
    Result<ImportResult> r = importDxf(testing::bytesOf(kAcadTable), ImportOptions{}, report);
    REQUIRE(r.ok());
    const Document& doc = *r.value().document;
    std::size_t n = 0;
    const TableData* t = firstTable(doc, &n);
    REQUIRE(t);
    CHECK(n == 1);
    CHECK(table::rowCount(*t) == 3);
    CHECK(table::colCount(*t) == 2);
    CHECK(t->origin.x == doctest::Approx(100));
    CHECK(t->origin.y == doctest::Approx(200));
    CHECK(t->rowHeights[0] == doctest::Approx(8));
    CHECK(t->colWidths[1] == doctest::Approx(40));
    CHECK(t->titleRow);
    CHECK(table::cellAt(*t, 0, 0).colSpan == 2);
    CHECK(table::cellAt(*t, 0, 0).utf8 == "Parts");
    CHECK(table::cellAt(*t, 0, 1).covered);
    CHECK(table::cellAt(*t, 1, 0).utf8 == "Name");
    CHECK(table::cellAt(*t, 2, 1).utf8 == "12");
    CHECK(t->textHeight == doctest::Approx(2.5));
    // the table is the object; its *T1 block is not kept as a block of the drawing
    CHECK(doc.blocks().empty());
}

TEST_CASE("tables are written as a block of lines and text unless nativeTables is on") {
    const auto docPtr = sampleDocument();
    const Document& doc = *docPtr;
    const std::string off = exportWith(doc, false).bytes;
    CHECK_FALSE(has(off, "ACAD_TABLE"));
    CHECK_FALSE(has(off, "AcDbTable"));
    CHECK(has(off, "\r\n0\r\nINSERT\r\n"));
    // an old version keeps the block even when asked
    const std::string r2000 = exportWith(doc, true, DxfVersion::R2000).bytes;
    CHECK_FALSE(has(r2000, "ACAD_TABLE"));
}

TEST_CASE("nativeTables writes the ACAD_TABLE layout of the DXF reference") {
    const auto docPtr = sampleDocument();
    const Document& doc = *docPtr;
    const std::string dxf = exportWith(doc, true).bytes;
    // the entity: block reference part, then the table part with its style and block
    const std::size_t at = dxf.find("\r\n0\r\nACAD_TABLE\r\n");
    REQUIRE(at != std::string::npos);
    const std::string rec = dxf.substr(at, dxf.find("\r\n0\r\nENDSEC", at) - at);
    CHECK(has(rec, "100\r\nAcDbEntity"));
    CHECK(has(rec, "100\r\nAcDbBlockReference\r\n2\r\n*T1\r\n10\r\n100.0\r\n20\r\n200.0"));
    CHECK(has(rec, "100\r\nAcDbTable\r\n280\r\n0\r\n342\r\n"));
    CHECK(has(rec, "\r\n343\r\n"));
    CHECK(has(rec, "\r\n91\r\n3\r\n92\r\n2\r\n"));
    CHECK(has(rec, "\r\n141\r\n"));
    CHECK(has(rec, "\r\n142\r\n"));
    // 3 x 2 cells, each a text cell with a CELL_VALUE ... ACVALUE_END
    std::size_t cells = 0, ends = 0;
    for (std::size_t p = rec.find("\r\n171\r\n"); p != std::string::npos;
         p = rec.find("\r\n171\r\n", p + 1))
        ++cells;
    for (std::size_t p = rec.find("ACVALUE_END"); p != std::string::npos;
         p = rec.find("ACVALUE_END", p + 1))
        ++ends;
    CHECK(cells == 6);
    CHECK(ends == 6);
    CHECK(has(rec, "\r\n301\r\nCELL_VALUE\r\n"));
    CHECK(has(rec, "\r\n1\r\nParts\r\n"));
    CHECK(has(rec, "Line one\\PLine two"));           // a line break in a cell is \P
    CHECK(has(rec, "\r\n175\r\n2\r\n176\r\n1\r\n"));  // the merged title spans 2 columns
    // what the entity points at exists
    CHECK(has(dxf, "\r\n0\r\nBLOCK\r\n"));
    CHECK(has(dxf, "\r\n2\r\n*T1\r\n"));
    CHECK(has(dxf, "\r\n  1\r\nACAD_TABLE\r\n  2\r\nAcDbTable\r\n"));
    CHECK(has(dxf, "\r\n0\r\nTABLESTYLE\r\n"));
    CHECK(has(dxf, "\r\n3\r\nACAD_TABLESTYLE\r\n"));
}

TEST_CASE("a native table written by TBCAD comes back exactly, formulas and all") {
    const auto docPtr = sampleDocument();
    const Document& doc = *docPtr;
    const TableData* before = firstTable(doc);
    REQUIRE(before);
    const std::string dxf = exportWith(doc, true).bytes;
    Report report;
    Result<ImportResult> r = importDxf(testing::bytesOf(dxf), ImportOptions{}, report);
    REQUIRE(r.ok());
    std::size_t n = 0;
    const TableData* after = firstTable(*r.value().document, &n);
    REQUIRE(after);
    CHECK(n == 1);
    CHECK(table::serialize(*after) == table::serialize(*before));
    CHECK(r.value().document->blocks().empty());
}

TEST_CASE(
    "a native table read by its cells (no TBCAD data, as after AutoCAD saved it) keeps text, "
    "sizes and merges") {
    const auto docPtr = sampleDocument();
    const Document& doc = *docPtr;
    std::string dxf = exportWith(doc, true).bytes;
    // drop the extended data of the table, leaving what any other program sees
    const std::size_t x = dxf.find("1001\r\nTBCAD\r\n", dxf.find("ACAD_TABLE"));
    REQUIRE(x != std::string::npos);
    const std::size_t end = dxf.find("\r\n0\r\nENDSEC", x);
    dxf.erase(x, end - x);
    Report report;
    Result<ImportResult> r = importDxf(testing::bytesOf(dxf), ImportOptions{}, report);
    REQUIRE(r.ok());
    const TableData* t = firstTable(*r.value().document);
    REQUIRE(t);
    CHECK(table::rowCount(*t) == 3);
    CHECK(table::colCount(*t) == 2);
    CHECK(t->origin.x == doctest::Approx(100));
    CHECK(t->origin.y == doctest::Approx(200));
    CHECK(t->colWidths[0] == doctest::Approx(40));
    CHECK(t->titleRow);
    CHECK(table::cellAt(*t, 0, 0).colSpan == 2);
    CHECK(table::cellAt(*t, 0, 0).utf8 == "Parts");
    CHECK(table::cellAt(*t, 1, 0).utf8 == "Beam");
    CHECK(table::cellAt(*t, 1, 1).utf8 == "3");  // the formula's value
    CHECK(table::cellAt(*t, 2, 0).utf8 == "Line one\\PLine two");
}

}  // namespace tbcad::dxf
