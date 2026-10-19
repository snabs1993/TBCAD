// Objects that DXF keeps outside the entity list must survive DXF -> .tbcad -> DXF: groups,
// multiline styles and the multilines that use them.
#include <cstring>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/core/core.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/mline/mline.h"

namespace tbcad::dxf {
namespace {

const char* kSrc =
    "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n  9\n$INSUNITS\n 70\n     4\n"
    "  0\nENDSEC\n"
    "  0\nSECTION\n  2\nENTITIES\n"
    "  0\nLINE\n  5\n100\n  8\n0\n 10\n0.0\n 20\n0.0\n 11\n5.0\n 21\n0.0\n"
    "  0\nLINE\n  5\n101\n  8\n0\n 10\n0.0\n 20\n5.0\n 11\n5.0\n 21\n5.0\n"
    "  0\nMLINE\n  5\n102\n  8\n0\n100\nAcDbMline\n  2\nDOUBLE\n 40\n1.0\n 70\n     1\n"
    " 71\n     1\n 72\n     2\n 73\n     2\n 10\n0.0\n 20\n20.0\n 30\n0.0\n"
    " 11\n0.0\n 21\n20.0\n 31\n0.0\n 12\n1.0\n 22\n0.0\n 32\n0.0\n 13\n0.0\n 23\n1.0\n 33\n0.0\n"
    " 74\n     2\n 41\n0.5\n 41\n0.0\n 75\n     0\n 74\n     2\n 41\n-0.5\n 41\n0.0\n 75\n     0\n"
    " 11\n10.0\n 21\n20.0\n 31\n0.0\n 12\n1.0\n 22\n0.0\n 32\n0.0\n 13\n0.0\n 23\n1.0\n 33\n0.0\n"
    " 74\n     2\n 41\n0.5\n 41\n0.0\n 75\n     0\n 74\n     2\n 41\n-0.5\n 41\n0.0\n 75\n     0\n"
    "  0\nENDSEC\n"
    "  0\nSECTION\n  2\nOBJECTS\n"
    "  0\nDICTIONARY\n  5\nC\n330\n0\n100\nAcDbDictionary\n  3\nACAD_GROUP\n350\nD\n"
    "  3\nACAD_MLINESTYLE\n350\nE\n"
    "  0\nDICTIONARY\n  5\nD\n330\nC\n100\nAcDbDictionary\n  3\nMYGROUP\n350\nF\n"
    "  0\nGROUP\n  5\nF\n330\nD\n100\nAcDbGroup\n  300\nmy lines\n 70\n     0\n 71\n     1\n"
    "340\n100\n340\n101\n"
    "  0\nDICTIONARY\n  5\nE\n330\nC\n100\nAcDbDictionary\n  3\nDOUBLE\n350\n10\n"
    "  0\nMLINESTYLE\n  5\n10\n330\nE\n100\nAcDbMlineStyle\n  2\nDOUBLE\n 70\n    16\n"
    "  3\nDouble line\n 62\n   256\n 51\n90.0\n 52\n90.0\n 71\n     2\n"
    " 49\n0.5\n 62\n     1\n  6\nBYLAYER\n 49\n-0.5\n 62\n     2\n  6\nBYLAYER\n"
    "  0\nENDSEC\n  0\nEOF\n";

}  // namespace

TEST_CASE("groups, multiline styles and multilines survive the .tbcad and DXF round trip") {
    Report r1;
    Result<ImportResult> first = importDxf(testing::bytesOf(kSrc), ImportOptions{}, r1);
    REQUIRE(first.ok());
    const Document& d1 = *first.value().document;

    // The multiline stays one multiline (not four lines), with its style.
    std::size_t mlines = 0;
    d1.forEachEntity([&](const Entity& e) { mlines += entityAs<MlineData>(e) ? 1 : 0; });
    CHECK(mlines == 1);
    auto style = mline::findStyle(d1, "double");
    REQUIRE(style.has_value());
    CHECK(style->description == "Double line");
    CHECK(style->startCap);
    REQUIRE(style->offsets.size() == 2);
    CHECK(style->offsets[0] == doctest::Approx(0.5));
    const auto groups = readGroups(d1);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].name == "MYGROUP");
    CHECK(groups[0].members.size() == 2);

    auto bytes = io::writeTbcadToMemory(d1);
    REQUIRE(bytes.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(bytes.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());

    // And the exported file gives the same things back.
    std::vector<std::byte> raw(out.value().bytes.size());
    std::memcpy(raw.data(), out.value().bytes.data(), raw.size());
    Report r3;
    Result<ImportResult> again = importDxf(raw, ImportOptions{}, r3);
    REQUIRE(again.ok());
    const Document& d3 = *again.value().document;
    auto style3 = mline::findStyle(d3, "DOUBLE");
    REQUIRE(style3.has_value());
    CHECK(style3->description == "Double line");
    CHECK(style3->offsets.size() == 2);
    const auto groups3 = readGroups(d3);
    REQUIRE(groups3.size() == 1);
    CHECK(groups3[0].name == "MYGROUP");
    CHECK(groups3[0].members.size() == 2);
}

TEST_CASE("shape linetypes keep their shape number and shape file; a layout keeps its page setup") {
    const char* src =
        "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n  9\n$INSUNITS\n 70\n     4\n"
        "  0\nENDSEC\n  0\nSECTION\n  2\nTABLES\n"
        "  0\nTABLE\n  2\nLTYPE\n 70\n     1\n"
        "  0\nLTYPE\n  2\nTRACKS\n 70\n     0\n  3\ntracks\n 72\n    65\n 73\n     3\n 40\n6.0\n"
        " 49\n3.0\n 74\n     0\n 49\n0.0\n 74\n     4\n 75\n   131\n340\nAA\n 46\n0.5\n 50\n0.0\n"
        " 44\n0.0\n 45\n0.0\n 49\n-3.0\n 74\n     0\n"
        "  0\nENDTAB\n"
        "  0\nTABLE\n  2\nSTYLE\n 70\n     1\n"
        "  0\nSTYLE\n  5\nAA\n  2\nLTYPESHP\n 70\n     1\n 40\n0.0\n 41\n1.0\n 50\n0.0\n 71\n     "
        "0\n"
        " 42\n2.5\n  3\nltypeshp.shx\n  4\n\n"
        "  0\nENDTAB\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n"
        "  0\nDICTIONARY\n  5\nC\n330\n0\n100\nAcDbDictionary\n  3\nACAD_LAYOUT\n350\nD\n"
        "  0\nDICTIONARY\n  5\nD\n330\nC\n100\nAcDbDictionary\n  3\nModel\n350\nE\n"
        "  0\nLAYOUT\n  5\nE\n330\nD\n100\nAcDbPlotSettings\n  1\n\n  2\nnone_device\n  4\nA3\n"
        " 40\n7.5\n 41\n20.0\n 42\n7.5\n 43\n20.0\n 44\n420.0\n 45\n297.0\n 70\n  1024\n 73\n     "
        "0\n"
        "100\nAcDbLayout\n  1\nModel\n 70\n     1\n 71\n     0\n"
        "  0\nENDSEC\n  0\nEOF\n";
    Report r1;
    Result<ImportResult> first = importDxf(testing::bytesOf(src), ImportOptions{}, r1);
    REQUIRE(first.ok());
    auto bytes = io::writeTbcadToMemory(*first.value().document);
    REQUIRE(bytes.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(bytes.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    CHECK(text.find("\r\n131\r\n") != std::string::npos);   // 75 131: the shape number
    CHECK(text.find("ltypeshp.shx") != std::string::npos);  // the shape file, as a flag 1 style
    CHECK(text.find("\r\nA3\r\n") != std::string::npos);    // the model layout's paper
    CHECK(text.find("\r\n420.0") != std::string::npos);
}

}  // namespace tbcad::dxf
