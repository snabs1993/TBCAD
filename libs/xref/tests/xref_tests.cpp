#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <cstring>
#include <fstream>

#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/xref/xref.h"

using namespace tbcad;
using namespace tbcad::xref;

namespace {

Entity lineOn(LayerId layer, double x0, double y0, double x1, double y1) {
    Entity e;
    e.data = LineData{{x0, y0}, {x1, y1}};
    e.common.layer = layer;
    return e;
}

// A small source: layers "walls" (with a child "inner") and 0, a block "door" inserted once.
std::unique_ptr<Document> makeSource(int extraLines = 0) {
    auto doc = std::make_unique<Document>();
    Transaction tx = doc->begin("source");
    Layer walls;
    walls.name = "walls";
    walls.color = Color::index(3);
    LayerId w = tx.addLayer(walls).value();
    Layer inner;
    inner.name = "inner";
    inner.parent = w;
    LayerId in = tx.addLayer(inner).value();
    TextStyle ts;
    ts.name = "Notes";
    ts.fontFamily = "Arial";
    TextStyleId notes = tx.addTextStyle(ts).value();
    BlockId door = tx.addBlock(BlockDef{.name = "door"}).value();
    (void)tx.add(lineOn(kLayerZero, 0, 0, 1, 0), door);
    (void)tx.add(lineOn(w, 0, 0, 0, 1), door);
    (void)tx.add(lineOn(w, 0, 0, 10, 0));
    (void)tx.add(lineOn(in, 0, 1, 10, 1));
    Entity ins;
    ins.data = Indirect<InsertData>(InsertData{door, {5, 5}, {1, 1}, 0});
    (void)tx.add(ins);
    TextData t;
    t.utf8 = "hi";
    t.style = notes;
    Entity te;
    te.data = Indirect<TextData>(t);
    te.common.layer = w;
    (void)tx.add(te);
    for (int i = 0; i < extraLines; ++i)
        (void)tx.add(lineOn(w, i, 2, i, 3));
    tx.commit();
    return doc;
}

Source sourceOf(std::unique_ptr<Document> d, std::int64_t stamp = 42) {
    Source s;
    s.doc = std::move(d);
    s.stamp = stamp;
    return s;
}

std::size_t modelCount(const Document& d) {
    return d.modelSpace().size();
}

}  // namespace

TEST_CASE("attach copies the model space under an xref layer tree") {
    Document host;
    Source src = sourceOf(makeSource());
    Transaction tx = host.begin("attach");
    Summary sum;
    auto b = attach(tx, host, "site", "plans/site.tbcad", src, &sum);
    REQUIRE(b.ok());
    tx.commit();
    const BlockDef* def = host.block(b.value());
    REQUIRE(def);
    CHECK(def->xrefPath == "plans/site.tbcad");
    CHECK(def->xrefStamp == 42);
    CHECK(def->entities.size() == 4);  // two lines, the insert and the text
    CHECK(sum.entities == 4);
    CHECK(sum.blocks == 1);
    const BlockDef* nested = host.blockByName("site$door");
    REQUIRE(nested);
    CHECK(nested->entities.size() == 2);

    // Layers: site > walls > inner, and site > 0.
    const Layer* root = host.layerByName("site");
    REQUIRE(root);
    const Layer* walls = host.layerByName(root->id, "walls");
    REQUIRE(walls);
    CHECK(walls->color == Color::index(3));
    CHECK(host.layerByName(walls->id, "inner") != nullptr);
    CHECK(host.layerByName(root->id, "0") != nullptr);
    for (EntityId id : def->entities) {
        const Entity* e = host.find(id);
        REQUIRE(e);
        CHECK(e->common.layer != kLayerZero);
    }
    // The text style came along by name.
    CHECK(host.textStyleByName("Notes") != nullptr);
    CHECK(modelCount(host) == 0);  // nothing is inserted yet
}

TEST_CASE("an attached xref can be inserted and its layers switched off together") {
    Document host;
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b, {100, 0}, {1, 1}, 0});
        REQUIRE(tx.add(ins).ok());
        tx.commit();
    }
    CHECK(useCount(host, b) == 1);
    LayerId root = host.layerByName("site")->id;
    LayerId walls = host.layerByName(root, "walls")->id;
    CHECK(host.effective(walls).on);
    {
        Transaction tx = host.begin("hide");
        REQUIRE(tx.modifyLayer(root, [](Layer& l) { l.on = false; }).ok());
        tx.commit();
    }
    CHECK_FALSE(host.effective(walls).on);
}

TEST_CASE("reload replaces the content and keeps the inserts") {
    Document host;
    BlockId b;
    EntityId insertId;
    {
        Source src = sourceOf(makeSource(), 1);
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b, {100, 0}, {1, 1}, 0});
        insertId = tx.add(ins).value();
        tx.commit();
    }
    const BlockDef* def = host.block(b);
    CHECK(def->entities.size() == 4);
    {
        Source src = sourceOf(makeSource(3), 2);
        Transaction tx = host.begin("reload");
        Summary sum;
        REQUIRE(reload(tx, host, b, src, &sum).ok());
        tx.commit();
        CHECK(sum.entities == 7);
    }
    def = host.block(b);
    CHECK(def->entities.size() == 7);
    CHECK(def->xrefStamp == 2);
    CHECK(host.find(insertId) != nullptr);
    CHECK(entityAs<InsertData>(*host.find(insertId))->block == b);
    // The nested block was replaced, not duplicated.
    std::size_t doors = 0;
    for (const auto& [id, blk] : host.blocks())
        doors += blk.name.rfind("site$door", 0) == 0;
    CHECK(doors == 1);
    // Layers are reused.
    std::size_t siteLayers = 0;
    for (const auto& [id, l] : host.layers())
        siteLayers += l.name == "walls";
    CHECK(siteLayers == 1);
}

TEST_CASE("detach removes inserts, blocks and unused layers - bind keeps everything") {
    Document host;
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b, {0, 0}, {1, 1}, 0});
        REQUIRE(tx.add(ins).ok());
        tx.commit();
    }
    {
        Transaction tx = host.begin("bind");
        REQUIRE(bind(tx, host, b).ok());
        tx.commit();
    }
    CHECK(host.block(b)->xrefPath.empty());
    CHECK(state(host, b, {}) == State::NotXref);
    CHECK(host.blockByName("site$door") != nullptr);
    CHECK(host.layerByName("site") != nullptr);

    Document host2;
    BlockId b2;
    {
        Transaction tx = host2.begin("attach");
        b2 = attach(tx, host2, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b2, {0, 0}, {1, 1}, 0});
        REQUIRE(tx.add(ins).ok());
        tx.commit();
    }
    {
        Transaction tx = host2.begin("detach");
        REQUIRE(detach(tx, host2, b2).ok());
        tx.commit();
    }
    CHECK(host2.blocks().empty());
    CHECK(modelCount(host2) == 0);
    CHECK(host2.layerByName("site") == nullptr);
    CHECK(host2.entityCount() == 0);
}

TEST_CASE("an xref is saved in the .tbcad file together with its content") {
    namespace fs = std::filesystem;
    Document host;
    {
        Source src = sourceOf(makeSource(), 7);
        Transaction tx = host.begin("attach");
        REQUIRE(attach(tx, host, "site", "site.tbcad", src).ok());
        tx.commit();
    }
    auto bytes = io::writeTbcadToMemory(host);
    REQUIRE(bytes.ok());
    io::IoReport rep;
    auto back = io::readTbcad(bytes.value(), rep);
    REQUIRE(back.ok());
    const BlockDef* def = back.value()->blockByName("site");
    REQUIRE(def);
    CHECK(def->xrefPath == "site.tbcad");
    CHECK(def->xrefStamp == 7);
    CHECK(def->entities.size() == 4);
}

TEST_CASE("files: load by extension, stamps, state and path rules") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_tests";
    fs::create_directories(dir);
    fs::path file = dir / "plan one.tbcad";
    {
        auto doc = makeSource();
        REQUIRE(io::writeTbcad(*doc, file, io::WriteOptions{.keepBackup = false}).ok());
    }
    CHECK(defaultName(file) == "plan one");
    CHECK(defaultName("a$b c.dxf") == "a_b c");
    CHECK(resolvePath("plan one.tbcad", dir / "host.tbcad") == file);
    CHECK(resolvePath(file.string(), "/elsewhere/host.tbcad") == file);

    auto loaded = loadSource(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().stamp == fileStamp(file));
    CHECK(loaded.value().stamp != 0);

    Document host;
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "plan", "plan one.tbcad", loaded.value()).value();
        tx.commit();
    }
    CHECK(state(host, b, dir / "host.tbcad") == State::UpToDate);
    {
        auto doc = makeSource(2);
        REQUIRE(io::writeTbcad(*doc, file, io::WriteOptions{.keepBackup = false}).ok());
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    }
    CHECK(state(host, b, dir / "host.tbcad") == State::Changed);
    fs::remove(file);
    CHECK(state(host, b, dir / "host.tbcad") == State::Missing);

    CHECK_FALSE(loadSource(dir / "missing.tbcad").ok());
    std::ofstream(dir / "x.txt") << "no";
    CHECK_FALSE(loadSource(dir / "x.txt").ok());
    fs::remove_all(dir);
}

TEST_CASE("a dxf file can be attached") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_dxf";
    fs::create_directories(dir);
    fs::path file = dir / "t.dxf";
    {
        std::ofstream o(file);
        o << "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nA-WALL\n10\n0.0\n20\n0.0\n11\n5.0\n21\n0.0\n0\n"
             "ENDSEC\n0\nEOF\n";
    }
    auto loaded = loadSource(file);
    REQUIRE(loaded.ok());
    Document host;
    Transaction tx = host.begin("attach");
    auto b = attach(tx, host, "t", "t.dxf", loaded.value());
    REQUIRE(b.ok());
    tx.commit();
    CHECK(host.block(b.value())->entities.size() == 1);
    const Layer* root = host.layerByName("t");
    REQUIRE(root);
    CHECK(host.layerByName(root->id, "A-WALL") != nullptr);
    fs::remove_all(dir);
}

TEST_CASE("importAsBlock makes an ordinary block and matches layers by name") {
    Document host;
    {
        Transaction tx = host.begin("layer");
        Layer l;
        l.name = "walls";
        l.color = Color::index(1);
        REQUIRE(tx.addLayer(l).ok());
        tx.commit();
    }
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("import");
        auto r = importAsBlock(tx, host, "door set", src);
        REQUIRE(r.ok());
        b = r.value();
        tx.commit();
    }
    const BlockDef* def = host.block(b);
    CHECK(def->xrefPath.empty());
    CHECK(def->entities.size() == 4);
    // The existing layer was reused (its colour is untouched), the missing one was created.
    std::size_t walls = 0;
    for (const auto& [id, l] : host.layers())
        walls += l.name == "walls";
    CHECK(walls == 1);
    CHECK(host.layerByName("walls")->color == Color::index(1));
    CHECK(host.layerByName(host.layerByName("walls")->id, "inner") != nullptr);
    CHECK(host.layerByName("door set") == nullptr);  // no xref layer
    CHECK(host.blockByName("door set$door") != nullptr);
    // Name already taken.
    Transaction tx = host.begin("again");
    CHECK_FALSE(importAsBlock(tx, host, "door set", src).ok());
}

TEST_CASE("overlay flag is kept, saved and cleared by bind") {
    Document host;
    BlockId b;
    {
        Source src = sourceOf(makeSource(), 7);
        Transaction tx = host.begin("overlay");
        b = attach(tx, host, "site", "site.tbcad", src, nullptr, true).value();
        tx.commit();
    }
    CHECK(host.block(b)->xrefOverlay);
    auto bytes = io::writeTbcadToMemory(host);
    REQUIRE(bytes.ok());
    io::IoReport rep;
    auto back = io::readTbcad(bytes.value(), rep);
    REQUIRE(back.ok());
    CHECK(back.value()->blockByName("site")->xrefOverlay);
    {
        Transaction tx = host.begin("bind");
        REQUIRE(bind(tx, host, b).ok());
        tx.commit();
    }
    CHECK_FALSE(host.block(b)->xrefOverlay);
}

TEST_CASE("an overlay is not carried along when its drawing is attached elsewhere") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_overlay";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // B holds its own line plus an attached xref of A and an overlay of A.
    {
        auto a = makeSource();
        REQUIRE(io::writeTbcad(*a, dir / "a.tbcad", io::WriteOptions{.keepBackup = false}).ok());
    }
    auto loadedA = loadSource(dir / "a.tbcad");
    REQUIRE(loadedA.ok());
    Document b;
    {
        Transaction tx = b.begin("b");
        (void)tx.add(lineOn(kLayerZero, 0, 0, 1, 1));
        BlockId att = attach(tx, b, "attached", "a.tbcad", loadedA.value()).value();
        BlockId ovl = attach(tx, b, "overlaid", "a.tbcad", loadedA.value(), nullptr, true).value();
        for (BlockId id : {att, ovl}) {
            Entity ins;
            ins.data = Indirect<InsertData>(InsertData{id, {0, 0}, {1, 1}, 0});
            (void)tx.add(ins);
        }
        tx.commit();
    }
    REQUIRE(io::writeTbcad(b, dir / "b.tbcad", io::WriteOptions{.keepBackup = false}).ok());

    auto loadedB = loadSource(dir / "b.tbcad");
    REQUIRE(loadedB.ok());
    Document c;
    Transaction tx = c.begin("c");
    auto blk = attach(tx, c, "b", "b.tbcad", loadedB.value());
    REQUIRE(blk.ok());
    tx.commit();
    // b's line + the insert of "attached"; the overlay insert is left out.
    CHECK(c.block(blk.value())->entities.size() == 2);
    CHECK(c.blockByName("b$overlaid") == nullptr);
    CHECK(c.blockByName("b$attached") != nullptr);
    fs::remove_all(dir);
}

TEST_CASE("nested xrefs are refreshed from their own files when the outer one loads") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_nested";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto writeA = [&](int extra) {
        auto a = makeSource(extra);
        REQUIRE(io::writeTbcad(*a, dir / "a.tbcad", io::WriteOptions{.keepBackup = false}).ok());
    };
    writeA(0);
    {
        auto loadedA = loadSource(dir / "a.tbcad");
        REQUIRE(loadedA.ok());
        Document b;
        Transaction tx = b.begin("b");
        BlockId att = attach(tx, b, "a", "a.tbcad", loadedA.value()).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{att, {0, 0}, {1, 1}, 0});
        (void)tx.add(ins);
        tx.commit();
        REQUIRE(io::writeTbcad(b, dir / "b.tbcad", io::WriteOptions{.keepBackup = false}).ok());
    }
    writeA(4);  // A grows after B was saved
    fs::last_write_time(dir / "a.tbcad",
                        fs::last_write_time(dir / "a.tbcad") + std::chrono::seconds(5));

    auto loadedB = loadSource(dir / "b.tbcad");
    REQUIRE(loadedB.ok());
    const Document& bdoc = *loadedB.value().doc;
    const BlockDef* a = bdoc.blockByName("a");
    REQUIRE(a);
    CHECK(a->entities.size() == 4 + 4);  // 4 original objects + the 4 new lines
    fs::remove_all(dir);
}

TEST_CASE("a loop of xrefs does not recurse forever") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_loop";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // x.tbcad attaches y.tbcad, y.tbcad attaches x.tbcad.
    auto write = [&](const char* self, const char* other, bool withOther) {
        Document d;
        Transaction tx = d.begin("d");
        (void)tx.add(lineOn(kLayerZero, 0, 0, 1, 1));
        if (withOther) {
            Source s;
            s.doc = std::make_unique<Document>();
            s.stamp = 1;
            auto blk = attach(tx, d, other, std::string(other) + ".tbcad", s);
            REQUIRE(blk.ok());
        }
        tx.commit();
        REQUIRE(io::writeTbcad(d, dir / (std::string(self) + ".tbcad"),
                               io::WriteOptions{.keepBackup = false})
                    .ok());
    };
    write("x", "y", true);
    write("y", "x", true);
    auto loaded = loadSource(dir / "x.tbcad");
    CHECK(loaded.ok());
    fs::remove_all(dir);
}

TEST_CASE("stored path types and extra loaders") {
    namespace fs = std::filesystem;
    fs::path root = fs::absolute(fs::temp_directory_path() / "tbcad_xref_paths");
    fs::path host = root / "drawings" / "main.tbcad";
    fs::path file = root / "refs" / "site.tbcad";
    CHECK(storedPath(file, host, PathType::Relative) == "../refs/site.tbcad");
    CHECK(storedPath(file, host, PathType::None) == "site.tbcad");
    CHECK(fs::path(storedPath(file, host, PathType::Absolute)) == file);
    CHECK(fs::path(storedPath(file, {}, PathType::Relative)) == file);

    fs::path dir = fs::temp_directory_path() / "tbcad_xref_loader";
    fs::create_directories(dir);
    std::ofstream(dir / "t.zzz") << "x";
    CHECK_FALSE(loadSource(dir / "t.zzz").ok());
    registerLoader(".ZZZ", [](const fs::path&) -> Result<Source> {
        Source s;
        s.doc = makeSource();
        return s;
    });
    auto loaded = loadSource(dir / "t.zzz");
    REQUIRE(loaded.ok());
    CHECK(loaded.value().stamp != 0);
    CHECK(loaded.value().file == dir / "t.zzz");
    fs::remove_all(dir);
}

TEST_CASE("unload empties the xref, keeps inserts and layer states, reload brings it back") {
    Document host;
    BlockId b;
    EntityId insertId;
    {
        Source src = sourceOf(makeSource(), 5);
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b, {100, 0}, {1, 1}, 0});
        insertId = tx.add(ins).value();
        tx.commit();
    }
    // Hide a layer of the xref: the state must survive unload and reload (VISRETAIN).
    LayerId walls = host.layerByName(host.layerByName("site")->id, "walls")->id;
    {
        Transaction tx = host.begin("hide");
        REQUIRE(tx.modifyLayer(walls, [](Layer& l) { l.on = false; }).ok());
        tx.commit();
    }
    {
        Transaction tx = host.begin("unload");
        REQUIRE(unload(tx, host, b).ok());
        tx.commit();
    }
    CHECK(host.block(b)->entities.empty());
    CHECK(host.block(b)->xrefPath == "site.tbcad");
    CHECK(host.find(insertId) != nullptr);
    CHECK(state(host, b, {}) == State::Unloaded);
    {
        Source src = sourceOf(makeSource(), 5);
        Transaction tx = host.begin("reload");
        REQUIRE(reload(tx, host, b, src).ok());
        tx.commit();
    }
    CHECK(host.block(b)->entities.size() == 4);
    CHECK_FALSE(host.layer(walls)->on);
    Transaction tx = host.begin("bad");
    CHECK_FALSE(unload(tx, host, BlockId{9999}).ok());
}

TEST_CASE("the insertion base of the source drawing becomes the block base") {
    Document host;
    auto srcDoc = makeSource();
    {
        Transaction tx = srcDoc->begin("base");
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.custom["tbcad.insbase"] = insertionBaseText({12.5, -3});
                  }).ok());
        tx.commit();
    }
    CHECK(insertionBase(*srcDoc) == Vec2{12.5, -3});
    CHECK(insertionBase(host) == Vec2{});
    Source src = sourceOf(std::move(srcDoc));
    Transaction tx = host.begin("import");
    auto r = importAsBlock(tx, host, "part", src);
    REQUIRE(r.ok());
    tx.commit();
    CHECK(host.block(r.value())->base == Vec2{12.5, -3});
}

TEST_CASE("bind with Insert merges the xref layers into the drawing's own and drops the prefixes") {
    Document host;
    LayerId hostWalls;
    {
        Transaction tx = host.begin("walls");
        Layer l;
        l.name = "walls";
        l.color = Color::index(1);
        hostWalls = tx.addLayer(l).value();
        tx.commit();
    }
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{b, {0, 0}, {1, 1}, 0});
        REQUIRE(tx.add(ins).ok());
        tx.commit();
    }
    REQUIRE(host.layerByName("site") != nullptr);
    {
        Transaction tx = host.begin("bind");
        REQUIRE(bind(tx, host, b, BindMode::Insert).ok());
        tx.commit();
    }
    CHECK(host.block(b)->xrefPath.empty());
    // The layers of the xref are gone; "walls" kept the host's colour, "inner" moved below it.
    CHECK(host.layerByName("site") == nullptr);
    const Layer* walls = host.layerByName("walls");
    REQUIRE(walls != nullptr);
    CHECK(walls->id == hostWalls);
    CHECK(walls->color == Color::index(1));
    const Layer* inner = host.layerByName(hostWalls, "inner");
    REQUIRE(inner != nullptr);
    // Nested block lost its prefix.
    CHECK(host.blockByName("site$door") == nullptr);
    const BlockDef* door = host.blockByName("door");
    REQUIRE(door != nullptr);
    // Everything that was on the xref's walls layers is on the host's now.
    std::size_t onWalls = 0, onInner = 0, onZero = 0;
    for (const auto& [id, blk] : host.blocks())
        for (EntityId eid : blk.entities) {
            const Entity* e = host.find(eid);
            onWalls += e->common.layer == hostWalls ? 1 : 0;
            onInner += e->common.layer == inner->id ? 1 : 0;
            onZero += e->common.layer == kLayerZero ? 1 : 0;
        }
    CHECK(onWalls >= 2);
    CHECK(onInner == 1);
    CHECK(onZero >= 1);
}

TEST_CASE("bind with Insert creates top-level layers when the drawing has none of that name") {
    Document host;
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        tx.commit();
    }
    {
        Transaction tx = host.begin("bind");
        REQUIRE(bind(tx, host, b, BindMode::Insert).ok());
        tx.commit();
    }
    const Layer* walls = host.layerByName("walls");
    REQUIRE(walls != nullptr);
    CHECK(walls->color == Color::index(3));  // the look it had
    CHECK(host.layerByName("site") == nullptr);
    const Layer* inner = host.layerByName(walls->id, "inner");
    REQUIRE(inner != nullptr);
    CHECK(inner->color.kind != ColorKind::ByParent);
}

TEST_CASE("changePath points the xref at another file and keeps the snapshot") {
    Document host;
    Source src = sourceOf(makeSource());
    BlockId b;
    {
        Transaction tx = host.begin("attach");
        b = attach(tx, host, "site", "site.tbcad", src).value();
        tx.commit();
    }
    const std::size_t count = host.block(b)->entities.size();
    {
        Transaction tx = host.begin("path");
        REQUIRE(changePath(tx, host, b, "new/site2.tbcad").ok());
        CHECK_FALSE(changePath(tx, host, b, "").ok());
        tx.commit();
    }
    CHECK(host.block(b)->xrefPath == "new/site2.tbcad");
    CHECK(host.block(b)->entities.size() == count);
    CHECK(state(host, b, {}) == State::Missing);
}

TEST_CASE("perf: attach, reload, bind and undo of an xref with 300000 entities") {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a) {
        return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
    };
    auto srcDoc = makeSource(300000);
    Document host;
    Source src = sourceOf(std::move(srcDoc));

    auto t0 = Clock::now();
    BlockId id;
    {
        Transaction tx = host.begin("attach");
        auto r = attach(tx, host, "site", "site.tbcad", src);
        REQUIRE(r.ok());
        id = r.value();
        tx.commit();
    }
    const double attachMs = ms(t0);

    const std::size_t members = host.block(id)->entities.size();
    t0 = Clock::now();
    ChangeSet reloaded;
    {
        Transaction tx = host.begin("reload");
        REQUIRE(reload(tx, host, id, src).ok());
        reloaded = tx.commit();
    }
    const double reloadMs = ms(t0);
    CHECK(host.block(id)->entities.size() == members);
    host.apply(reloaded, Direction::Undo);
    CHECK(host.block(id)->entities.size() == members);
    host.apply(reloaded, Direction::Redo);
    CHECK(host.block(id)->entities.size() == members);

    ChangeSet bindChanges;
    t0 = Clock::now();
    {
        Transaction tx = host.begin("bind");
        REQUIRE(bind(tx, host, id, BindMode::Insert).ok());
        bindChanges = tx.commit();
    }
    const double bindMs = ms(t0);

    t0 = Clock::now();
    host.apply(bindChanges, Direction::Undo);
    const double undoMs = ms(t0);
    std::printf("[perf] xref 300k: attach %.0f ms, reload %.0f ms, bind %.0f ms, undo %.0f ms\n",
                attachMs, reloadMs, bindMs, undoMs);
    CHECK(attachMs < 5000);
    CHECK(reloadMs < 5000);
    CHECK(bindMs < 5000);
    CHECK(undoMs < 5000);
}

TEST_CASE("open: an xref a DXF names is loaded from beside the drawing, a missing one stays") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "tbcad_xref_open_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        auto doc = makeSource();
        REQUIRE(io::writeTbcad(*doc, dir / "site.tbcad", io::WriteOptions{.keepBackup = false})
                    .ok());
    }
    auto xrefBlock = [](const char* name, const char* path) {
        return std::string("  0\nBLOCK\n  8\n0\n  2\n") + name + "\n 70\n4\n 10\n0\n 20\n0\n 30\n0\n  3\n" +
               name + "\n  1\n" + path + "\n  0\nENDBLK\n";
    };
    const std::string text = std::string("  0\nSECTION\n  2\nBLOCKS\n") +
                             xrefBlock("site", "C:\\old project\\xrefs\\site.tbcad") +
                             xrefBlock("gone", "gone.dwg") +
                             "  0\nENDSEC\n  0\nSECTION\n  2\nENTITIES\n"
                             "  0\nINSERT\n  8\n0\n  2\nsite\n 10\n0\n 20\n0\n"
                             "  0\nINSERT\n  8\n0\n  2\ngone\n 10\n0\n 20\n0\n"
                             "  0\nENDSEC\n  0\nEOF\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    dxf::Report report;
    auto imported = dxf::importDxf(bytes, {}, report);
    REQUIRE(imported.ok());
    Document& doc = *imported.value().document;
    const fs::path host = dir / "host.dxf";

    OpenLoad r = loadOnOpen(doc, host);
    CHECK(r.loaded == std::vector<std::string>{"site"});
    CHECK(r.missing == std::vector<std::string>{"gone"});
    CHECK(r.failed.empty());
    const BlockDef* site = doc.blockByName("site");
    REQUIRE(site);
    CHECK_FALSE(site->entities.empty());
    CHECK(site->xrefStamp != 0);
    CHECK(site->xrefPath == "C:\\old project\\xrefs\\site.tbcad");  // the stored path stays
    CHECK(state(doc, site->id, host) == State::UpToDate);
    const BlockDef* gone = doc.blockByName("gone");
    REQUIRE(gone);
    CHECK(gone->entities.empty());
    CHECK(state(doc, gone->id, host) == State::Unloaded);

    // Nothing left to load: a second pass does nothing.
    OpenLoad again = loadOnOpen(doc, host);
    CHECK(again.loaded.empty());
    CHECK(again.missing == std::vector<std::string>{"gone"});
    fs::remove_all(dir);
}
