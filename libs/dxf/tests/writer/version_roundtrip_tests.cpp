// Open, save as DXF at every version this program writes, open again: the same objects of the
// same kinds come back, with the same properties (see docs/dxf-export.md for what R12 changes).
#include <algorithm>
#include <filesystem>
#include <map>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dim/dim.h"
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

template <class T>
Entity make(T data) {
    Entity e;
    e.data = std::move(data);
    return e;
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

TEST_CASE("a drawing made here with every kind of object survives DXF R2000 and R2007") {
    Document doc;
    {
        Transaction tx = doc.begin("all kinds");
        const BlockId b = tx.addBlock(BlockDef{.name = "chair", .base = {0, 0}}).value();
        REQUIRE(tx.add(make(CircleData{{0, 0}, 2}), b).ok());
        REQUIRE(tx.add(make(LineData{{0, 0}, {10, 5}})).ok());
        REQUIRE(tx.add(make(CircleData{{20, 0}, 3})).ok());
        REQUIRE(tx.add(make(ArcData{{30, 0}, 3, 0.5, 2.5})).ok());
        PolylineData pl;
        pl.v = {{{0, 10}, 0.5, 0, 0}, {{10, 10}, 0, 1, 1}, {{10, 20}, 0, 0, 0}};
        pl.closed = true;
        REQUIRE(tx.add(make(pl)).ok());
        REQUIRE(tx.add(make(PointData{{40, 0}})).ok());
        EllipseData el;
        el.center = {50, 0};
        el.majorAxis = {6, 2};
        el.ratio = 0.4;
        REQUIRE(tx.add(make(Indirect<EllipseData>(el))).ok());
        SplineData sp;
        sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
        sp.control = {{60, 0}, {62, 4}, {66, -4}, {68, 0}};
        REQUIRE(tx.add(make(Indirect<SplineData>(sp))).ok());
        TextData t;
        t.position = t.alignPoint = {0, 30};
        t.height = 2.5;
        t.utf8 = "\u0e1a\u0e49\u0e32\u0e19 house";
        REQUIRE(tx.add(make(Indirect<TextData>(t))).ok());
        MTextData mt;
        mt.position = {0, 40};
        mt.width = 30;
        mt.utf8 = "two\nlines";
        REQUIRE(tx.add(make(Indirect<MTextData>(mt))).ok());
        HatchData h;
        h.loops.push_back({{HatchLineEdge{{0, 50}, {10, 50}}, HatchLineEdge{{10, 50}, {10, 60}},
                            HatchLineEdge{{10, 60}, {0, 50}}},
                           true});
        h.solid = false;
        h.pattern = "ANSI31";
        h.patternScale = 2;
        REQUIRE(tx.add(make(Indirect<HatchData>(h))).ok());
        REQUIRE(tx.add(make(InsertData{b, {80, 0}, {1, 1}, 0})).ok());
        InsertData arr{b, {80, 20}, {1, 1}, 0};
        ArrayInfo info;
        info.rows = 2;
        info.columns = 3;
        info.rowSpacing = 5;
        info.columnSpacing = 6;
        arr.array = info;
        REQUIRE(tx.add(make(Indirect<InsertData>(arr))).ok());
        DimensionData d;
        d.defPts = {Vec2{0, -10}, Vec2{20, -10}, Vec2{0, -15}, Vec2{}};
        d.textPos = {10, -15};
        dim::refresh(doc, d);
        REQUIRE(tx.add(make(Indirect<DimensionData>(d))).ok());
        LeaderData ld;
        ld.vertices = {{100, 0}, {110, 10}, {115, 10}};
        ld.textPos = {115, 10};
        REQUIRE(tx.add(make(Indirect<LeaderData>(ld))).ok());
        TableData tb;
        tb.origin = {120, 40};
        tb.colWidths = {20, 30};
        tb.rowHeights = {8, 8};
        tb.cells = {{"A"}, {"B"}, {"C"}, {"D"}};
        REQUIRE(tx.add(make(Indirect<TableData>(tb))).ok());
        RayData ray;
        ray.origin = {0, -50};
        ray.dir = {1, 0};
        REQUIRE(tx.add(make(Indirect<RayData>(ray))).ok());
        MlineData ml;
        ml.v = {{0, -60}, {30, -60}, {30, -40}};
        REQUIRE(tx.add(make(Indirect<MlineData>(ml))).ok());
        MLeaderData mlead;
        mlead.lines = {{{140, 0}, {150, 10}}};
        mlead.landing = {150, 10};
        mlead.contentPos = {152, 10};
        mlead.utf8 = "note";
        REQUIRE(tx.add(make(Indirect<MLeaderData>(mlead))).ok());
        tx.commit();
    }
    const auto k1 = kinds(doc);
    REQUIRE(k1.size() == 17);  // every kind but proxies, which only come from files
    for (DxfVersion v : {DxfVersion::R2000, DxfVersion::R2007}) {
        INFO("version: " << versionCode(v));
        ExportOptions opt;
        opt.version = v;
        Report exportReport;
        Result<ExportResult> out = exportDxf(doc, opt, exportReport);
        REQUIRE(out.ok());
        Report report;
        Result<ImportResult> back =
            importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, report);
        REQUIRE(back.ok());
        const Document& doc2 = *back.value().document;
        CHECK_MESSAGE(kinds(doc2) == k1, kindsText(k1) << "-> " << kindsText(kinds(doc2)));
        const std::string diff =
            testing::SemanticDump::diff(testing::SemanticDump(doc), testing::SemanticDump(doc2));
        CHECK_MESSAGE(diff.empty(), diff);
    }
}

}  // namespace tbcad::dxf
