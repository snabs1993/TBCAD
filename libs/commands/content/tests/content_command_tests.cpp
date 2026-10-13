#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    (std::string("tbcad-content-") +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A drawing file with layers "walls" and "walls~inner" style rows and blocks.
fs::path writeSource(const fs::path& dir) {
    Document src;
    Transaction tx = src.begin("src");
    Layer walls;
    walls.name = "walls";
    LayerId w = tx.addLayer(walls).value();
    Layer inner;
    inner.name = "inner";
    inner.parent = w;
    (void)tx.addLayer(inner);
    TextStyle notes;
    notes.name = "Notes";
    (void)tx.addTextStyle(notes);
    BlockId door = tx.addBlock(BlockDef{.name = "door"}).value();
    Entity line;
    line.data = LineData{{0, 0}, {1, 0}};
    line.common.layer = w;
    (void)tx.add(line, door);
    BlockId gate = tx.addBlock(BlockDef{.name = "gate"}).value();
    Entity line2;
    line2.data = LineData{{0, 0}, {2, 0}};
    (void)tx.add(line2, gate);
    tx.commit();
    const fs::path file = dir / "lib.tbcad";
    REQUIRE(io::writeTbcad(src, file));
    return file;
}

struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};

}  // namespace

TEST_CASE("CONTENTIMPORT copies named blocks, with their layers, in one undo step") {
    TempDir dir;
    const fs::path file = writeSource(dir.path);
    Fixture f;
    f.host.run("CONTENTIMPORT", {file.string(), "Block", "door|gate"});
    CHECK(f.doc.blockByName("door"));
    CHECK(f.doc.blockByName("gate"));
    CHECK(f.doc.layerByName("walls"));
    CHECK(f.host.logContains("Copied 2"));
    CHECK(f.host.undo());
    CHECK(f.doc.blockByName("door") == nullptr);
    CHECK(f.doc.layerByName("walls") == nullptr);
}

TEST_CASE("CONTENTIMPORT copies all layers and styles with *, skips what exists") {
    TempDir dir;
    const fs::path file = writeSource(dir.path);
    Fixture f;
    f.host.run("CIMP", {file.string(), "Layer", "*"});
    REQUIRE(f.doc.layerByName("walls"));
    CHECK(f.doc.layerByName(f.doc.layerByName("walls")->id, "inner"));
    f.host.run("CIMP", {file.string(), "Textstyle", "Notes|Nope"});
    CHECK(f.doc.textStyleByName("Notes"));
    CHECK(f.host.logContains("Skipped Nope"));
    f.host.run("CIMP", {file.string(), "Textstyle", "Notes"});
    CHECK(f.host.logContains("already in the drawing"));
}

TEST_CASE("CONTENTIMPORT List shows what a file offers; bad paths are reported") {
    TempDir dir;
    const fs::path file = writeSource(dir.path);
    Fixture f;
    f.host.run("CONTENTIMPORT", {file.string(), "List"});
    CHECK(f.host.logContains("Block     door"));
    CHECK(f.host.logContains("Layer     walls~inner"));
    f.host.run("CONTENTIMPORT", {(dir.path / "missing.tbcad").string()});
    CHECK(f.host.logContains("Cannot find"));
    f.host.run("CONTENTIMPORT", {file.string(), "Block", "  "});
    CHECK(f.host.logContains("Nothing to copy"));
}

TEST_CASE("the content commands are registered") {
    Fixture f;
    CHECK(f.reg.find("CONTENT"));
    CHECK(f.reg.find("ADC"));
    CHECK(f.reg.find("CONTENTIMPORT"));
}
