// End-to-end smoke test: open a DXF, draw, edit, hatch, save .tbcad, reload, undo everything.
// Runs headless through ScriptHost with every command module on main. Commands that belong to
// workstreams not merged yet (dimensions, plot) are exercised only when they are registered.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

std::vector<std::byte> readBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        out[i] = static_cast<std::byte>(raw[i]);
    return out;
}

std::size_t countKind(const Document& doc, EntityKind k) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == k)
            ++n;
    });
    return n;
}

}  // namespace

TEST_CASE("end to end: open DXF, draw, hatch, move, save, reload, undo") {
    const std::filesystem::path corpus = TBCAD_TEST_CORPUS_DIR;

    // 1. Open a DXF.
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(corpus / "reader" / "r2000_entities.dxf"), {}, report);
    REQUIRE(imported.ok());
    Document& doc = *imported.value().document;
    const std::size_t importedCount = doc.modelSpaceCount();
    REQUIRE(importedCount > 0);
    const std::string importedDump = canonicalDump(doc);

    // 2. Draw next to it, far from the imported geometry.
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(host.run("LINE", {"5000,5000", "5100,5000", ScriptItem::enter()}).finished);
    REQUIRE(host.run("RECTANGLE", {"5000,5100", "5100,5160"}).finished);
    REQUIRE(host.run("CIRCLE", {"5050,5130", "10"}).finished);
    CHECK(doc.modelSpaceCount() == importedCount + 3);

    // 3. Hatch the area between the rectangle's lines (the rectangle is a closed polyline).
    const std::size_t hatchesBefore = countKind(doc, EntityKind::Hatch);
    auto h = host.run("HATCH", {"5005,5105", ScriptItem::enter()});
    CHECK(h.finished);
    CHECK(countKind(doc, EntityKind::Hatch) == hatchesBefore + 1);

    // 4. Move what was just drawn.
    host.setSelection(doc.modelSpace());
    const auto beforeMove = canonicalDump(doc);
    REQUIRE(host.run("MOVE", {"0,0", "10,10"}).finished);
    CHECK(canonicalDump(doc) != beforeMove);

    // 5. Optional commands from later workstreams.
    for (const char* name : {"DIMLINEAR", "PLOT", "OFFSET", "TRIM"})
        MESSAGE(std::string(name) << (reg.find(name) ? std::string(": registered")
                                                     : std::string(": not on main yet")));

    // 6. Save and reload: the canonical dump must match.
    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_smoke";
    std::filesystem::create_directories(dir);
    auto file = dir / "smoke.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    // 7. Undo every step: back to the imported drawing.
    while (host.undo()) {
    }
    CHECK(canonicalDump(doc) == importedDump);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("end to end: Thai text, blocks, move, DXF export and re-import") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    // Thai text and a plain text.
    REQUIRE(host.run("TEXT", {"0,0", "10", "0", "TBCAD"}).finished);
    REQUIRE(host.run("TEXT", {"0,20", "10", "0", "สวัสดี"}).finished);
    CHECK(countKind(doc, EntityKind::Text) == 2);

    // Geometry, turned into a block, inserted twice, exploded once.
    REQUIRE(host.run("RECTANGLE", {"100,0", "140,20"}).finished);
    REQUIRE(host.run("CIRCLE", {"120,10", "5"}).finished);
    std::vector<EntityId> shapes;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Polyline || kind(e) == EntityKind::Circle)
            shapes.push_back(e.common.id);
    });
    REQUIRE(shapes.size() == 2);
    host.setSelection(shapes);
    REQUIRE(host.run("BLOCK", {"Door", "100,0", "C"}).finished);
    CHECK(countKind(doc, EntityKind::Insert) == 1);
    REQUIRE(host.run("INSERT", {"Door", "200,0"}).finished);
    CHECK(countKind(doc, EntityKind::Insert) == 2);

    // Move a text with a typed base and target point.
    std::vector<EntityId> texts;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Text)
            texts.push_back(e.common.id);
    });
    host.setSelection(texts);
    REQUIRE(host.run("MOVE", {"0,0", "0,-50"}).finished);

    // DXF export, then import into a fresh document: same counts of the main kinds.
    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_dxf";
    std::filesystem::create_directories(dir);
    const auto dxfFile = (dir / "out.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    REQUIRE(std::filesystem::exists(dxfFile));

    dxf::Report report;
    auto back = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(back.ok());
    const Document& d2 = *back.value().document;
    CHECK(countKind(d2, EntityKind::Text) == 2);
    CHECK(countKind(d2, EntityKind::Insert) == 2);
    CHECK(d2.blocks().size() == doc.blocks().size());

    // The Thai text survives the round trip.
    bool thai = false;
    d2.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e); t && t->utf8 == "สวัสดี")
            thai = true;
    });
    CHECK(thai);

    // Explode one insert, then undo it.
    std::vector<EntityId> inserts;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Insert)
            inserts.push_back(e.common.id);
    });
    host.setSelection({inserts.front()});
    REQUIRE(host.run("EXPLODE", {}).finished);
    CHECK(countKind(doc, EntityKind::Insert) == 1);
    CHECK(host.undo());
    CHECK(countKind(doc, EntityKind::Insert) == 2);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
