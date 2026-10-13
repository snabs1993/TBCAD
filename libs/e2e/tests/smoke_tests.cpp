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
    for (const char* name : {"DIMLINEAR", "PLOT", "BLOCK", "OFFSET", "TRIM"})
        MESSAGE(name << (reg.find(name) ? std::string(": registered")
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
