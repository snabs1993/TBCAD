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
