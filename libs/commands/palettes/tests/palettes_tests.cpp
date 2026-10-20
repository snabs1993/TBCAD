#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/palettes/palettes.h"

using namespace tbcad;
using namespace tbcad::palettes;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    (std::string("tbcad-pal-") +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

TEST_CASE("a missing file gives the default palettes") {
    TempDir dir;
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    REQUIRE(store.palettes().size() == 2);
    CHECK(store.palettes()[0].name == "Draw");
    CHECK(store.warning().empty());
}

TEST_CASE("palettes save and load with every tool field") {
    TempDir dir;
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    REQUIRE(store.addPalette("Doors").empty());
    Tool t;
    t.name = "Single door";
    t.kind = ToolKind::Block;
    t.block = "door";
    t.file = "D:/lib/doors.tbcad";
    t.layer = "A-DOOR";
    t.scale = 2;
    t.angle = 90;
    REQUIRE(store.addTool("doors", t).empty());  // names are case-insensitive
    REQUIRE(store.save().empty());

    PaletteStore again(dir.path / "palettes.json");
    again.load();
    Palette* p = again.find("Doors");
    REQUIRE(p);
    REQUIRE(p->tools.size() == 1);
    const Tool& r = p->tools[0];
    CHECK(r.name == "Single door");
    CHECK(r.kind == ToolKind::Block);
    CHECK(r.block == "door");
    CHECK(r.file == "D:/lib/doors.tbcad");
    CHECK(r.layer == "A-DOOR");
    CHECK(r.scale == doctest::Approx(2));
    CHECK(r.angle == doctest::Approx(90));
}

TEST_CASE("editing: duplicates, bad input, rename, remove, move") {
    PaletteStore store(fs::path{});
    store.load();
    CHECK_FALSE(store.addPalette("draw").empty());
    CHECK_FALSE(store.addPalette("  ").empty());
    CHECK(store.renamePalette("Draw", "Basics").empty());
    CHECK_FALSE(store.renamePalette("Basics", "Hatches").empty());
    CHECK_FALSE(store.renamePalette("Nope", "X").empty());
    Tool dup;
    dup.name = "line";
    dup.command = "LINE";
    CHECK_FALSE(store.addTool("Basics", dup).empty());
    Tool noBlock;
    noBlock.name = "B";
    noBlock.kind = ToolKind::Block;
    CHECK_FALSE(store.addTool("Basics", noBlock).empty());
    CHECK_FALSE(store.addTool("Missing", dup).empty());

    CHECK(store.moveTool("Basics", "Circle", -1).empty());
    CHECK(store.find("Basics")->tools[1].name == "Circle");
    CHECK(store.moveTool("Basics", "Circle", -10).empty());
    CHECK(store.find("Basics")->tools[0].name == "Circle");
    CHECK(store.removeTool("Basics", "circle").empty());
    CHECK_FALSE(store.removeTool("Basics", "circle").empty());
    CHECK(store.removePalette("Basics").empty());
    CHECK(store.palettes().size() == 1);
}

TEST_CASE("a damaged file falls back to the defaults with a warning") {
    TempDir dir;
    std::ofstream(dir.path / "palettes.json") << "{ nope";
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    CHECK_FALSE(store.warning().empty());
    CHECK(store.palettes().size() == 2);
}

TEST_CASE("toolLines: layer first, then the command with the saved answers") {
    Document doc;
    Tool block;
    block.kind = ToolKind::Block;
    block.block = "door";
    block.file = "lib.tbcad";
    block.layer = "A-DOOR";
    block.scale = 2;
    block.angle = 90;
    auto lines = toolLines(block, doc);
    const std::vector<std::string> expect{"-LAYER",    "Set",   "A-DOOR", "",       "CONTENTIMPORT",
                                          "lib.tbcad", "Block", "door",   "INSERT", "door",
                                          "Scale",     "2",     "Rotate", "90"};
    CHECK(lines == expect);

    // The block is already in the drawing: no copy step. Defaults add no keyword answers.
    {
        Transaction tx = doc.begin("b");
        (void)tx.addBlock(BlockDef{.name = "door"});
        tx.commit();
    }
    Tool plain;
    plain.kind = ToolKind::Block;
    plain.block = "door";
    plain.file = "lib.tbcad";
    CHECK(toolLines(plain, doc) == std::vector<std::string>{"INSERT", "door"});

    Tool hatch;
    hatch.kind = ToolKind::Hatch;
    hatch.pattern = "ANSI31";
    hatch.scale = 0.5;
    hatch.angle = 45;
    CHECK(toolLines(hatch, doc) ==
          std::vector<std::string>{"HATCH", "Pattern", "ANSI31", "Scale", "0.5", "Angle", "45"});

    Tool macro;
    macro.command = " ZOOM | Extents ||";
    CHECK(toolLines(macro, doc) == std::vector<std::string>{"ZOOM", "Extents"});
}

TEST_CASE("-PALETTE without a settings folder explains - with one it edits palettes.json") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    host.run("-PALETTE", {"New", "X", "Done"});
    CHECK(host.logContains("settings folder"));
    CHECK(reg.find("PALETTES"));
    CHECK(reg.find("TP"));

    TempDir dir;
    host.setSettingsFolder(dir.path);
    host.run("-PALETTE", {"New", "Doors", "AddBlock", "Doors", "Single", "door", "2", "AddCommand",
                          "Doors", "Zoom all", "ZOOM|Extents", "AddHatch", "Doors", "Brick",
                          "AR-B816", "", "New", "Doors", "Done"});
    CHECK(host.logContains("already exists"));
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    Palette* p = store.find("Doors");
    REQUIRE(p);
    REQUIRE(p->tools.size() == 3);
    CHECK(p->tools[0].kind == ToolKind::Block);
    CHECK(p->tools[0].scale == doctest::Approx(2));
    CHECK(p->tools[1].command == "ZOOM|Extents");
    CHECK(p->tools[2].pattern == "AR-B816");

    host.run("-PALETTE",
             {"Remove", "Doors", "Brick", "Rename", "Doors", "Entrances", "List", "Done"});
    store.load();
    CHECK(store.find("Entrances"));
    CHECK(store.find("Entrances")->tools.size() == 2);
    CHECK(host.logContains("Zoom all"));
    host.run("-PALETTE", {"Delete", "Entrances", "Done"});
    store.load();
    CHECK(store.find("Entrances") == nullptr);
}

TEST_CASE("replaceTool edits a tool in place and checks the new values") {
    TempDir dir;
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    REQUIRE(store.addPalette("P").empty());
    Tool a;
    a.name = "Zoom";
    a.command = "ZOOM|E";
    Tool b = a;
    b.name = "Pan";
    b.command = "PAN";
    REQUIRE(store.addTool("P", a).empty());
    REQUIRE(store.addTool("P", b).empty());
    Tool edit = a;
    edit.command = "ZOOM|W";
    CHECK(store.replaceTool("P", "zoom", edit).empty());  // same name is fine
    CHECK(store.find("P")->tools[0].command == "ZOOM|W");
    edit.name = "pan";
    CHECK_FALSE(store.replaceTool("P", "Zoom", edit).empty());  // taken by another tool
    edit.name = "Zoom window";
    edit.command = " ";
    CHECK_FALSE(store.replaceTool("P", "Zoom", edit).empty());  // no command
    edit.command = "ZOOM|W";
    CHECK(store.replaceTool("P", "Zoom", edit).empty());
    REQUIRE(store.find("P")->tools.size() == 2);
    CHECK(store.find("P")->tools[0].name == "Zoom window");  // keeps its place
    CHECK_FALSE(store.replaceTool("P", "Nope", edit).empty());
}

TEST_CASE("toolsFromObjects: inserts, hatches and other objects become tools on their layer") {
    Document doc;
    Transaction tx = doc.begin("setup");
    const LayerId walls = tx.addLayer(Layer{.name = "A-WALL"}).value();
    const LayerId edge = tx.addLayer(Layer{.name = "edge", .parent = walls}).value();
    const BlockId door = tx.addBlock(BlockDef{.name = "Door"}).value();
    auto put = [&](EntityData data, LayerId layer) {
        Entity e;
        e.common.layer = layer;
        e.data = std::move(data);
        return tx.add(std::move(e)).value();
    };
    const EntityId ins =
        put(Indirect<InsertData>(InsertData{door, {5, 5}, {2, 2}, 3.14159265358979323846 / 2}),
            walls);
    HatchData h;
    h.solid = false;
    h.pattern = "ANSI31";
    h.patternScale = 0.5;
    HatchLoop loop;
    loop.edges.push_back(HatchPolylineEdge{{{{0, 0}}, {{4, 0}}, {{4, 4}}}, true});
    h.loops.push_back(std::move(loop));
    const EntityId hatch = put(Indirect<HatchData>(std::move(h)), edge);
    const EntityId line1 = put(LineData{{0, 0}, {1, 0}}, kLayerZero);
    const EntityId line2 = put(LineData{{0, 1}, {1, 1}}, kLayerZero);
    const EntityId circle = put(CircleData{{0, 0}, 3}, edge);
    tx.commit();

    const auto tools = toolsFromObjects(doc, {ins, hatch, line1, line2, circle}, "lib.tbcad");
    REQUIRE(tools.size() == 4);  // the second line adds nothing new
    CHECK(tools[0].kind == ToolKind::Block);
    CHECK(tools[0].name == "Door");
    CHECK(tools[0].block == "Door");
    CHECK(tools[0].file == "lib.tbcad");
    CHECK(tools[0].scale == 2);
    CHECK(tools[0].angle == doctest::Approx(90));
    CHECK(tools[0].layer == "A-WALL");
    CHECK(tools[1].kind == ToolKind::Hatch);
    CHECK(tools[1].pattern == "ANSI31");
    CHECK(tools[1].scale == 0.5);
    CHECK(tools[1].layer == "A-WALL~edge");
    CHECK(tools[2].kind == ToolKind::Command);
    CHECK(tools[2].command == "LINE");
    CHECK(tools[2].layer == "0");
    CHECK(tools[3].command == "CIRCLE");
    // The tools start the right commands.
    CHECK(toolLines(tools[3], doc) ==
          std::vector<std::string>{"-LAYER", "Set", "A-WALL~edge", "", "CIRCLE"});
}

TEST_CASE("-PALETTE AddSelected adds tools for the selected objects") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    TempDir dir;
    host.setSettingsFolder(dir.path);
    EntityId line;
    {
        Transaction tx = doc.begin("l");
        Entity e;
        e.data = LineData{{0, 0}, {10, 0}};
        line = tx.add(std::move(e)).value();
        tx.commit();
    }
    host.run("-PALETTE", {"New", "Mine", "AddSelected", "Mine", cmd::ScriptItem::select({line}),
                          cmd::ScriptItem::enter(), "Done"});
    CHECK(host.logContains("1 tool(s) added to Mine."));
    PaletteStore store(dir.path / "palettes.json");
    store.load();
    REQUIRE(store.find("Mine"));
    REQUIRE(store.find("Mine")->tools.size() == 1);
    CHECK(store.find("Mine")->tools[0].command == "LINE");
}
