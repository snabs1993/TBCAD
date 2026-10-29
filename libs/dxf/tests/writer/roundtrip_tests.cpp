// DXF -> document -> DXF -> document over the whole test corpus (W4-6 acceptance).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dim/dim.h"
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

// Equal line by line; numbers may differ in the last digits (trigonometry differs by one unit in
// the last place between compilers).
bool sameExport(const std::string& a, const std::string& b, std::size_t& badLine) {
    auto lines = [](const std::string& s) {
        std::vector<std::string> v;
        std::size_t from = 0;
        while (from < s.size()) {
            std::size_t to = s.find('\n', from);
            if (to == std::string::npos)
                to = s.size();
            v.push_back(s.substr(from, to - from));
            from = to + 1;
        }
        return v;
    };
    const auto la = lines(a), lb = lines(b);
    if (la.size() != lb.size()) {
        badLine = std::min(la.size(), lb.size());
        return false;
    }
    for (std::size_t i = 0; i < la.size(); ++i) {
        if (la[i] == lb[i])
            continue;
        char *ea = nullptr, *eb = nullptr;
        const double x = std::strtod(la[i].c_str(), &ea), y = std::strtod(lb[i].c_str(), &eb);
        const bool numbers = *ea == '\r' || *ea == 0;
        if (numbers && (*eb == '\r' || *eb == 0) && ea != la[i].c_str() && eb != lb[i].c_str() &&
            std::abs(x - y) <= 1e-9 * std::max({1.0, std::abs(x), std::abs(y)}))
            continue;
        badLine = i;
        return false;
    }
    return true;
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

        // Objects this program has no reader for (AEC, Civil, ACIS ...) are written back as they
        // were read, so the re-imported document is a different, equally valid one.
        Result<ImportResult> second = importBytes(toBytes(out.value().bytes));
        REQUIRE_MESSAGE(second.ok(), (second.ok() ? "" : second.error().message));
        const Document& doc2 = *second.value().document;
        // Saving and opening again never adds or loses objects (a Civil label's text, kept both
        // in its proxy graphics and as a Text, once came back twice).
        CHECK(doc2.modelSpaceCount() == doc1.modelSpaceCount());
        CHECK(doc2.entityCount() == doc1.entityCount());
        if (path.find("aec_objects") != std::string::npos) {
            CHECK(out.value().bytes.find("AEC_WALL") != std::string::npos);
            ++roundTripped;
            continue;
        }

        std::string diff =
            testing::SemanticDump::diff(testing::SemanticDump(doc1), testing::SemanticDump(doc2));
        CHECK_MESSAGE(diff.empty(), diff);

        // A second export of the re-imported document is the same file.
        Report again;
        Result<ExportResult> out2 = exportDxf(doc2, ExportOptions{}, again);
        REQUIRE(out2.ok());
        std::size_t bad = 0;
        if (!sameExport(out.value().bytes, out2.value().bytes, bad)) {
            if (const char* dump = std::getenv("TBCAD_DUMP_RT")) {
                std::ofstream(std::string(dump) + path.substr(path.rfind('/') + 1) + ".1")
                    << out.value().bytes;
                std::ofstream(std::string(dump) + path.substr(path.rfind('/') + 1) + ".2")
                    << out2.value().bytes;
            }
            auto lineAt = [&](const std::string& b, std::size_t n) {
                std::size_t p0 = 0;
                for (std::size_t i = 0; i < n; ++i)
                    p0 = b.find('\n', p0) + 1;
                return b.substr(p0, b.find('\n', p0) - p0);
            };
            FAIL_CHECK("second export differs at line "
                       << bad + 1 << ": '" << lineAt(out.value().bytes, bad) << "' vs '"
                       << lineAt(out2.value().bytes, bad) << "'");
        }
        ++roundTripped;
    }
    CHECK(roundTripped >= 20);
}

TEST_CASE("every corpus file set to metres is written in metres and reads back the same") {
    std::size_t done = 0;
    for (const std::string& path : corpusFiles()) {
        INFO("file: " << path);
        if (path.find("aec_objects") != std::string::npos)
            continue;
        Result<ImportResult> first = importBytes(testing::readFile(path));
        if (!first)
            continue;
        Document& doc1 = *first.value().document;
        // Objects kept as the DXF groups they were read as are written back as they were, in the
        // numbers of their source file; those files are not about units.
        bool kept = false;
        doc1.forEachEntity([&](const Entity& e) {
            if (const auto* p = entityAs<ProxyData>(e); p && !p->raw.empty())
                kept = true;
        });
        if (kept)
            continue;
        {
            Transaction tx = doc1.begin("metres");
            REQUIRE(tx.modifyHeader([](Header& h) { h.units = LengthUnit::Meter; }));
            std::vector<DimStyleId> styles;  // dimension styles show the drawing's unit, as read
            for (const auto& kv : doc1.dimStyles())
                styles.push_back(kv.first);
            for (DimStyleId id : styles)
                REQUIRE(tx.modifyDimStyle(id, [](DimStyle& d) { d.units = LengthUnit::Meter; }));
            tx.commit();
            // Dimensions place their text again for the new number format.
            std::vector<EntityId> dims;
            doc1.forEachEntity([&](const Entity& e) {
                if (entityAs<DimensionData>(e))
                    dims.push_back(e.common.id);
            });
            Transaction t2 = doc1.begin("refresh dimensions");
            for (EntityId id : dims)
                REQUIRE(t2.modify(
                    id, [&](Entity& e) { dim::refresh(doc1, *entityAs<DimensionData>(e)); }));
            t2.commit();
        }
        Report exportReport;
        Result<ExportResult> out = exportDxf(doc1, ExportOptions{}, exportReport);
        REQUIRE(out.ok());
        Result<ImportResult> second = importBytes(toBytes(out.value().bytes));
        REQUIRE(second.ok());
        const Document& doc2 = *second.value().document;
        CHECK(doc2.header().units == LengthUnit::Meter);
        std::string diff =
            testing::SemanticDump::diff(testing::SemanticDump(doc1), testing::SemanticDump(doc2));
        CHECK_MESSAGE(diff.empty(), diff);
        ++done;
    }
    CHECK(done >= 15);
}

}  // namespace tbcad::dxf
