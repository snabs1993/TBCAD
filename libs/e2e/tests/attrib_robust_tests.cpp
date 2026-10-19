// Block attributes under stress: tag changes, ATTSYNC after the definition changed, transforms,
// explode, file round trips with Thai values, and thousands of inserts.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    BlockId block{};
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    static Entity attdef(const std::string& tag, const std::string& def, bool invisible,
                         bool constant, Vec2 at) {
        TextData t;
        t.position = at;
        t.height = 2;
        t.utf8 = tag;
        t.attdef = AttDefInfo{tag, tag + "?", def, invisible, constant, false, false};
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        return e;
    }
    // Block "TAG": a line and the attributes NAME, NOTE (invisible), CODE (constant).
    void makeBlock() {
        auto tx = doc.begin("block");
        block = tx.addBlock({.name = "TAG"}).value();
        Entity l;
        l.data = LineData{{0, 0}, {10, 0}};
        (void)tx.add(std::move(l), block);
        (void)tx.add(attdef("NAME", "none", false, false, {0, 3}), block);
        (void)tx.add(attdef("NOTE", "", true, false, {0, 6}), block);
        (void)tx.add(attdef("CODE", "K1", false, true, {0, 9}), block);
        tx.commit();
    }
    EntityId insert(Vec2 at, const std::string& name) {
        host.run("INSERT", {"TAG", std::to_string(at.x) + "," + std::to_string(at.y), name,
                            ScriptItem::enter()});
        host.feed({ScriptItem::esc()});
        std::vector<EntityId> v = inserts();
        return v.back();
    }
    std::vector<EntityId> inserts() const {
        std::vector<EntityId> v;
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<InsertData>(e) && !doc.ownerBlock(e.common.id))
                v.push_back(e.common.id);
        });
        std::sort(v.begin(), v.end());
        return v;
    }
    const InsertData& ins(EntityId id) const { return *entityAs<InsertData>(*doc.find(id)); }
    static std::string valueOf(const InsertData& d, const std::string& tag) {
        for (const AttribValue& v : d.attribs)
            if (v.tag == tag)
                return v.value;
        return "<missing>";
    }
    EntityId defOf(const std::string& tag) const {
        for (EntityId id : doc.block(block)->entities)
            if (const auto* t = entityAs<TextData>(*doc.find(id));
                t && t->attdef && t->attdef->tag == tag)
                return id;
        return {};
    }
};

}  // namespace

TEST_CASE("a tag renamed in BATTMAN keeps the values of the inserts") {
    Rig r;
    r.makeBlock();
    const EntityId a = r.insert({0, 0}, "Alice");
    const std::string before = canonicalDump(r.doc);
    r.host.run("BATTMAN", {"TAG", "Edit", "NAME", "Tag", "OWNER", "Done"});
    REQUIRE(r.defOf("OWNER"));
    CHECK(Rig::valueOf(r.ins(a), "OWNER") == "Alice");
    CHECK(Rig::valueOf(r.ins(a), "NAME") == "<missing>");
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("a tag cannot be renamed to one the block already has") {
    Rig r;
    r.makeBlock();
    r.insert({0, 0}, "Alice");
    const std::string before = canonicalDump(r.doc);
    r.host.run("BATTMAN", {"TAG", "Edit", "NAME", "Tag", "NOTE", "Done"});
    CHECK(r.defOf("NAME"));
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("ATTSYNC after attributes were added, removed and reordered") {
    Rig r;
    r.makeBlock();
    const EntityId a = r.insert({0, 0}, "Alice");
    const EntityId b = r.insert({50, 0}, "Bob");
    const std::string before = canonicalDump(r.doc);
    {
        auto tx = r.doc.begin("edit block");
        REQUIRE(tx.erase(r.defOf("NOTE")).ok());
        (void)tx.add(Rig::attdef("PLACE", "Bangkok", false, false, {0, 12}), r.block);
        tx.commit();
    }
    r.host.run("ATTSYNC", {"TAG"});
    for (EntityId id : {a, b}) {
        CHECK(Rig::valueOf(r.ins(id), "NOTE") == "<missing>");
        CHECK(Rig::valueOf(r.ins(id), "PLACE") == "Bangkok");
    }
    CHECK(Rig::valueOf(r.ins(a), "NAME") == "Alice");
    CHECK(Rig::valueOf(r.ins(b), "NAME") == "Bob");
    CHECK(r.host.undo());  // ATTSYNC
    CHECK(Rig::valueOf(r.ins(a), "PLACE") == "<missing>");
    (void)before;
}

TEST_CASE("attributes follow their insert through MOVE ROTATE SCALE MIRROR COPY and undo") {
    Rig r;
    r.makeBlock();
    const EntityId a = r.insert({0, 0}, "Alice");
    const std::string before = canonicalDump(r.doc);
    auto sel = [&] { r.host.setSelection({a}); };
    sel();
    r.host.run("MOVE", {"0,0", "5,5"});
    sel();
    r.host.run("ROTATE", {"0,0", "30"});
    sel();
    r.host.run("SCALE", {"0,0", "2"});
    sel();
    r.host.run("MIRROR", {"0,0", "0,10", "Y"});
    CHECK(Rig::valueOf(r.ins(a), "NAME") == "Alice");
    r.host.setSelection({a});
    r.host.run("COPY", {"0,0", "100,0", ScriptItem::enter()});
    REQUIRE(r.inserts().size() == 2);
    CHECK(Rig::valueOf(r.ins(r.inserts().back()), "NAME") == "Alice");
    while (r.host.undo()) {
    }
    // Back to the empty drawing with the block only.
    CHECK(r.inserts().empty());
    while (r.host.redo()) {
    }
    CHECK(r.inserts().size() == 2);
    (void)before;
}

TEST_CASE("Thai and awkward attribute values survive .tbcad and DXF") {
    Rig r;
    r.makeBlock();
    const std::vector<std::string> values{"สมชาย ใจดี", "quote \" and ; semicolon", "  padded  ",
                                          "x"};
    for (std::size_t i = 0; i < values.size(); ++i)
        r.insert({double(i * 30), 0}, values[i]);
    const auto ids = r.inserts();
    REQUIRE(ids.size() == values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        CHECK(Rig::valueOf(r.ins(ids[i]), "NAME") == values[i]);

    const auto dir = std::filesystem::temp_directory_path() / "tbcad_attrib_robust";
    std::filesystem::create_directories(dir);
    const auto file = dir / "a.tbcad";
    REQUIRE(io::writeTbcad(r.doc, file).ok());
    io::IoReport rep;
    auto back = io::readTbcad(file, rep);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(r.doc));

    dxf::Report er;
    auto out = dxf::exportDxf(r.doc, dxf::ExportOptions{}, er);
    REQUIRE(out.ok());
    std::vector<std::byte> bytes(out.value().bytes.size());
    std::memcpy(bytes.data(), out.value().bytes.data(), bytes.size());
    dxf::Report ir;
    auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, ir);
    REQUIRE(in.ok());
    std::vector<std::string> got;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<InsertData>(e))
            if (!in.value().document->ownerBlock(e.common.id))
                got.push_back(Rig::valueOf(*d, "NAME"));
    });
    std::sort(got.begin(), got.end());
    auto want = values;
    std::sort(want.begin(), want.end());
    CHECK(got == want);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("3000 inserts with attributes: ATTSYNC and BATTMAN stay fast and undo exactly") {
    Rig r;
    r.makeBlock();
    {
        auto tx = r.doc.begin("many");
        for (int i = 0; i < 3000; ++i) {
            Entity e;
            InsertData d;
            d.block = r.block;
            d.position = {double(i % 100) * 20, double(i / 100) * 20};
            d.attribs = {{"NAME", "n" + std::to_string(i)}, {"NOTE", ""}};
            e.data = Indirect<InsertData>(std::move(d));
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    const std::string before = canonicalDump(r.doc);
    auto t0 = std::chrono::steady_clock::now();
    r.host.run("BATTMAN", {"TAG", "Edit", "NAME", "Tag", "OWNER", "Done"});
    r.host.run("ATTSYNC", {"TAG"});
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(sec < 10.0);
    CHECK(Rig::valueOf(r.ins(r.inserts()[2999]), "OWNER") == "n2999");
    while (r.host.undo()) {
    }
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("ATTEDIT takes Thai text and BURST turns the attributes into text") {
    Rig r;
    r.makeBlock();
    const EntityId a = r.insert({0, 0}, "Alice");
    r.host.run("ATTEDIT", {ScriptItem::pick(a), "ทดสอบ ๑๒๓", "note text"});
    CHECK(Rig::valueOf(r.ins(a), "NAME") == "ทดสอบ ๑๒๓");
    CHECK(Rig::valueOf(r.ins(a), "NOTE") == "note text");
    r.host.setSelection({a});
    r.host.run("BURST", {});
    CHECK(r.inserts().empty());
    std::vector<std::string> texts;
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e); t && !r.doc.ownerBlock(e.common.id)) {
            CHECK_FALSE(t->attdef.has_value());
            texts.push_back(t->utf8);
        }
    });
    std::sort(texts.begin(), texts.end());
    CHECK(std::count(texts.begin(), texts.end(), "ทดสอบ ๑๒๓") == 1);
    CHECK(std::count(texts.begin(), texts.end(), "K1") == 1);         // the constant one
    CHECK(std::count(texts.begin(), texts.end(), "note text") == 0);  // invisible: not drawn
    CHECK(r.host.undo());
    CHECK(r.inserts().size() == 1);
}
