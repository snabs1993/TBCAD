#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "support.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path;
    explicit Dir(const char* name) {
        path = fs::temp_directory_path() / name;
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() { fs::remove_all(path); }
};

void writeBox(const fs::path& file, double w, double h) {
    Document d;
    Transaction tx = d.begin("box");
    auto line = [&](double a, double b, double c, double e) {
        Entity ent;
        ent.data = LineData{{a, b}, {c, e}};
        (void)tx.add(ent);
    };
    line(0, 0, w, 0);
    line(w, 0, w, h);
    line(w, h, 0, h);
    line(0, h, 0, 0);
    tx.commit();
    fs::create_directories(file.parent_path());
    REQUIRE(io::writeTbcad(d, file, io::WriteOptions{.keepBackup = false}).ok());
}

BlockId makeBlock(Fixture& f, const char* name, double w, double h) {
    f.addLine(0, 0, w, 0);
    f.addLine(w, 0, w, h);
    f.host->setSelection(f.modelSpace());
    f.host->run("BLOCK", {name, "0,0", "D"});
    return f.doc.blockByName(name)->id;
}

}  // namespace

TEST_CASE("thumbnail of a block is scaled so the long side is 1") {
    Fixture f;
    BlockId b = makeBlock(f, "Wide", 40, 10);
    blocks::registerInsertDisplay();
    Thumbnail t = thumbnailOfBlock(f.doc, b);
    REQUIRE_FALSE(t.empty());
    CHECK(t.width == doctest::Approx(1));
    CHECK(t.height == doctest::Approx(0.25));
    for (const auto& s : t.strokes)
        for (Vec2 p : s) {
            CHECK(p.x >= -1e-9);
            CHECK(p.x <= 1 + 1e-9);
            CHECK(p.y >= -1e-9);
            CHECK(p.y <= 0.25 + 1e-9);
        }
    Document empty;
    CHECK(thumbnailOfModel(empty).empty());
    CHECK(thumbnailOfBlock(f.doc, BlockId{999}).empty());
}

TEST_CASE("library: scan, filter, thumbnails and file lookup") {
    Dir dir("tbcad_palette_lib");
    writeBox(dir.path / "Door.tbcad", 10, 20);
    writeBox(dir.path / "sub" / "Window.tbcad", 30, 10);
    std::ofstream(dir.path / "notes.txt") << "x";
    std::ofstream(dir.path / "plan.dxf") << "0\nSECTION\n2\nENTITIES\n0\nENDSEC\n0\nEOF\n";

    auto all = scanLibrary(dir.path);
    REQUIRE(all.size() == 3);
    CHECK(all[0].name == "Door");
    CHECK(all[1].name == "plan");
    CHECK(all[2].name == "Window");
    auto some = scanLibrary(dir.path, "WIN");
    REQUIRE(some.size() == 1);
    CHECK(some[0].name == "Window");
    CHECK(scanLibrary(dir.path / "missing").empty());

    LibraryThumbnails thumbs;
    CHECK(thumbs.find(all[0].path) == nullptr);
    int loaded = 0;
    while (thumbs.loadNext(all))
        ++loaded;
    CHECK(loaded == 3);
    const Thumbnail* door = thumbs.find(all[0].path);
    REQUIRE(door);
    CHECK(door->height == doctest::Approx(1));
    CHECK(door->width == doctest::Approx(0.5));
    const Thumbnail* plan = thumbs.find(all[1].path);
    REQUIRE(plan);
    CHECK(plan->empty());  // a drawing without geometry still counts as loaded

    CHECK(libraryFileFor("Door", {}, dir.path) == dir.path / "Door.tbcad");
    CHECK(libraryFileFor("sub/Window.tbcad", {}, dir.path) == dir.path / "sub/Window.tbcad");
    CHECK(libraryFileFor("Door", dir.path / "host.tbcad", {}) == dir.path / "Door.tbcad");
    CHECK(libraryFileFor((dir.path / "plan.dxf").string(), {}, {}) == dir.path / "plan.dxf");
    CHECK(libraryFileFor("nothing", {}, dir.path).empty());
    CHECK(libraryFileFor("notes.txt", {}, dir.path).empty());
}

TEST_CASE("INSERT takes a library file by name and makes it a block once") {
    Dir dir("tbcad_palette_insert");
    writeBox(dir.path / "Door.tbcad", 10, 20);
    Fixture f;
    f.host->setSetting("block.library", dir.path.string());
    auto r = f.host->run("INSERT", {"Door", "5,5"});
    CHECK(r.finished);
    REQUIRE(f.doc.blockByName("Door"));
    CHECK(f.doc.blockByName("Door")->entities.size() == 4);
    REQUIRE(f.modelSpace().size() == 1);
    // A second insert reuses the block.
    f.host->run("INSERT", {"Door", "50,5"});
    CHECK(f.modelSpace().size() == 2);
    CHECK(f.doc.blocks().size() == 1);
    // Esc after naming the file leaves nothing behind.
    Fixture g;
    g.host->setSetting("block.library", dir.path.string());
    g.host->run("INSERT", {"Door", cmd::ScriptItem::esc()});
    CHECK(g.doc.blockByName("Door") == nullptr);
}

TEST_CASE("block count: direct and nested inserts, CSV, table") {
    Fixture f;
    BlockId a = makeBlock(f, "A", 10, 10);
    // B contains two inserts of A.
    f.host->run("INSERT", {"A", "0,0"});
    f.host->run("INSERT", {"A", "20,0"});
    f.host->setSelection(f.modelSpace());
    f.host->run("BLOCK", {"B", "0,0", "D"});
    f.host->run("INSERT", {"B", "0,0"});
    f.host->run("INSERT", {"B", "100,0"});
    f.host->run("INSERT", {"A", "200,0"});
    (void)a;

    auto rows = blockCount(f.doc);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "A");
    CHECK(rows[0].direct == 1);
    CHECK(rows[0].total == 5);  // one placed + two in each of two Bs
    CHECK(rows[1].name == "B");
    CHECK(rows[1].direct == 2);
    CHECK(rows[1].total == 2);
    CHECK(blockCountCsv(rows) == "Block,Count,Total\r\nA,1,5\r\nB,2,2\r\n");
    CHECK(blockCountCsv({{"x\"y z", 1, 2}}) == "Block,Count,Total\r\n\"x\"\"y z\",1,2\r\n");
    CHECK(blockCountCsv({{"a,b", 1, 2}}) == "Block,Count,Total\r\n\"a,b\",1,2\r\n");

    Dir dir("tbcad_palette_count");
    fs::path csv = dir.path / "out.csv";
    f.host->clearLog();
    f.host->run("BLOCKCOUNT", {"C", csv.string()});
    CHECK(f.host->logContains("2 blocks, 3 inserts"));
    CHECK(fs::exists(csv));

    std::size_t before = f.modelSpace().size();
    f.host->run("BLOCKCOUNT", {"T", "0,-50"});
    CHECK(f.modelSpace().size() > before);
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == before);

    Fixture none;
    none.host->run("BLOCKCOUNT", {});
    CHECK(none.host->logContains("No blocks are inserted"));
}

TEST_CASE("block count: blocks that hold each other many times over, and cycles") {
    Document doc;
    std::vector<BlockId> chain;
    BlockId self, ping, pong;
    {
        auto tx = doc.begin("blocks");
        // L0 holds L1 twice, L1 holds L2 twice, ... 40 levels: 2^39 L39 in one L0.
        for (int i = 0; i < 40; ++i)
            chain.push_back(tx.addBlock(BlockDef{.name = "L" + std::to_string(i)}).value());
        for (int i = 0; i + 1 < 40; ++i)
            for (double y : {0.0, 1.0})
                (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{chain[i + 1], {0, y}})},
                             chain[i]);
        self = tx.addBlock(BlockDef{.name = "SELF"}).value();
        (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{self})}, self);
        ping = tx.addBlock(BlockDef{.name = "PING"}).value();
        pong = tx.addBlock(BlockDef{.name = "PONG"}).value();
        (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{pong})}, ping);
        (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{ping})}, pong);
        for (BlockId b : {chain[0], chain[0], self, ping})
            (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{b})});
        tx.commit();
    }
    std::unordered_map<std::string, BlockCountRow> by;
    for (const BlockCountRow& r : blockCount(doc))
        by[r.name] = r;
    CHECK(by.at("L0").direct == 2);
    CHECK(by.at("L0").total == 2);
    CHECK(by.at("L1").total == 4);
    CHECK(by.at("L39").total == (std::size_t{1} << 40));
    CHECK(by.at("SELF").total >= 1);
    CHECK(by.count("PING") == 1);
}

TEST_CASE("INSERT in a drawing with no blocks still takes a typed drawing file") {
    Dir dir("tbcad_insert_first_file");
    writeBox(dir.path / "Frame.tbcad", 10, 20);
    Fixture f;
    REQUIRE(f.doc.blocks().empty());
    const std::string file = (dir.path / "Frame.tbcad").string();
    CHECK(f.host->run("INSERT", {file, "5,5"}).finished);
    CHECK(f.doc.blockByName("Frame") != nullptr);
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("paletteInsertLines adds only the options that differ from plain INSERT") {
    CHECK(paletteInsertLines("Door", {}) == std::vector<std::string>{"INSERT", "Door"});
    InsertOptions o{.scale = 2.5, .rotationDeg = 90, .repeat = true, .explode = true};
    CHECK(paletteInsertLines("Door", o) == std::vector<std::string>{"INSERT", "Door", "Scale",
                                                                    "2.5", "Rotate", "90",
                                                                    "Multiple", "Explode"});
    CHECK(paletteInsertLines("D", {.scale = 0}) == std::vector<std::string>{"INSERT", "D"});
}

TEST_CASE("the recent block list keeps the newest first, once per block and file") {
    std::string s = pushRecent("", {"Door", "", false});
    s = pushRecent(s, {"Window", "C:/lib/w.tbcad", true});
    s = pushRecent(s, {"door", "", false});  // the same block again moves to the front
    auto list = parseRecent(s);
    REQUIRE(list.size() == 2);
    CHECK(list[0] == RecentBlock{"door", "", false});
    CHECK(list[1] == RecentBlock{"Window", "C:/lib/w.tbcad", true});
    // The same name from another drawing is another entry.
    s = pushRecent(s, {"Door", "other.tbcad", false});
    CHECK(parseRecent(s).size() == 3);
    for (int i = 0; i < 40; ++i)
        s = pushRecent(s, {"B" + std::to_string(i), "", false});
    list = parseRecent(s);
    CHECK(list.size() == kMaxRecentBlocks);
    CHECK(list.front().name == "B39");
    CHECK(parseRecent(formatRecent(list)) == list);
    CHECK(parseRecent("broken\x1e\x1fno name\x1f"
                      "0\x1eok\x1f\x1f"
                      "1") == std::vector<RecentBlock>{{"ok", "", true}});
}

TEST_CASE("INSERT records recent blocks, with the drawing file for whole files") {
    Dir dir("tbcad_insert_recent");
    writeBox(dir.path / "Frame.tbcad", 10, 20);
    Fixture f;
    const std::string file = (dir.path / "Frame.tbcad").string();
    f.host->run("INSERT", {file, "5,5"});
    f.host->setSetting("tmp.insert.source", "C:/other.tbcad");  // as the palette does
    f.host->run("INSERT", {"Frame", "15,5"});
    const auto list = parseRecent(f.host->setting("block.recent").value_or(""));
    REQUIRE(list.size() == 2);
    CHECK(list[0] == RecentBlock{"Frame", "C:/other.tbcad", false});
    CHECK(list[1].whole);
    CHECK(fs::path(list[1].file) == dir.path / "Frame.tbcad");
    CHECK(f.host->setting("tmp.insert.source").value_or("") == "");  // used once
}

TEST_CASE("INSERT asks for scale and rotation after the point when the palette says so") {
    Dir dir("tbcad_insert_ask");
    writeBox(dir.path / "Frame.tbcad", 10, 20);
    Fixture f;
    f.host->run("INSERT", {(dir.path / "Frame.tbcad").string(), "0,0"});
    f.host->setSetting("tmp.insert.ask", "SR");
    CHECK(f.host->run("INSERT", {"Frame", "100,0", "3", "0,100"}).finished);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace().back()));
    REQUIRE(ins);
    CHECK(ins->position == Vec2{100, 0});
    CHECK(ins->scale == Vec2{3, 3});
    CHECK(ins->rotation == doctest::Approx(3 * kPi / 4));  // toward the picked point
    // Only for that one run.
    CHECK(f.host->run("INSERT", {"Frame", "200,0"}).finished);
    CHECK(entityAs<InsertData>(*f.doc.find(f.modelSpace().back()))->scale == Vec2{1, 1});
    // Enter keeps the defaults.
    f.host->setSetting("tmp.insert.ask", "SR");
    CHECK(f.host->run("INSERT", {"Frame", "S", "2", "300,0", "", ""}).finished);
    const auto* last = entityAs<InsertData>(*f.doc.find(f.modelSpace().back()));
    CHECK(last->scale == Vec2{2, 2});
    CHECK(last->rotation == doctest::Approx(0));
}

TEST_CASE("BLOCKSPALETTE opens the Blocks palette") {
    Fixture f;
    CHECK(f.host->run("BLOCKSPALETTE", {}).finished);
    CHECK(f.host->run("BP", {}).finished);
    CHECK(f.host->run("BLOCKSPALETTECLOSE", {}).finished);
}
