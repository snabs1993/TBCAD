#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/document.h"

using namespace tbcad;

namespace {

std::filesystem::path tempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void drawSomething(Document& doc) {
    auto tx = doc.begin("setup");
    Layer l;
    l.name = "S-FLOOR";
    auto parent = tx.addLayer(l);
    REQUIRE(parent.ok());
    Layer c;
    c.name = "edge";
    c.parent = parent.value();
    auto child = tx.addLayer(c);
    REQUIRE(child.ok());
    Entity e{{}, LineData{{0, 0}, {100, 0}}};
    e.common.layer = child.value();
    REQUIRE(tx.add(e).ok());
    Entity circle{{}, CircleData{{50, 50}, 25}};
    REQUIRE(tx.add(circle).ok());
    tx.commit();
}

}  // namespace

TEST_CASE("EXPORTDXF writes a DXF file and IMPORTDXF reads it back as one undo step") {
    const auto file = tempFile("tbcad_dxfexport_test.dxf");
    std::filesystem::remove(file);

    Document src;
    drawSomething(src);
    cmd::Registry reg;
    registerAllCommands(reg);
    REQUIRE(reg.find("EXPORTDXF") != nullptr);
    REQUIRE(reg.find("IMPORTDXF") != nullptr);
    {
        cmd::ScriptHost host(src, reg);
        auto r = host.run("EXPORTDXF", {file.string(), ""});
        CHECK(r.finished);
        CHECK_FALSE(host.undoStack().canUndo());  // exporting changes nothing
    }
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) > 500);

    Document dst;
    cmd::ScriptHost host(dst, reg);
    auto r = host.run("IMPORTDXF", {file.string()});
    CHECK(r.finished);
    CHECK(dst.entityCount() == 2);
    const Layer* parent = dst.layerByName("S-FLOOR");
    REQUIRE(parent != nullptr);
    CHECK(dst.layerByName(parent->id, "edge") != nullptr);

    CHECK(host.undo());
    CHECK(dst.entityCount() == 0);
    CHECK(host.redo());
    CHECK(dst.entityCount() == 2);
    std::filesystem::remove(file);
}

TEST_CASE("EXPORTDXF takes a version and Esc leaves no file") {
    const auto file = tempFile("tbcad_dxfexport_r12.dxf");
    std::filesystem::remove(file);
    Document doc;
    drawSomething(doc);
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);

    host.run("EXPORTDXF", {file.string(), cmd::ScriptItem::esc()});
    CHECK_FALSE(std::filesystem::exists(file));

    host.run("EXPORTDXF", {file.string(), "R12"});
    std::ifstream f(file, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().find("AC1009") != std::string::npos);
    f.close();
    std::filesystem::remove(file);
}

TEST_CASE("IMPORTDXF reports a missing file and changes nothing") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    auto r = host.run("IMPORTDXF", {"/nonexistent/none.dxf"});
    CHECK(r.finished);
    CHECK(doc.entityCount() == 0);
    CHECK_FALSE(host.undoStack().canUndo());
}
