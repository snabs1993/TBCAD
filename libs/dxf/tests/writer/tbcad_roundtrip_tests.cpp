// DXF -> document -> .tbcad -> document -> DXF over the corpus, the sample drawing and the block
// library: saving as .tbcad and opening again must lose nothing, and exporting the reopened
// document must give the same DXF as exporting the original.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::dxf {
namespace {

std::vector<std::string> dxfFiles() {
    std::vector<std::string> files;
    const std::filesystem::path roots[] = {
        std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / "reader",
        std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / "roundtrip",
        std::filesystem::path(TBCAD_SAMPLES_DIR), std::filesystem::path(TBCAD_ASSET_BLOCKS_DIR)};
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(root))
            if (entry.path().extension() == ".dxf")
                files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

TEST_CASE("every DXF survives DXF -> document -> .tbcad -> document, and exports the same DXF") {
    const std::vector<std::string> files = dxfFiles();
    REQUIRE(files.size() >= 40);
    std::size_t checked = 0;
    for (const std::string& path : files) {
        INFO("file: " << path);
        Report importReport;
        Result<ImportResult> first =
            importDxf(testing::readFile(path), ImportOptions{}, importReport);
        if (!first)
            continue;  // the broken_* files are meant to fail
        const Document& doc1 = *first.value().document;

        auto bytes = io::writeTbcadToMemory(doc1);
        REQUIRE_MESSAGE(bytes.ok(), (bytes.ok() ? "" : bytes.error().message));
        io::IoReport ioReport;
        auto reopened = io::readTbcad(bytes.value(), ioReport);
        REQUIRE_MESSAGE(reopened.ok(), (reopened.ok() ? "" : reopened.error().message));
        const Document& doc2 = *reopened.value();

        CHECK(doc1.entityCount() == doc2.entityCount());
        CHECK(doc1.layers().size() == doc2.layers().size());
        CHECK(doc1.blocks().size() == doc2.blocks().size());
        CHECK(doc1.textStyles().size() == doc2.textStyles().size());
        CHECK(doc1.dimStyles().size() == doc2.dimStyles().size());
        CHECK(doc1.linetypes().size() == doc2.linetypes().size());
        const std::string diff =
            testing::SemanticDump::diff(testing::SemanticDump(doc1), testing::SemanticDump(doc2));
        CHECK_MESSAGE(diff.empty(), diff);

        Report r1, r2;
        auto out1 = exportDxf(doc1, ExportOptions{}, r1);
        auto out2 = exportDxf(doc2, ExportOptions{}, r2);
        REQUIRE(out1.ok());
        REQUIRE(out2.ok());
        CHECK_MESSAGE(out1.value().bytes == out2.value().bytes,
                      "DXF exported from the reopened .tbcad differs");

        // Saving the reopened document again gives the same file.
        auto again = io::writeTbcadToMemory(doc2);
        REQUIRE(again.ok());
        CHECK(again.value() == bytes.value());
        ++checked;
    }
    CHECK(checked >= 40);
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {
namespace {

// The groups of one header variable ("" when absent), as the text from its name to the next
// variable or section end.
std::string headerVar(const std::string& dxf, const std::string& name) {
    const std::string key = "\r\n9\r\n" + name + "\r\n";
    const std::size_t at = dxf.find(key);
    if (at == std::string::npos)
        return {};
    const std::size_t end = std::min(dxf.find("\r\n9\r\n", at + 3), dxf.find("\r\n0\r\n", at + 3));
    return dxf.substr(at, end - at);
}

}  // namespace

TEST_CASE("header variables and classes TBCAD does not model survive open, save and open again") {
    const std::string src =
        "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1027\n"
        "  9\n$TDCREATE\n 40\n2460000.5\n"
        "  9\n$PROJECTNAME\n  1\nPlant \xE0\xB8\x81\n"
        "  9\n$FINGERPRINTGUID\n  2\n{ABCD-1234}\n"
        "  9\n$CMATERIAL\n347\n3F\n"
        "  9\n$LUNITS\n 70\n     4\n"
        "  9\n$CECOLOR\n 62\n     1\n"
        "  0\nENDSEC\n"
        "  0\nSECTION\n  2\nCLASSES\n"
        "  0\nCLASS\n  1\nACDBPLACEHOLDER\n  2\nAcDbPlaceHolder\n  3\nObjectDBX Classes\n"
        " 90\n        0\n 91\n        5\n280\n     0\n281\n     0\n"
        "  0\nENDSEC\n"
        "  0\nSECTION\n  2\nENTITIES\n  0\nLINE\n  8\n0\n 10\n0\n 20\n0\n 11\n5\n 21\n5\n"
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());

    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());

    // through a .tbcad file
    auto saved = io::writeTbcadToMemory(*first.value().document);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());

    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string dxf(reinterpret_cast<const char*>(out.value().bytes.data()),
                          out.value().bytes.size());

    CHECK(headerVar(dxf, "$TDCREATE").find("2460000.5") != std::string::npos);
    CHECK(headerVar(dxf, "$FINGERPRINTGUID").find("{ABCD-1234}") != std::string::npos);
    CHECK(headerVar(dxf, "$PROJECTNAME").find("Plant") != std::string::npos);
    CHECK(headerVar(dxf, "$CMATERIAL").empty());  // points at an object of the source file
    CHECK(headerVar(dxf, "$LUNITS") == "\r\n9\r\n$LUNITS\r\n70\r\n4");
    CHECK(headerVar(dxf, "$CECOLOR") == "\r\n9\r\n$CECOLOR\r\n62\r\n1");
    CHECK(dxf.find("ACDBPLACEHOLDER") != std::string::npos);
    CHECK(dxf.find("ObjectDBX Classes") != std::string::npos);
    // each variable once
    CHECK(dxf.find("$LUNITS") == dxf.rfind("$LUNITS"));
    CHECK(dxf.find("$TDCREATE") == dxf.rfind("$TDCREATE"));

    // and a second generation changes nothing
    Report r3;
    Result<ImportResult> again =
        importDxf(std::as_bytes(std::span(out.value().bytes)), ImportOptions{}, r3);
    REQUIRE(again.ok());
    Report r4;
    Result<ExportResult> out2 = exportDxf(*again.value().document, ExportOptions{}, r4);
    REQUIRE(out2.ok());
    CHECK(headerVar(std::string(reinterpret_cast<const char*>(out2.value().bytes.data()),
                                out2.value().bytes.size()),
                    "$TDCREATE") == headerVar(dxf, "$TDCREATE"));
}

}  // namespace tbcad::dxf
