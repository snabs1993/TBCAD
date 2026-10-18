// Blocks at scale: 50,000 references to a block nested five levels deep, a block edit that
// updates every reference, attribute extraction on 10,000 attributes, WBLOCK and INSERT from a
// file, and PURGE of a chain of unused nested blocks. Size: TBCAD_BENCH_N references (default
// 50,000). Prints; the checks guard the results and generous time limits.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Big {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    int n = 0;
    int fan = 2;                 // references per nesting level (TBCAD_BENCH_FAN)
    std::vector<BlockId> level;  // level[0] = leaf ... level[4] = outermost

    Big() {
        dir = std::filesystem::temp_directory_path() / "tbcad_block_scale";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_BENCH_N");
        n = env ? std::atoi(env) : 50000;
        if (const char* f = std::getenv("TBCAD_BENCH_FAN"))
            fan = std::atoi(f);
    }

    Document& doc() { return core->document(); }

    // L0: six lines; L1..L4: two references to the level below, side by side.
    void buildBlocks() {
        auto tx = doc().begin("blocks");
        for (int lv = 0; lv < 5; ++lv) {
            BlockId b = tx.addBlock({.name = "L" + std::to_string(lv)}).value();
            level.push_back(b);
            if (lv == 0) {
                for (int k = 0; k < 6; ++k) {
                    Entity e;
                    e.data = LineData{{k * 1.0, 0}, {k * 1.0 + 0.8, 3}};
                    (void)tx.add(std::move(e), b);
                }
            } else {
                for (int k = 0; k < fan; ++k) {
                    Entity e;
                    e.data =
                        Indirect<InsertData>(InsertData{level[static_cast<std::size_t>(lv - 1)],
                                                        {k * 7.0 * std::pow(fan, lv - 1), 0},
                                                        {1, 1},
                                                        0});
                    (void)tx.add(std::move(e), b);
                }
            }
        }
        tx.commit();
    }

    void placeInserts() {
        auto tx = doc().begin("inserts");
        for (int i = 0; i < n; ++i) {
            Entity e;
            e.data = Indirect<InsertData>(
                InsertData{level.back(), {(i % 250) * 200.0, (i / 250) * 40.0}, {1, 1}, 0});
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
};

}  // namespace

TEST_CASE("perf: 50,000 references, five levels deep - draw, INSERT, block edit") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    big.buildBlocks();
    auto t = std::chrono::steady_clock::now();
    big.placeInserts();
    std::printf("place %d references of a 5-level block: %.0f ms\n", big.n, ms(t));
    t = std::chrono::steady_clock::now();
    big.core->zoomExtents();
    big.core->updateFrame();
    std::printf("first frame (all references): %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    big.core->updateFrame();
    std::printf("next frame: %.0f ms\n", ms(t));

    const std::size_t before = big.doc().modelSpaceCount();
    t = std::chrono::steady_clock::now();
    for (const char* l : {"INSERT", "L4", "0,-100", "1", "1", "0"})
        big.core->submitText(l);
    big.core->updateFrame();
    const double insertMs = ms(t);
    std::printf("INSERT one more: %.0f ms (objects %zu -> %zu)\n", insertMs, before,
                big.doc().modelSpaceCount());
    CHECK(big.doc().modelSpaceCount() == before + 1);

    // Block edit: change the leaf block, so every reference changes with it.
    t = std::chrono::steady_clock::now();
    for (const char* l : {"BEDIT", "Name", "L0"})
        big.core->submitText(l);
    big.core->updateFrame();
    const double beditMs = ms(t);
    t = std::chrono::steady_clock::now();
    big.core->submitText("BCLOSE");
    big.core->submitText("");
    big.core->updateFrame();
    const double bcloseMs = ms(t);
    std::printf("BEDIT %.0f ms, BCLOSE (save, every reference follows) %.0f ms\n", beditMs,
                bcloseMs);
    CHECK(big.doc().modelSpaceCount() == before + 1);
    t = std::chrono::steady_clock::now();
    CHECK(big.core->undo());
    big.core->updateFrame();
    std::printf("undo of the block edit: %.0f ms\n", ms(t));
    CHECK(insertMs < 20000);
    CHECK(bcloseMs < 20000);
}

TEST_CASE("perf: ATTEXT on 10,000 attributes and a block edit that adds an attribute") {
    Big big;
    BlockId tag;
    {
        auto tx = big.doc().begin("tag");
        tag = tx.addBlock({.name = "Tag"}).value();
        for (int k = 0; k < 10; ++k) {
            TextData t;
            t.position = {0, k * 3.0};
            t.height = 2;
            t.utf8 = "x";
            t.attdef = AttDefInfo{.tag = "T" + std::to_string(k), .defaultValue = "d"};
            Entity e;
            e.data = std::move(t);
            (void)tx.add(std::move(e), tag);
        }
        tx.commit();
    }
    const int refs = 1000;
    {
        auto tx = big.doc().begin("refs");
        for (int i = 0; i < refs; ++i) {
            InsertData ins{tag, {i * 20.0, 0}, {1, 1}, 0};
            ins.attribs = blocks::defaultAttribs(big.doc(), tag);
            ins.attribs[0].value = "No." + std::to_string(i);
            Entity e;
            e.data = Indirect<InsertData>(std::move(ins));
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    const auto csv = big.dir / "out.csv";
    auto t = std::chrono::steady_clock::now();
    big.core->submitText("ATTEXT");
    big.core->submitText("");
    big.core->submitText(csv.string());
    const double extMs = ms(t);
    std::ifstream in(csv, std::ios::binary);
    REQUIRE(in);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    std::size_t rows = 0;
    for (char c : text)
        rows += c == '\n';
    std::printf("ATTEXT of %d attributes: %.0f ms (%zu lines)\n", refs * 10, extMs, rows);
    CHECK(rows == static_cast<std::size_t>(refs) + 1);
    CHECK(text.find("No.999") != std::string::npos);
    CHECK(extMs < 10000);

    // A new attribute definition reaches every reference (what BSAVE does for the block).
    t = std::chrono::steady_clock::now();
    {
        auto tx = big.doc().begin("edit block");
        TextData d;
        d.height = 2;
        d.utf8 = "x";
        d.attdef = AttDefInfo{.tag = "EXTRA", .defaultValue = "new"};
        Entity e;
        e.data = std::move(d);
        (void)tx.add(std::move(e), tag);
        CHECK(blocks::syncInserts(tx, big.doc(), tag) == static_cast<std::size_t>(refs));
        tx.commit();
    }
    std::printf("new attribute synced to %d references: %.0f ms\n", refs, ms(t));
    std::size_t withExtra = 0;
    big.doc().forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e))
            if (ins->attribs.size() == 11 && ins->attribs[0].value.rfind("No.", 0) == 0)
                ++withExtra;
    });
    CHECK(withExtra == static_cast<std::size_t>(refs));
    CHECK(ms(t) < 10000);
}

TEST_CASE("perf: WBLOCK of nested blocks and INSERT of the file") {
    Big big;
    big.buildBlocks();
    {
        auto tx = big.doc().begin("lines");
        for (int i = 0; i < 20000; ++i) {
            Entity e;
            e.data = LineData{{i * 1.0, 0}, {i * 1.0, 5}};
            (void)tx.add(std::move(e));
        }
        for (int i = 0; i < 500; ++i) {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{big.level.back(), {i * 100.0, 50}, {1, 1}, 0});
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    const auto file = big.dir / "part.tbcad";
    auto t = std::chrono::steady_clock::now();
    big.core->submitText("WBLOCK");
    big.core->submitText("ALL");
    big.core->submitText("");
    big.core->submitText("0,0");
    big.core->submitText(file.string());
    const double wbMs = ms(t);
    REQUIRE(std::filesystem::exists(file));
    std::printf("WBLOCK of %zu objects with 5 nested blocks: %.0f ms (%.2f MB)\n",
                big.doc().modelSpaceCount(), wbMs,
                static_cast<double>(std::filesystem::file_size(file)) / 1e6);
    CHECK(wbMs < 10000);

    // A fresh drawing takes the file in as a block and places it.
    ui::AppCore other(big.registry, platform::AppPaths{big.dir / "cfg2", big.dir / "data2"});
    other.setViewportSize(1400, 900);
    t = std::chrono::steady_clock::now();
    const std::string fileText = file.string();
    for (const char* l : {"INSERT", fileText.c_str(), "10,10"})
        other.submitText(l);
    other.updateFrame();
    const double insMs = ms(t);
    for (const auto& h : other.history())
        std::printf("  | %s\n", h.c_str());
    std::printf("INSERT of that file: %.0f ms (%zu objects, %zu blocks)\n", insMs,
                other.document().modelSpaceCount(), other.document().blocks().size());
    CHECK(other.document().modelSpaceCount() == 1);
    CHECK(other.document().blockByName("part") != nullptr);
    // 20,000 lines + 500 references, each pulling in the five nested blocks once
    CHECK(other.document().blocks().size() == 1 + 5);
    CHECK(insMs < 10000);
}

TEST_CASE("perf: PURGE removes a chain of unused nested blocks, keeps the used ones") {
    Big big;
    const int chain = 300;
    BlockId keep;
    {
        auto tx = big.doc().begin("chain");
        std::vector<BlockId> ids;
        for (int i = 0; i < chain; ++i)
            ids.push_back(tx.addBlock({.name = "C" + std::to_string(i)}).value());
        for (int i = 0; i < chain; ++i) {
            Entity e;
            if (i + 1 < chain)
                e.data = Indirect<InsertData>(
                    InsertData{ids[static_cast<std::size_t>(i + 1)], {1, 0}, {1, 1}, 0});
            else
                e.data = LineData{{0, 0}, {1, 1}};
            (void)tx.add(std::move(e), ids[static_cast<std::size_t>(i)]);
        }
        // a used pair: Outer (placed in model space) holds Inner
        BlockId inner = tx.addBlock({.name = "Inner"}).value();
        Entity ln;
        ln.data = LineData{{0, 0}, {2, 2}};
        (void)tx.add(std::move(ln), inner);
        keep = tx.addBlock({.name = "Outer"}).value();
        Entity ie;
        ie.data = Indirect<InsertData>(InsertData{inner, {0, 0}, {1, 1}, 0});
        (void)tx.add(std::move(ie), keep);
        Entity top;
        top.data = Indirect<InsertData>(InsertData{keep, {5, 5}, {1, 1}, 0});
        (void)tx.add(std::move(top));
        for (int i = 0; i < 100000; ++i) {
            Entity e;
            e.data = LineData{{i * 1.0, 0}, {i * 1.0, 1}};
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    auto t = std::chrono::steady_clock::now();
    big.core->submitText("PURGE");
    big.core->submitText("Blocks");
    big.core->submitText("Yes");
    const double ms1 = ms(t);
    std::printf("PURGE of a chain of %d nested unused blocks, 100k objects: %.0f ms\n", chain, ms1);
    CHECK(big.doc().blockByName("C0") == nullptr);
    CHECK(big.doc().blockByName("C299") == nullptr);
    CHECK(big.doc().blockByName("Outer") != nullptr);
    CHECK(big.doc().blockByName("Inner") != nullptr);
    CHECK(ms1 < 20000);
    CHECK(big.core->undo());
    CHECK(big.doc().blockByName("C150") != nullptr);
}
