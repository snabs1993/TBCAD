// DXF is the only format TBCAD saves drawings in: a drawing saved as DXF and opened again must keep
// everything TBCAD stores about it, also the data no other program knows (plot frames, paper
// names, settings a later version adds), and leave out what only describes this session.
#include <cstring>
#include <filesystem>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::dxf {
namespace {

std::unique_ptr<Document> saveAndOpen(const Document& doc) {
    Report r1;
    auto out = exportDxf(doc, ExportOptions{}, r1);
    REQUIRE(out.ok());
    const std::string& s = out.value().bytes;
    std::vector<std::byte> bytes(s.size());
    std::memcpy(bytes.data(), s.data(), s.size());
    Report r2;
    auto back = importDxf(bytes, ImportOptions{}, r2);
    REQUIRE(back.ok());
    return std::move(back.value().document);
}

std::unique_ptr<Document> sample() {
    io::IoReport ioReport;
    auto doc =
        io::readTbcad(std::filesystem::path(TBCAD_SAMPLES_DIR) / "thai-house.tbcad", ioReport);
    REQUIRE(doc.ok());
    return std::move(doc.value());
}

}  // namespace

TEST_CASE("every TBCAD setting in the drawing survives saving as DXF and opening again") {
    std::unique_ptr<Document> doc = sample();
    // Values are kept as they are, whatever they hold (line breaks, tabs, backslashes, Thai).
    const std::pair<const char*, std::string> keys[] = {
        {"tbcad.plotframes",
         "F\tSheet 1\t0\t0\t42000\t29700\tA3\t297\t420\t1\t0\t100\t\n"
         "F\tแผ่น 2\t50000\t0\t92000\t29700\tA1\t594\t841\t1\t0\t50\t\n"},
        {"tbcad.plotsets", "set one\\two\r\nline"},
        {"tbcad.pdfframe", "2"},
        {"tbcad.xclipframe", "1"},
        {"tbcad.pickstyle", "1"},
        {"tbcad.annoautoscale", "4"},
        {"tbcad.vports.Pair", "2\tv"},
        {"tbcad.mleaderopt.Tag", "a\tb"},
        {"tbcad.some.future.setting", std::string(5000, 'x') + "end"},
        {"pdmode", "35"},
        {"sheet.number", "S-01"}};
    {
        auto tx = doc->begin("settings");
        (void)tx.modifyHeader([&](Header& h) {
            for (const auto& [k, v] : keys)
                h.custom[k] = v;
            h.custom["isolate.hidden"] = "1,2,3";  // object ids: new ones when opened again
            h.custom["tmp.bedit.block"] = "7";     // a running command's state
        });
        tx.commit();
    }
    std::unique_ptr<Document> back = saveAndOpen(*doc);
    for (const auto& [k, v] : keys) {
        INFO("key: " << k);
        auto it = back->header().custom.find(k);
        REQUIRE(it != back->header().custom.end());
        CHECK(it->second == v);
    }
    CHECK(back->header().custom.count("isolate.hidden") == 0);
    CHECK(back->header().custom.count("tmp.bedit.block") == 0);

    // Saved again, the file is the same: the settings record does not grow or move.
    Report a, b;
    auto first = exportDxf(*back, ExportOptions{}, a);
    std::unique_ptr<Document> again = saveAndOpen(*back);
    auto second = exportDxf(*again, ExportOptions{}, b);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.value().bytes == second.value().bytes);
}

TEST_CASE("the sample drawing saved as DXF looks the same opened again, paper names included") {
    std::unique_ptr<Document> doc = sample();
    std::unique_ptr<Document> back = saveAndOpen(*doc);
    const std::string diff =
        testing::SemanticDump::diff(testing::SemanticDump(*doc), testing::SemanticDump(*back));
    CHECK_MESSAGE(diff.empty(), diff);
    // The layout keeps its paper name ("A3"), not only the size.
    const std::string& before = doc->header().custom.at("tbcad.layouts");
    const std::string& after = back->header().custom.at("tbcad.layouts");
    CHECK(before.find("\tA3\t") != std::string::npos);
    CHECK(after.find("\tA3\t") != std::string::npos);
    CHECK(after.find("\tCustom\t") == std::string::npos);
}

}  // namespace tbcad::dxf
