// Associative dimensions through the editing commands, undo and the file formats.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    EntityId line{}, dim{};
    Rig() : host(doc, (registerAllCommands(reg), reg)) {
        {
            auto tx = doc.begin("setup");
            Entity e;
            e.data = LineData{{0, 0}, {100, 0}};
            line = tx.add(std::move(e)).value();
            tx.commit();
        }
        REQUIRE(host.run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                dim = e.common.id;
        });
        REQUIRE(dim);
    }
    const DimensionData& d() const { return *entityAs<DimensionData>(*doc.find(dim)); }
    const LineData& l() const { return *entityAs<LineData>(*doc.find(line)); }
    // The dimension's measured points are the line's ends.
    void checkFollows() {
        CHECK(distance(d().defPts[0], l().a) < 1e-9);
        CHECK(distance(d().defPts[1], l().b) < 1e-9);
        const Vec2 axis{std::cos(d().rotation), std::sin(d().rotation)};
        CHECK(d().measurement == doctest::Approx(std::abs(dot(l().b - l().a, axis))));
    }
};

}  // namespace

TEST_CASE("the dimension follows its line through MOVE, ROTATE, SCALE, MIRROR and STRETCH") {
    Rig r;
    const std::size_t base = r.host.undoStack().undoCount();
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("MOVE", {"0,0", "10,5"}).finished);
    r.checkFollows();
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("ROTATE", {"10,5", "45"}).finished);
    r.checkFollows();
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("SCALE", {"10,5", "2"}).finished);
    r.checkFollows();
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("MIRROR", {"0,0", "0,10", "No"}).finished);
    r.checkFollows();
    // Stretch the line's end by a crossing window around it.
    const Vec2 b = r.l().b;
    r.host.setSelection({});
    (void)r.host.run("STRETCH", {std::to_string(b.x - 1) + "," + std::to_string(b.y - 1),
                                 std::to_string(b.x + 1) + "," + std::to_string(b.y + 1),
                                 std::to_string(b.x) + "," + std::to_string(b.y), "300,300"});
    r.checkFollows();
    // Each step is one undo step and walks the dimension back with the line.
    while (r.host.undoStack().undoCount() > base)
        REQUIRE(r.host.undo());
    CHECK(r.l().b.x == doctest::Approx(100));
    r.checkFollows();
}

TEST_CASE("moving the line together with its dimension keeps the link") {
    Rig r;
    r.host.setSelection({r.line, r.dim});
    REQUIRE(r.host.run("MOVE", {"0,0", "40,10"}).finished);
    r.checkFollows();
    CHECK(r.d().assoc[0].kind == DimAssoc::Kind::Vertex);
    CHECK(r.d().assoc[0].target == r.line);
    r.host.setSelection({r.line, r.dim});
    REQUIRE(r.host.run("ROTATE", {"0,0", "90"}).finished);
    r.checkFollows();
    r.host.setSelection({r.line, r.dim});
    REQUIRE(r.host.run("SCALE", {"0,0", "0.5"}).finished);
    r.checkFollows();
    r.host.setSelection({r.line, r.dim});
    REQUIRE(r.host.run("MIRROR", {"0,0", "10,0", "No"}).finished);
    r.checkFollows();
}

TEST_CASE("erasing the line leaves the dimension drawn, undo brings the link back") {
    Rig r;
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("ERASE", {}).finished);
    REQUIRE(r.doc.find(r.dim));
    CHECK(distance(r.d().defPts[0], Vec2{0, 0}) < 1e-9);
    CHECK(distance(r.d().defPts[1], Vec2{100, 0}) < 1e-9);
    CHECK(r.host.undo());
    REQUIRE(r.doc.find(r.line));
    CHECK(r.d().assoc[0].target == r.line);
    r.host.setSelection({r.line});
    (void)r.host.run("MOVE", {"0,0", "0,50"});
    r.checkFollows();
}

TEST_CASE("DIMDISASSOC and DIMREASSOC with edits in between") {
    Rig r;
    r.host.run("DIMDISASSOC", {ScriptItem::select({r.dim}), ScriptItem::enter()});
    CHECK(r.d().assoc[0].kind == DimAssoc::Kind::None);
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("MOVE", {"0,0", "0,10"}).finished);
    // The dimension stayed where it was.
    CHECK(distance(r.d().defPts[0], Vec2{0, 0}) < 1e-9);
    // Move the line back and relink: the points sit on its ends again.
    r.host.setSelection({r.line});
    REQUIRE(r.host.run("MOVE", {"0,10", "0,0"}).finished);
    r.host.run("DIMREASSOC", {ScriptItem::select({r.dim}), ScriptItem::enter()});
    CHECK(r.d().assoc[0].target == r.line);
    CHECK(r.d().assoc[1].index == 1);
    r.host.setSelection({r.line});
    (void)r.host.run("MOVE", {"0,0", "7,7"});
    r.checkFollows();
    // Undo the move, undo the relink: the link is gone again.
    CHECK(r.host.undo());
    CHECK(r.host.undo());
    CHECK(r.d().assoc[0].kind == DimAssoc::Kind::None);
}

TEST_CASE("copies of a dimension are not linked to the original, the link survives save and load") {
    Rig r;
    r.host.setSelection({r.line, r.dim});
    REQUIRE(r.host.run("COPY", {"0,0", "0,100", ScriptItem::enter()}).finished);
    int dims = 0;
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* dm = entityAs<DimensionData>(e)) {
            ++dims;
            if (e.common.id != r.dim)
                CHECK(dm->assoc[0].target != r.line);
        }
    });
    CHECK(dims == 2);
    CHECK(r.d().assoc[0].target == r.line);

    const auto file = std::filesystem::temp_directory_path() / "tbcad_dim_assoc.tbcad";
    REQUIRE(io::writeTbcad(r.doc, file).ok());
    io::IoReport rep;
    auto back = io::readTbcad(file, rep);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(r.doc));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE(
    "a dimension inside a block follows the block's line; exploding an insert keeps it sane") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    BlockId blk{};
    EntityId line{}, dim{};
    {
        auto tx = doc.begin("block");
        BlockDef def;
        def.name = "BEAM";
        blk = tx.addBlock(def).value();
        Entity l;
        l.data = LineData{{0, 0}, {100, 0}};
        line = tx.add(std::move(l), blk).value();
        DimensionData d;
        d.kind = DimKind::Linear;
        d.defPts = {Vec2{0, 0}, Vec2{100, 0}, Vec2{50, -20}, Vec2{}};
        const int linked = linkDimension(doc, d, blk);
        REQUIRE(linked == 2);
        Entity de;
        de.data = Indirect<DimensionData>(d);
        dim = tx.add(std::move(de), blk).value();
        InsertData ins;
        ins.block = blk;
        ins.position = {500, 500};
        Entity ie;
        ie.data = Indirect<InsertData>(ins);
        REQUIRE(tx.add(std::move(ie)).ok());
        tx.commit();
    }
    {
        auto tx = doc.begin("edit in block");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {160, 0}; }));
        tx.commit();
    }
    const auto& d = *entityAs<DimensionData>(*doc.find(dim));
    CHECK(d.defPts[1].x == doctest::Approx(160));
    CHECK(d.measurement == doctest::Approx(160));

    // Explode the insert: the loose dimension must not point into the block definition.
    EntityId insert{};
    doc.forEachEntity([&](const Entity& e) {
        if (entityAs<InsertData>(e))
            insert = e.common.id;
    });
    host.setSelection({insert});
    REQUIRE(host.run("EXPLODE", {}).finished);
    int loose = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (doc.ownerBlock(e.common.id) != BlockId{})
            return;
        if (const auto* dm = entityAs<DimensionData>(e)) {
            ++loose;
            for (const DimAssoc& a : dm->assoc)
                if (a.kind != DimAssoc::Kind::None)
                    CHECK(a.target != line);
            CHECK(dm->defPts[0].x == doctest::Approx(500));
            CHECK(dm->defPts[1].x == doctest::Approx(660));
        }
    });
    CHECK(loose == 1);
}

TEST_CASE("grip edits of the line move the dimension with it, and undo walks both back") {
    Rig r;
    const std::size_t base = r.host.undoStack().undoCount();
    r.host.setSelection({r.line});
    // Stretch the end grip.
    REQUIRE(r.host.run("GRIPEDIT", {"100,0", "140,30"}).finished);
    CHECK(r.l().b.x == doctest::Approx(140));
    r.checkFollows();
    CHECK(r.host.undoStack().undoCount() == base + 1);
    // Stretch the start grip.
    REQUIRE(r.host.run("GRIPEDIT", {"0,0", "-20,-10"}).finished);
    r.checkFollows();
    // Same through the Move mode of the middle grip.
    r.host.setSelection({r.line});
    const Vec2 mid = (r.l().a + r.l().b) * 0.5;
    (void)r.host.run("GRIPEDIT", {std::to_string(mid.x) + "," + std::to_string(mid.y), "Move",
                                  std::to_string(mid.x + 15) + "," + std::to_string(mid.y + 9)});
    r.checkFollows();
    while (r.host.undoStack().undoCount() > base)
        REQUIRE(r.host.undo());
    CHECK(r.l().b.x == doctest::Approx(100));
    CHECK(r.l().a.x == doctest::Approx(0));
    r.checkFollows();
    while (r.host.redo()) {
    }
    r.checkFollows();
}

TEST_CASE("grip edit of the dimension's own text moves only the text") {
    Rig r;
    const Vec2 a = r.l().a, b = r.l().b;
    r.host.setSelection({r.dim});
    const Vec2 text = r.d().defPts.size() > 4 ? r.d().defPts[4] : Vec2{50, -30};
    (void)r.host.run("GRIPEDIT", {std::to_string(text.x) + "," + std::to_string(text.y),
                                  std::to_string(text.x + 5) + "," + std::to_string(text.y - 7)});
    CHECK(distance(r.l().a, a) < 1e-9);
    CHECK(distance(r.l().b, b) < 1e-9);
    r.checkFollows();
}

TEST_CASE("radius and diameter dimensions follow their circle through MOVE and SCALE") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    EntityId circle{};
    {
        auto tx = doc.begin("setup");
        Entity e;
        e.data = CircleData{{0, 0}, 10};
        circle = tx.add(std::move(e)).value();
        tx.commit();
    }
    REQUIRE(host.run("DIMRADIUS", {ScriptItem::pick(circle, {10, 0}), "20,5"}).finished);
    REQUIRE(host.run("DIMDIAMETER", {ScriptItem::pick(circle, {0, 10}), "-20,5"}).finished);
    std::vector<EntityId> dims;
    doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dims.push_back(e.common.id);
    });
    REQUIRE(dims.size() == 2);
    auto dim = [&](std::size_t i) { return *entityAs<DimensionData>(*doc.find(dims[i])); };
    for (std::size_t i = 0; i < 2; ++i)
        CHECK(dim(i).assoc[1].kind == DimAssoc::Kind::OnCurve);
    CHECK(dim(0).assoc[0].kind == DimAssoc::Kind::Center);
    host.setSelection({circle});
    REQUIRE(host.run("MOVE", {"0,0", "100,50"}).finished);
    host.setSelection({circle});
    REQUIRE(host.run("SCALE", {"100,50", "2"}).finished);
    CHECK(dim(0).measurement == doctest::Approx(20));
    CHECK(dim(1).measurement == doctest::Approx(40));
    CHECK(distance(dim(0).defPts[0], {100, 50}) < 1e-9);
    CHECK(distance(dim(0).defPts[1], {100, 50}) == doctest::Approx(20));
    // The link survives a .tbcad round trip.
    const auto path = std::filesystem::temp_directory_path() / "tbcad_radial_assoc.tbcad";
    REQUIRE(io::writeTbcad(doc, path).ok());
    io::IoReport rep;
    auto back = io::readTbcad(path, rep);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(doc));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
