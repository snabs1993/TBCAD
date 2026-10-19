// DXF -> document -> .tbcad -> document -> DXF over the corpus, the sample drawing and the block
// library: saving as .tbcad and opening again must lose nothing, and exporting the reopened
// document must give the same DXF as exporting the original.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

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

namespace tbcad::dxf {

TEST_CASE("an entity type without a reader is written back as it was, until it is moved") {
    const std::string src =
        "  0\nSECTION\n  2\nENTITIES\n"
        "  0\n3DSOLID\n  5\nAB\n330\n1F\n100\nAcDbEntity\n  8\nWALLS\n 62\n     3\n"
        "100\nAcDbModelerGeometry\n 70\n     1\n  1\nSOLID DATA \xE0\xB8\x81\n  3\nmore data\n"
        "310\n0A0B0C\n 40\n2.5\n350\n77\n1001\nMYAPP\n1000\nkept xdata\n"
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());
    Document& doc = *first.value().document;
    REQUIRE(doc.modelSpace().size() == 1);

    auto exportText = [&](const Document& d) {
        Report r;
        Result<ExportResult> out = exportDxf(d, ExportOptions{}, r);
        REQUIRE(out.ok());
        return out.value().bytes;
    };

    auto saved = io::writeTbcadToMemory(doc);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());

    const std::string text = exportText(*reopened.value());
    const std::size_t at = text.find("\r\n3DSOLID\r\n");
    REQUIRE(at != std::string::npos);
    const std::string rec = text.substr(at, text.find("\r\n0\r\n", at + 4) - at);
    CHECK(rec.find("\r\n8\r\nWALLS\r\n") != std::string::npos);
    CHECK(rec.find("\r\n100\r\nAcDbModelerGeometry\r\n") != std::string::npos);
    CHECK(rec.find("\r\n1\r\nSOLID DATA \\U+0E01\r\n") != std::string::npos);
    CHECK(rec.find("\r\n3\r\nmore data\r\n") != std::string::npos);
    CHECK(rec.find("\r\n310\r\n0A0B0C\r\n") != std::string::npos);
    CHECK(rec.find("\r\n40\r\n2.5\r\n") != std::string::npos);
    CHECK(rec.find("kept xdata") != std::string::npos);
    CHECK(rec.find("\r\n350\r\n77\r\n") == std::string::npos);  // handles are the source file's

    // Written once, and a second generation is the same record.
    CHECK(text.find("\r\n3DSOLID\r\n") == text.rfind("\r\n3DSOLID\r\n"));
    Report r3;
    Result<ImportResult> again = importDxf(std::as_bytes(std::span(text)), ImportOptions{}, r3);
    REQUIRE(again.ok());
    const std::string text2 = exportText(*again.value().document);
    const std::size_t at2 = text2.find("\r\n3DSOLID\r\n");
    REQUIRE(at2 != std::string::npos);
    CHECK(text2.substr(at2, text2.find("\r\n0\r\n", at2 + 4) - at2).find("\r\n310\r\n0A0B0C\r\n") !=
          std::string::npos);

    // Moved: the groups no longer fit, so the entity is no longer written as it was.
    Document& d = *reopened.value();
    {
        auto tx = d.begin("move");
        REQUIRE(tx.modify(d.modelSpace()[0], [](Entity& e) {
                      staleProxyDxf(*entityAs<ProxyData>(e));
                  }).ok());
        tx.commit();
    }
    CHECK(exportText(d).find("\r\n3DSOLID\r\n") == std::string::npos);
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {

TEST_CASE("objects of other programs hang on under their dictionary with fresh handles") {
    const std::string src =
        "  0\nSECTION\n  2\nENTITIES\n  0\nLINE\n  5\nE1\n  8\n0\n 10\n0\n 20\n0\n 11\n5\n 21\n5\n"
        "  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n"
        "  0\nDICTIONARY\n  5\nC\n330\n0\n100\nAcDbDictionary\n281\n     1\n"
        "  3\nACAD_GROUP\n350\nD1\n  3\nMYAPP_DATA\n350\nA1\n  3\nACAD_MATERIAL\n350\nB1\n"
        "  0\nDICTIONARY\n  5\nD1\n330\nC\n100\nAcDbDictionary\n281\n     1\n"
        "  0\nDICTIONARY\n  "
        "5\nA1\n102\n{ACAD_REACTORS\n330\nC\n102\n}\n330\nC\n100\nAcDbDictionary\n"
        "281\n     1\n  3\nSETTINGS\n350\nA2\n"
        "  0\nXRECORD\n  5\nA2\n330\nA1\n100\nAcDbXrecord\n280\n     1\n  1\nvalue \xE0\xB8\x81\n"
        " 70\n     7\n340\nE1\n"
        "  0\nDICTIONARY\n  5\nB1\n330\nC\n100\nAcDbDictionary\n281\n     1\n  3\nSteel\n350\nB2\n"
        "  0\nMATERIAL\n  5\nB2\n330\nB1\n100\nAcDbMaterial\n  1\nSteel\n 43\n0.25\n"
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());

    auto saved = io::writeTbcadToMemory(*first.value().document);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;

    // the groups of the output, as code/value pairs
    std::vector<std::pair<int, std::string>> g;
    {
        std::istringstream in(text);
        std::string a, b;
        while (std::getline(in, a) && std::getline(in, b)) {
            while (!a.empty() && (a.back() == '\r' || a.back() == ' '))
                a.pop_back();
            while (!b.empty() && b.back() == '\r')
                b.pop_back();
            g.emplace_back(std::atoi(a.c_str()), b);
        }
    }
    auto record = [&](const std::string& type) {
        std::vector<std::pair<int, std::string>> rec;
        for (std::size_t i = 0; i < g.size(); ++i)
            if (g[i].first == 0 && g[i].second == type) {
                for (++i; i < g.size() && g[i].first != 0; ++i)
                    rec.push_back(g[i]);
                break;
            }
        return rec;
    };
    auto find = [](const std::vector<std::pair<int, std::string>>& rec, int code) {
        for (const auto& [c, v] : rec)
            if (c == code)
                return v;
        return std::string("<none>");
    };

    const auto xr = record("XRECORD");
    REQUIRE(!xr.empty());
    CHECK(find(xr, 1) == "value \\U+0E01");
    CHECK(find(xr, 70) == "7");
    CHECK(find(xr, 340) == "E1");  // the line is still there and keeps its handle
    const auto mat = record("MATERIAL");
    REQUIRE(!mat.empty());
    CHECK(find(mat, 43) == "0.25");

    // each handle is defined once, and every pointer finds its target
    std::set<std::string> defined;
    for (const auto& [c, v] : g)
        if (c == 5)
            CHECK_MESSAGE(defined.insert(v).second, "handle " << v << " defined twice");
    for (const auto& [c, v] : g)
        if (c >= 330 && c <= 369 && v != "0")
            CHECK_MESSAGE(defined.count(v) > 0, "dangling pointer " << v);

    // the xrecord is owned by the dictionary under MYAPP_DATA, which hangs on the root
    std::string myapp;
    for (std::size_t i = 0; i + 1 < g.size(); ++i)
        if (g[i].first == 3 && g[i].second == "MYAPP_DATA")
            myapp = g[i + 1].second;
    CHECK(!myapp.empty());
    CHECK(find(xr, 330) != "<none>");
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {

TEST_CASE("entities keep the handles they had, new ones get handles that clash with none") {
    const std::string src =
        "  0\nSECTION\n  2\nENTITIES\n"
        "  0\nLINE\n  5\n2B\n  8\n0\n 10\n0\n 20\n0\n 11\n5\n 21\n5\n"
        "  0\nCIRCLE\n  5\n21\n  8\n0\n 10\n0\n 20\n0\n 40\n3\n"
        "  0\nLINE\n  5\n2B\n  8\n0\n 10\n1\n 20\n1\n 11\n6\n 21\n6\n"  // a second 2B
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());
    Document& doc = *first.value().document;
    {
        auto tx = doc.begin("add");
        Entity e;
        e.data = LineData{{0, 0}, {1, 1}};
        e.common.layer = kLayerZero;
        REQUIRE(tx.add(std::move(e)).ok());
        tx.commit();
    }
    auto saved = io::writeTbcadToMemory(doc);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;

    std::vector<std::pair<int, std::string>> g;
    {
        std::istringstream in(text);
        std::string a, b;
        while (std::getline(in, a) && std::getline(in, b)) {
            while (!a.empty() && (a.back() == '\r' || a.back() == ' '))
                a.pop_back();
            while (!b.empty() && b.back() == '\r')
                b.pop_back();
            g.emplace_back(std::atoi(a.c_str()), b);
        }
    }
    // handles of the entities, in order, within the ENTITIES section
    std::vector<std::pair<std::string, std::string>> ents;  // (type, handle)
    bool inEntities = false;
    for (std::size_t i = 0; i + 1 < g.size(); ++i) {
        if (g[i].first == 2 && g[i].second == "ENTITIES")
            inEntities = true;
        else if (inEntities && g[i].first == 0 && g[i].second == "ENDSEC")
            break;
        else if (inEntities && g[i].first == 0 && g[i + 1].first == 5)
            ents.emplace_back(g[i].second, g[i + 1].second);
    }
    REQUIRE(ents.size() == 4);
    CHECK(ents[0] == std::make_pair(std::string("LINE"), std::string("2B")));
    CHECK(ents[1] == std::make_pair(std::string("CIRCLE"), std::string("21")));
    CHECK(ents[2].second != "2B");  // the second 2B is a new handle
    CHECK(ents[3].second != "2B");

    std::set<std::string> defined;
    bool pastHeader = false;
    for (const auto& [c, v] : g) {
        if (c == 0 && v == "ENDSEC")
            pastHeader = true;  // (the header's own group 5 is the handle seed)
        if (c == 5 && pastHeader)
            CHECK_MESSAGE(defined.insert(v).second, "handle " << v << " defined twice");
    }
    // $HANDSEED is above every handle
    std::uint64_t seed = 0, top = 0;
    for (std::size_t i = 0; i + 1 < g.size(); ++i)
        if (g[i].first == 9 && g[i].second == "$HANDSEED")
            seed = std::strtoull(g[i + 1].second.c_str(), nullptr, 16);
    for (const auto& h : defined)
        top = std::max<std::uint64_t>(top, std::strtoull(h.c_str(), nullptr, 16));
    CHECK(seed > top);
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {

TEST_CASE("the page setup of a layout is written back while its paper size is the same") {
    const std::string src =
        "  0\nSECTION\n  2\nBLOCKS\n"
        "  0\nBLOCK\n  2\n*Paper_Space\n 70\n     0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nLINE\n  8\n0\n 67\n     1\n 10\n5\n 20\n5\n 11\n100\n 21\n5\n"
        "  0\nENDBLK\n"
        "  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n"
        "  0\nLAYOUT\n  5\nA0\n100\nAcDbPlotSettings\n  1\nSetup A1\n  2\nDWG To PDF.pc3\n"
        "  4\nISO full bleed A1 (841.00 x 594.00 MM)\n  6\n\n 40\n0.0\n 41\n0.0\n 42\n0.0\n"
        " 43\n0.0\n 44\n841.0\n 45\n594.0\n 70\n  1024\n 73\n     1\n  7\n_EP-100.ctb\n"
        " 75\n    16\n147\n1.0\n"
        "100\nAcDbLayout\n  1\nSheet A1\n 70\n     1\n 71\n     1\n"
        "  0\nLAYOUT\n  5\nA1\n100\nAcDbPlotSettings\n  1\n\n  2\nnone_device\n"
        "100\nAcDbLayout\n  1\nModel\n 70\n     1\n 71\n     0\n"
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());
    auto saved = io::writeTbcadToMemory(*first.value().document);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    CHECK(text.find("DWG To PDF.pc3") != std::string::npos);
    CHECK(text.find("_EP-100.ctb") != std::string::npos);
    CHECK(text.find("\r\nSetup A1\r\n") != std::string::npos);
    CHECK(text.find("none_device") != std::string::npos);  // the model layout has no setup kept
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {

TEST_CASE("unit precision comes in from a DXF and goes out with the value set in TBCAD") {
    const std::string src =
        "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1027\n"
        "  9\n$LUPREC\n 70\n     2\n  9\n$AUPREC\n 70\n     3\n"
        "  0\nENDSEC\n  0\nSECTION\n  2\nENTITIES\n  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> in = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(in.ok());
    Document& doc = *in.value().document;
    CHECK(linearPrecision(doc.header()) == 2);
    CHECK(anglePrecision(doc.header()) == 3);
    {
        Transaction tx = doc.begin("units");
        REQUIRE(tx.modifyHeader([](Header& h) { setLinearPrecision(h, 6); }));
        tx.commit();
    }
    Report r2;
    Result<ExportResult> out = exportDxf(doc, ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string dxf(reinterpret_cast<const char*>(out.value().bytes.data()),
                          out.value().bytes.size());
    CHECK(headerVar(dxf, "$LUPREC") == "\r\n9\r\n$LUPREC\r\n70\r\n6");
    CHECK(headerVar(dxf, "$AUPREC").find("3") != std::string::npos);
    CHECK(dxf.find("$LUPREC") == dxf.rfind("$LUPREC"));
    // A new drawing has the defaults.
    Document fresh;
    CHECK(linearPrecision(fresh.header()) == 4);
    CHECK(anglePrecision(fresh.header()) == 0);
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {

// AutoCAD keeps an INSERT placed further away than TBCAD can draw (1e10 mm) and an LWPOLYLINE
// with a single vertex. TBCAD cannot show them, but keeps their records and writes them back.
TEST_CASE("an object beyond the coordinate limit and a one-vertex polyline are saved back") {
    const std::string src =
        "  0\nSECTION\n  2\nBLOCKS\n"
        "  0\nBLOCK\n  8\n0\n  2\nFAR\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n  3\nFAR\n"
        "  0\nLINE\n  8\n0\n 10\n0\n 20\n0\n 30\n0\n 11\n1\n 21\n0\n 31\n0\n"
        "  0\nENDBLK\n  8\n0\n"
        "  0\nENDSEC\n"
        "  0\nSECTION\n  2\nENTITIES\n"
        "  0\nINSERT\n  8\nMARKS\n  2\nFAR\n 10\n3.5E+13\n 20\n-2E+12\n 30\n0\n 41\n2\n"
        "  0\nLWPOLYLINE\n  8\nDOTS\n 90\n1\n 70\n0\n 10\n12.5\n 20\n7.5\n"
        "  0\nLINE\n  8\n0\n 10\n0\n 20\n0\n 30\n0\n 11\n10\n 21\n0\n 31\n0\n"
        "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(src.size());
    std::memcpy(bytes.data(), src.data(), src.size());
    Report r1;
    Result<ImportResult> first = importDxf(bytes, ImportOptions{}, r1);
    REQUIRE(first.ok());
    Document& doc = *first.value().document;
    CHECK(doc.modelSpace().size() == 3);
    CHECK(first.value().summary.beyondHardCap == 1);
    CHECK(r1.countCode(code::kBeyondHardCap) == 1);

    auto saved = io::writeTbcadToMemory(doc);
    REQUIRE(saved.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(saved.value(), ioReport);
    REQUIRE(reopened.ok());
    Report r2;
    Result<ExportResult> out = exportDxf(*reopened.value(), ExportOptions{}, r2);
    REQUIRE(out.ok());
    const std::string text(out.value().bytes.begin(), out.value().bytes.end());
    // The text of a record, up to the type of the one written after it.
    auto record = [&](const char* type, const char* next) {
        const std::size_t at = text.find(std::string("\r\n") + type + "\r\n", text.find("ENTITIES"));
        REQUIRE(at != std::string::npos);
        return text.substr(at, text.find(std::string("\r\n") + next + "\r\n", at) - at);
    };
    const std::string ins = record("INSERT", "LWPOLYLINE");
    CHECK(ins.find("\r\n8\r\nMARKS\r\n") != std::string::npos);
    CHECK(ins.find("\r\n2\r\nFAR\r\n") != std::string::npos);
    CHECK(ins.find("\r\n10\r\n35000000000000\r\n") != std::string::npos);
    CHECK(ins.find("\r\n41\r\n2\r\n") != std::string::npos);
    const std::string pl = record("LWPOLYLINE", "LINE");
    CHECK(pl.find("\r\n8\r\nDOTS\r\n") != std::string::npos);
    CHECK(pl.find("\r\n90\r\n1\r\n") != std::string::npos);
    CHECK(pl.find("\r\n10\r\n12.5\r\n") != std::string::npos);

    // Read again: the same objects, still kept.
    Report r3;
    Result<ImportResult> again = importDxf(std::as_bytes(std::span(text)), ImportOptions{}, r3);
    REQUIRE(again.ok());
    CHECK(again.value().document->modelSpace().size() == 3);
}

}  // namespace tbcad::dxf
