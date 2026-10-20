// End-to-end smoke test: open a DXF, draw, edit, hatch, save .tbcad, reload, undo everything.
// Runs headless through ScriptHost with every command module on main. Commands that belong to
// workstreams not merged yet are exercised only when they are registered.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
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

    // 5. The commands of the later workstreams are real steps in the room-plan test below.
    for (const char* name : {"DIMLINEAR", "PLOT", "OFFSET", "TRIM"})
        CHECK(reg.find(name) != nullptr);

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

TEST_CASE("end to end: room plan with OFFSET, TRIM, DIMLINEAR, PDF, save and DXF") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    // Four walls of a 6 m x 4 m room, drawn as lines.
    REQUIRE(host.run("LINE", {"0,0", "6000,0", "6000,4000", "0,4000", "C"}).finished);
    auto lines = [&] {
        std::vector<EntityId> out;
        doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Line)
                out.push_back(e.common.id);
        });
        return out;
    };
    REQUIRE(lines().size() == 4);
    auto wallAt = [&](Vec2 p) {
        for (EntityId id : lines()) {
            const auto* l = entityAs<LineData>(*doc.find(id));
            Vec2 d = l->b - l->a;
            double t = dot(p - l->a, d) / lengthSquared(d);
            if (t >= 0 && t <= 1 && distance(l->a + d * t, p) < 1e-6)
                return id;
        }
        return EntityId{};
    };

    // OFFSET: a partition 2 m inside the left wall.
    EntityId left = wallAt({0, 2000});
    REQUIRE(left != EntityId{});
    REQUIRE(host.run("OFFSET",
                     {"2000", ScriptItem::pick(left, {0, 2000}), "100,2000", ScriptItem::enter()})
                .finished);
    CHECK(lines().size() == 5);
    bool partition = false;
    for (EntityId id : lines()) {
        const auto* l = entityAs<LineData>(*doc.find(id));
        if (std::abs(l->a.x - 2000) < 1e-6 && std::abs(l->b.x - 2000) < 1e-6)
            partition = true;
    }
    CHECK(partition);

    // TRIM: a door opening in the bottom wall between two cutting lines.
    REQUIRE(host.run("LINE", {"2500,-100", "2500,100", ScriptItem::enter()}).finished);
    REQUIRE(host.run("LINE", {"3500,-100", "3500,100", ScriptItem::enter()}).finished);
    EntityId bottom = wallAt({4500, 0});
    REQUIRE(bottom != EntityId{});
    const std::size_t before = lines().size();
    REQUIRE(host.run("TRIM", {ScriptItem::enter(), ScriptItem::pick(bottom, {3000, 0}),
                              ScriptItem::enter()})
                .finished);
    CHECK(lines().size() == before + 1);  // the wall became two pieces
    CHECK(wallAt({3000, 0}) == EntityId{});

    // DIMLINEAR: the room width.
    REQUIRE(host.run("DIMLINEAR", {"0,0", "6000,0", "3000,-600"}).finished);
    const DimensionData* dim = nullptr;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e))
            dim = d;
    });
    REQUIRE(dim);
    CHECK(dim->kind == DimKind::Linear);
    CHECK(dim->measurement == doctest::Approx(6000));

    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_room";
    std::filesystem::create_directories(dir);

    // PLOT to a PDF at 1:100.
    const auto pdf = (dir / "room.pdf").string();
    REQUIRE(host.run("EXPORTPDF", {"Scale", "1:100", "", pdf}).finished);
    REQUIRE(std::filesystem::exists(pdf));
    auto pdfBytes = readBytes(pdf);
    REQUIRE(pdfBytes.size() > 100);
    CHECK(static_cast<char>(pdfBytes[0]) == '%');
    CHECK(static_cast<char>(pdfBytes[1]) == 'P');

    // Save and reload: the dimension and the edited walls survive.
    const auto file = dir / "room.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    // DXF export and re-import keep the dimension and its measurement.
    const auto dxfFile = (dir / "room.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(imported.ok());
    CHECK(countKind(*imported.value().document, EntityKind::Dimension) == 1);
    CHECK(countKind(*imported.value().document, EntityKind::Line) == lines().size());

    // One undo per command, back to an empty drawing.
    while (host.undo()) {
    }
    CHECK(doc.modelSpaceCount() == 0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("end to end: current colour and lineweight reach what every drawing command creates") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    REQUIRE(
        host.run("-CURRENT", {"Color", "red", "Lineweight", "0.5", ScriptItem::enter()}).finished);
    const Color red = doc.header().currentColor;
    const LineWeight half = doc.header().currentLineweight;
    REQUIRE(red != Color::byLayer());
    REQUIRE(half != LineWeight::byLayer());

    REQUIRE(host.run("LINE", {"0,0", "100,0", "100,60", "0,60", "C"}).finished);
    REQUIRE(host.run("DONUT", {"5", "10", "200,0", ScriptItem::enter()}).finished);
    REQUIRE(host.run("TEXT", {"0,100", "10", "0", "TBCAD"}).finished);
    REQUIRE(host.run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    REQUIRE(host.run("HATCH", {"50,30", ScriptItem::enter()}).finished);

    std::set<EntityKind> seen;
    doc.forEachEntity([&](const Entity& e) {
        seen.insert(kind(e));
        INFO("entity kind " << static_cast<int>(kind(e)));
        CHECK(e.common.color == red);
        CHECK(e.common.lineweight == half);
    });
    // Lines, a ring, text, a dimension and a hatch were all created.
    CHECK(seen.size() >= 5);
}

TEST_CASE("end to end: table with merged title, save, reload, DXF export and re-import") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(host.run("TABLE", {"3", "3", "40", "10", "Door schedule", "0,0"}).finished);
    EntityId id{};
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Table)
            id = e.common.id;
    });
    REQUIRE(id != EntityId{});
    // The title row is row 0; click the first data cell and type, then a second cell.
    REQUIRE(host.run("TABLEEDIT", {ScriptItem::pick(id, {20, -15}), "20,-15", "D1", "60,-15",
                                   "Oak 900", ScriptItem::enter()})
                .finished);

    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_table";
    std::filesystem::create_directories(dir);
    const auto file = dir / "table.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    const auto dxfFile = (dir / "table.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(imported.ok());
    const Document& d2 = *imported.value().document;
    CHECK(countKind(d2, EntityKind::Table) == 1);
    CHECK(countKind(d2, EntityKind::Line) == 0);  // the drawn grid stays in the file's *T block
    CHECK(canonicalDump(d2).find("Oak 900") != std::string::npos);
    CHECK(canonicalDump(d2).find("Door schedule") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("end to end: annotative text and table keep their flag through .tbcad and DXF") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(host.run("TEXT", {"0,0", "2.5", "0", "Note"}).finished);
    REQUIRE(host.run("TABLE", {"2", "2", "40", "10", "", "0,-20"}).finished);
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    REQUIRE(ids.size() == 2);
    REQUIRE(host.run("ANNOTATIVE", {ScriptItem::pick(ids[0], {0, 0}),
                                    ScriptItem::pick(ids[1], {20, -25}), ScriptItem::enter(), "On"})
                .finished);
    host.setSelection({ids[0]});
    REQUIRE(host.run("ANNOTATIVE", {"Scales", "50,100"}).finished);
    REQUIRE(host.run("CANNOSCALE", {"50"}).finished);
    auto annotatives = [](const Document& d) {
        int n = 0;
        d.forEachEntity([&](const Entity& e) { n += e.common.annotative ? 1 : 0; });
        return n;
    };
    CHECK(annotatives(doc) == 2);

    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_anno";
    std::filesystem::create_directories(dir);
    const auto file = dir / "anno.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(annotatives(*back.value()) == 2);
    CHECK(currentAnnotationScale(back.value()->header()) == doctest::Approx(50));

    const auto dxfFile = (dir / "anno.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(imported.ok());
    CHECK(annotatives(*imported.value().document) == 2);
    // The scales an object is made for survive both formats.
    auto scalesOf = [&](const Document& d) {
        std::size_t n = 0;
        d.forEachEntity([&](const Entity& e) { n += e.common.annoScales.size(); });
        return n;
    };
    CHECK(scalesOf(doc) == 2);
    CHECK(scalesOf(*back.value()) == 2);
    CHECK(scalesOf(*imported.value().document) == 2);
    // The current scale and the scale list travel in DXF too, as SCALE objects for R2007.
    CHECK(currentAnnotationScale(imported.value().document->header()) == doctest::Approx(50));
    REQUIRE(host.run("SCALELISTEDIT", {"Add", "40", "Done"}).finished);
    const auto dxf2007 = (dir / "anno2007.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxf2007, "R2007"}).finished);
    dxf::Report report2;
    auto again = dxf::importDxf(readBytes(dxf2007), {}, report2);
    REQUIRE(again.ok());
    const Header& h2 = again.value().document->header();
    CHECK(currentAnnotationScale(h2) == doctest::Approx(50));
    auto list2 = annotationScaleList(h2);
    CHECK(std::find(list2.begin(), list2.end(), 40.0) != list2.end());
    {
        auto bytes = readBytes(dxf2007);
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        CHECK(text.find("ACAD_SCALELIST") != std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("end to end: a wipeout keeps hiding what is behind it through .tbcad and DXF") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    REQUIRE(host.run("WIPEOUT", {"Rectangle", "0,0", "40,20"}).finished);
    auto wipeouts = [](const Document& d) {
        int n = 0;
        d.forEachEntity([&](const Entity& e) { n += e.common.wipeout ? 1 : 0; });
        return n;
    };
    CHECK(wipeouts(doc) == 1);

    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_wipeout";
    std::filesystem::create_directories(dir);
    const auto file = dir / "w.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(wipeouts(*back.value()) == 1);
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    const auto dxfFile = (dir / "w.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(imported.ok());
    CHECK(wipeouts(*imported.value().document) == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("end to end: rays, xlines and multilines survive .tbcad and DXF round trips") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    REQUIRE(host.run("RAY", {"10,10", "50,10", ""}).finished);
    REQUIRE(host.run("XLINE", {"0,0", "100,100", ""}).finished);
    REQUIRE(host.run("-MLSTYLE", {"New", "WALL", "1 0 -1", "Both"}).finished);
    REQUIRE(host.run("-MLSTYLE", {"Current", "WALL"}).finished);
    REQUIRE(host.run("MLINE", {"0,0", "200,0", "200,100", ""}).finished);
    CHECK(countKind(doc, EntityKind::Ray) == 2);
    CHECK(countKind(doc, EntityKind::MLine) == 1);

    auto dir = std::filesystem::temp_directory_path() / "tbcad_e2e_ray";
    std::filesystem::create_directories(dir);
    const auto file = dir / "ray.tbcad";
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport ioReport;
    auto back = io::readTbcad(file, ioReport);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));

    const auto dxfFile = (dir / "ray.dxf").string();
    REQUIRE(host.run("EXPORTDXF", {dxfFile, "R2000"}).finished);
    dxf::Report report;
    auto imported = dxf::importDxf(readBytes(dxfFile), {}, report);
    REQUIRE(imported.ok());
    const Document& d2 = *imported.value().document;
    CHECK(countKind(d2, EntityKind::Ray) == 2);
    CHECK(countKind(d2, EntityKind::MLine) == 1);
    // Entities and the multiline style come back as they went out (the header differs in
    // DXF bookkeeping only).
    auto entityLines = [](const Document& d) {
        std::string out, line;
        std::istringstream in(canonicalDump(d));
        while (std::getline(in, line))
            if (line.rfind("e", 0) == 0 || line.find("mlstyle.") != std::string::npos)
                out += line + "\n";
        return out;
    };
    CHECK(entityLines(d2) == entityLines(doc));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
