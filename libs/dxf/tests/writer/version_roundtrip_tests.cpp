// Open, save as DXF at every version this program writes, open again: the same objects of the
// same kinds come back, with the same properties (see docs/dxf-export.md for what R12 changes).
#include <algorithm>
#include <filesystem>
#include <map>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::dxf {
namespace {

std::vector<std::string> drawings() {
    std::vector<std::string> files;
    const std::filesystem::path roots[] = {
        std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / "reader",
        std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / "roundtrip",
        std::filesystem::path(TBCAD_SAMPLES_DIR), std::filesystem::path(TBCAD_ASSET_BLOCKS_DIR)};
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(root))
            if (entry.path().extension() == ".dxf" || entry.path().extension() == ".tbcad")
                files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::map<int, std::size_t> kinds(const Document& doc) {
    std::map<int, std::size_t> m;
    doc.forEachEntity([&](const Entity& e) { ++m[static_cast<int>(kind(e))]; });
    return m;
}

std::string kindsText(const std::map<int, std::size_t>& m) {
    std::string s;
    for (auto [k, n] : m)
        s += std::to_string(k) + ":" + std::to_string(n) + " ";
    return s;
}

}  // namespace

TEST_CASE("every drawing survives saving as DXF R2000, R2007 and R12 and opening again") {
    std::size_t checked = 0;
    for (const std::string& path : drawings()) {
        INFO("file: " << path);
        std::unique_ptr<Document> owned;
        if (path.ends_with(".tbcad")) {
            io::IoReport ioReport;
            auto doc = io::readTbcad(std::filesystem::path(path), ioReport);
            REQUIRE(doc.ok());
            owned = std::move(doc.value());
        } else {
            Report report;
            Result<ImportResult> first = importDxf(testing::readFile(path), ImportOptions{}, report);
            if (!first)
                continue;  // the broken_* files are meant to fail
            owned = std::move(first.value().document);
        }
        const Document& doc1 = *owned;
        for (DxfVersion v : {DxfVersion::R2000, DxfVersion::R2007, DxfVersion::R12}) {
            INFO("version: " << versionCode(v));
            ExportOptions opt;
            opt.version = v;
            Report exportReport;
            Result<ExportResult> out = exportDxf(doc1, opt, exportReport);
            REQUIRE(out.ok());
            Report report;
            Result<ImportResult> second =
                importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, report);
            REQUIRE(second.ok());
            const Document& doc2 = *second.value().document;
            const auto k1 = kinds(doc1), k2 = kinds(doc2);
            CHECK(doc2.layers().size() == doc1.layers().size());
            if (v != DxfVersion::R12) {
                CHECK_MESSAGE(k1 == k2, kindsText(k1) << "-> " << kindsText(k2));
                const std::string diff = testing::SemanticDump::diff(
                    testing::SemanticDump(doc1), testing::SemanticDump(doc2));
                CHECK_MESSAGE(diff.empty(), diff);
                continue;
            }
            // R12 writes hatches, ellipses, splines, leaders and notes as the simple objects it
            // has (see docs/dxf-export.md): those kinds may only grow, none may be lost.
            for (EntityKind k : {EntityKind::Line, EntityKind::Circle, EntityKind::Arc,
                                 EntityKind::Polyline, EntityKind::Point, EntityKind::Text,
                                 EntityKind::Insert, EntityKind::Dimension}) {
                const int i = static_cast<int>(k);
                const std::size_t before = k1.contains(i) ? k1.at(i) : 0;
                const std::size_t after = k2.contains(i) ? k2.at(i) : 0;
                CHECK_MESSAGE(after >= before, "kind " << i << ": " << kindsText(k1) << "-> "
                                                       << kindsText(k2));
            }
        }
        ++checked;
    }
    CHECK(checked >= 40);
}

}  // namespace tbcad::dxf
