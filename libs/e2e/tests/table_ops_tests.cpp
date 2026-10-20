// PURGE, RENAME and LAYMRG with tables that are used only inside block definitions, nested
// blocks and external references.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    // Tables used only by what is inside the blocks.
    LayerId layerInBlock{};
    LinetypeId ltInBlock{};
    TextStyleId styleInBlock{};
    DimStyleId dimInBlock{};
    BlockId inner{}, outer{};
    EntityId outerInsert{};

    void build() {
        Transaction tx = doc.begin("setup");
        Linetype lt;
        lt.name = "DASHEDX";
        lt.pattern = {5, -2};
        ltInBlock = tx.addLinetype(lt).value();
        Layer l;
        l.name = "OnlyInBlock";
        l.linetype = ltInBlock;
        layerInBlock = tx.addLayer(l).value();
        TextStyle ts;
        ts.name = "OnlyInBlockStyle";
        styleInBlock = tx.addTextStyle(ts).value();
        DimStyle ds;
        ds.name = "OnlyInBlockDim";
        dimInBlock = tx.addDimStyle(ds).value();

        BlockDef in;
        in.name = "Inner";
        inner = tx.addBlock(in).value();
        Entity line{{}, LineData{{0, 0}, {10, 0}}};
        line.common.layer = layerInBlock;
        REQUIRE(tx.add(std::move(line), inner).ok());
        TextData t;
        t.position = {0, 5};
        t.alignPoint = t.position;
        t.utf8 = "x";
        t.style = styleInBlock;
        REQUIRE(tx.add(Entity{{}, Indirect<TextData>(t)}, inner).ok());
        DimensionData dd;
        dd.style = dimInBlock;
        dd.defPts = {Vec2{0, 0}, Vec2{10, 0}, Vec2{0, 3}, Vec2{0, 0}};
        dd.textPos = {5, 3};
        REQUIRE(tx.add(Entity{{}, Indirect<DimensionData>(dd)}, inner).ok());

        BlockDef out;
        out.name = "Outer";
        outer = tx.addBlock(out).value();
        InsertData innerIns;
        innerIns.block = inner;
        innerIns.position = {5, 5};
        REQUIRE(tx.add(Entity{{}, innerIns}, outer).ok());

        InsertData ins;
        ins.block = outer;
        ins.position = {100, 100};
        outerInsert = tx.add(Entity{{}, ins}).value();
        tx.commit();
    }
};

}  // namespace

TEST_CASE("PURGE keeps tables used only inside nested block definitions, then removes all of it") {
    Rig r;
    r.build();
    const std::string before = canonicalDump(r.doc);
    r.host.run("-PURGE", {"", "Yes"});
    // Everything is still reachable from the Outer insert.
    CHECK(r.doc.layerByName("OnlyInBlock") != nullptr);
    CHECK(r.doc.linetypeByName("DASHEDX") != nullptr);
    CHECK(r.doc.textStyleByName("OnlyInBlockStyle") != nullptr);
    CHECK(r.doc.dimStyleByName("OnlyInBlockDim") != nullptr);
    CHECK(r.doc.blockByName("Inner") != nullptr);
    CHECK(r.doc.blockByName("Outer") != nullptr);
    CHECK(canonicalDump(r.doc) == before);

    // Erase the one insert: now nothing uses Outer, Inner or what is inside them.
    r.host.setSelection({r.outerInsert});
    r.host.run("ERASE", {});
    const std::string erased = canonicalDump(r.doc);
    r.host.run("-PURGE", {"", "Yes"});
    CHECK(r.doc.blockByName("Outer") == nullptr);
    CHECK(r.doc.blockByName("Inner") == nullptr);
    CHECK(r.doc.layerByName("OnlyInBlock") == nullptr);
    CHECK(r.doc.linetypeByName("DASHEDX") == nullptr);
    CHECK(r.doc.textStyleByName("OnlyInBlockStyle") == nullptr);
    CHECK(r.doc.dimStyleByName("OnlyInBlockDim") == nullptr);
    // One undo brings every row back, exactly.
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == erased);
}

TEST_CASE("RENAME of a block, a layer and styles used inside blocks keeps every link") {
    Rig r;
    r.build();
    r.host.run("RENAME", {"Layer", "OnlyInBlock", "Renamed"});
    r.host.run("RENAME", {"Block", "Inner", "InnerRenamed"});
    r.host.run("RENAME", {"Textstyle", "OnlyInBlockStyle", "TS2"});
    r.host.run("RENAME", {"Linetype", "DASHEDX", "LT2"});
    CHECK(r.doc.layerByName("Renamed") != nullptr);
    CHECK(r.doc.blockByName("InnerRenamed") != nullptr);
    CHECK(r.doc.textStyleByName("TS2") != nullptr);
    CHECK(r.doc.linetypeByName("LT2") != nullptr);
    // The links are by id, so PURGE still finds everything in use.
    const std::string renamed = canonicalDump(r.doc);
    r.host.run("-PURGE", {"", "Yes"});
    CHECK(canonicalDump(r.doc) == renamed);
    for (int i = 0; i < 4; ++i)
        CHECK(r.host.undo());
    CHECK(r.doc.layerByName("OnlyInBlock") != nullptr);
    CHECK(r.doc.blockByName("Inner") != nullptr);
}

TEST_CASE("LAYMRG moves objects inside block definitions and undoes exactly") {
    Rig r;
    r.build();
    EntityId onZero;
    {
        Transaction tx = r.doc.begin("setup");
        onZero = tx.add(Entity{{}, LineData{{0, 0}, {1, 1}}}).value();
        tx.commit();
    }
    EntityId inBlockLine{};
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && e.common.layer == r.layerInBlock)
            inBlockLine = e.common.id;
    });
    // forEachEntity may not visit block contents; fall back to the block's list.
    if (!inBlockLine) {
        for (EntityId id : r.doc.blocks().at(r.inner).entities)
            if (const Entity* e = r.doc.find(id); e && e->common.layer == r.layerInBlock)
                inBlockLine = id;
    }
    REQUIRE(inBlockLine);
    const std::string before = canonicalDump(r.doc);
    r.host.run("LAYMRG", {ScriptItem::select({inBlockLine}), ScriptItem::enter(),
                          ScriptItem::pick(onZero, {0, 0})});
    const Entity* moved = r.doc.find(inBlockLine);
    REQUIRE(moved != nullptr);
    CHECK(moved->common.layer == r.doc.find(onZero)->common.layer);
    CHECK(r.doc.layerByName("OnlyInBlock") == nullptr);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("PURGE removes an unreferenced xref block and keeps an inserted one") {
    Rig r;
    BlockId used, unused;
    EntityId insert;
    {
        Transaction tx = r.doc.begin("setup");
        BlockDef a;
        a.name = "RefA";
        a.xrefPath = "a.tbcad";
        used = tx.addBlock(a).value();
        BlockDef b;
        b.name = "RefB";
        b.xrefPath = "b.tbcad";
        unused = tx.addBlock(b).value();
        InsertData ins;
        ins.block = used;
        insert = tx.add(Entity{{}, ins}).value();
        tx.commit();
    }
    (void)insert;
    const std::string before = canonicalDump(r.doc);
    r.host.run("-PURGE", {"Blocks", "Yes"});
    CHECK(r.doc.blockByName("RefA") != nullptr);
    CHECK(r.doc.blockByName("RefB") == nullptr);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
    (void)unused;
}

namespace {

std::filesystem::path writeXrefSource(const std::filesystem::path& dir) {
    Document d;
    {
        Transaction tx = d.begin("source");
        Layer l;
        l.name = "walls";
        LayerId w = tx.addLayer(l).value();
        for (int i = 0; i < 3; ++i) {
            Entity e{{}, LineData{{double(i), 0}, {double(i), 10}}};
            e.common.layer = w;
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    const auto file = dir / "plan.tbcad";
    REQUIRE(io::writeTbcad(d, file, io::WriteOptions{.keepBackup = false}).ok());
    return file;
}

}  // namespace

TEST_CASE("xref layers: PURGE, RENAME and LAYMRG leave an attached xref intact and undo exactly") {
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_table_ops_xref";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = writeXrefSource(dir);

    Rig r;
    EntityId mine;
    {
        Transaction tx = r.doc.begin("setup");
        mine = tx.add(Entity{{}, LineData{{0, 0}, {5, 5}}}).value();
        tx.commit();
    }
    r.host.run("XATTACH", {file.string(), "", "", "", "10,20"});
    const BlockDef* def = r.doc.blockByName("plan");
    REQUIRE(def != nullptr);
    const std::string attached = canonicalDump(r.doc);
    const std::size_t layers = r.doc.layers().size();

    // PURGE must keep the xref block and the layers its objects are on.
    r.host.run("-PURGE", {"", "Yes"});
    CHECK(r.doc.blockByName("plan") != nullptr);
    CHECK(r.doc.layers().size() == layers);

    // Find a layer the xref brought in and try to rename and merge it.
    std::string xrefLayer;
    for (const auto& [id, row] : r.doc.layers())
        if (row.name != "0" && row.name.find("walls") != std::string::npos)
            xrefLayer = row.name;
    REQUIRE_FALSE(xrefLayer.empty());
    const std::string before = canonicalDump(r.doc);
    r.host.run("RENAME", {"Layer", xrefLayer, "Mine"});
    EntityId anyXrefEntity{};
    for (EntityId id : r.doc.blockByName("plan")->entities)
        anyXrefEntity = id;
    r.host.run("LAYMRG", {ScriptItem::select({anyXrefEntity}), ScriptItem::enter(),
                          ScriptItem::pick(mine, {0, 0})});
    // Whatever the commands decided, the drawing is consistent, saves, and undo restores it.
    const std::string now = canonicalDump(r.doc);
    {
        const auto out = dir / "out.tbcad";
        REQUIRE(io::writeTbcad(r.doc, out).ok());
        io::IoReport report;
        auto back = io::readTbcad(out, report);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == now);
    }
    while (r.host.undoStack().undoCount() > 1)
        REQUIRE(r.host.undo());
    CHECK(canonicalDump(r.doc) == attached);
    std::filesystem::remove_all(dir);
    (void)before;
}
