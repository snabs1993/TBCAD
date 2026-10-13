// DXF -> document -> DXF -> document over the whole test corpus (W4-6 acceptance).
#include <algorithm>
#include <filesystem>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {
namespace {

std::vector<std::string> corpusFiles() {
    std::vector<std::string> files;
    for (const char* dir : {"reader", "roundtrip"}) {
        std::filesystem::path root = std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / dir;
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().extension() == ".dxf")
                files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

Result<ImportResult> importBytes(const std::vector<std::byte>& bytes) {
    Report report;
    return importDxf(bytes, ImportOptions{}, report);
}

std::vector<std::byte> toBytes(const std::string& s) {
    return testing::bytesOf(s);
}

}  // namespace

TEST_CASE("every corpus file survives DXF -> tbcad document -> DXF -> document") {
    std::vector<std::string> files = corpusFiles();
    REQUIRE(files.size() >= 20);
    std::size_t roundTripped = 0;
    for (const std::string& path : files) {
        INFO("file: " << path);
        Result<ImportResult> first = importBytes(testing::readFile(path));
        if (!first)
            continue;  // the broken_* files are meant to fail
        const Document& doc1 = *first.value().document;

        Report exportReport;
        Result<ExportResult> out = exportDxf(doc1, ExportOptions{}, exportReport);
        REQUIRE(out.ok());
        CHECK(exportReport.count(Severity::Error) == 0);

        Result<ImportResult> second = importBytes(toBytes(out.value().bytes));
        REQUIRE_MESSAGE(second.ok(), (second.ok() ? "" : second.error().message));
        const Document& doc2 = *second.value().document;

        std::string diff = testing::SemanticDump::diff(testing::SemanticDump(doc1),
                                                       testing::SemanticDump(doc2));
        CHECK_MESSAGE(diff.empty(), diff);

        // A second export of the re-imported document is byte for byte the same file.
        Report again;
        Result<ExportResult> out2 = exportDxf(doc2, ExportOptions{}, again);
        REQUIRE(out2.ok());
        if (out2.value().bytes != out.value().bytes) {
            const std::string& a = out.value().bytes;
            const std::string& b = out2.value().bytes;
            std::size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i])
                ++i;
            std::size_t from = a.rfind('\n', i > 80 ? i - 80 : 0);
            FAIL_CHECK("second export differs at byte " << i << ":\n"
                                                         << a.substr(from == std::string::npos ? 0 : from, 200)
                                                         << "\n----\n"
                                                         << b.substr(from == std::string::npos ? 0 : from, 200));
        }
        ++roundTripped;
    }
    CHECK(roundTripped >= 20);
}

}  // namespace tbcad::dxf
