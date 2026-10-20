// STRETCH with a crossing window over the kinds AutoCAD treats specially: hatches (associative or
// not), block inserts, dimensions, multilines, tables and construction lines.
#include <doctest/doctest.h>

#include <string>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(d)});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    template <class T>
    const T& get(EntityId id) const {
        const Entity* e = doc.find(id);
        REQUIRE(e != nullptr);
        const T* d = entityAs<T>(*e);
        REQUIRE(d != nullptr);
        return *d;
    }
    // STRETCH with the crossing window (x0,y0)-(x1,y1), from base to base + d.
    void stretch(Vec2 w0, Vec2 w1, Vec2 d) {
        auto s = [](Vec2 p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
        REQUIRE(host.run("STRETCH", {s(w0), s(w1), "0,0", s(d)}).finished);
    }
};

HatchData boxHatch() {
    HatchData h;
    HatchLoop loop;
    HatchPolylineEdge pl;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}})
        pl.v.push_back(PolyVertex{.p = p});
    loop.edges.push_back(pl);
    h.loops.push_back(loop);
    return h;
}

}  // namespace

TEST_CASE("STRETCH moves the outline corners of a hatch inside the window, or the whole hatch") {
    Rig r;
    const EntityId h = r.add(boxHatch());
    const std::string before = canonicalDump(r.doc);
    r.stretch({80, -10}, {120, 60}, {20, 0});
    BBox2 b = hatch::hatchBounds(r.get<HatchData>(h));
    CHECK(b.min.x == doctest::Approx(0));
    CHECK(b.max.x == doctest::Approx(120));
    CHECK(b.max.y == doctest::Approx(50));
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    r.stretch({-10, -10}, {110, 60}, {5, 5});  // everything inside: the hatch moves
    b = hatch::hatchBounds(r.get<HatchData>(h));
    CHECK(b.min.x == doctest::Approx(5));
    CHECK(b.max.y == doctest::Approx(55));
}

TEST_CASE(
    "STRETCH of an associative hatch's boundary updates the hatch, and it stays associative") {
    Rig r;
    REQUIRE(r.host.run("RECTANG", {"0,0", "100,50"}).finished);
    REQUIRE(r.host.run("HATCH", {"50,25", ScriptItem::enter()}).finished);
    EntityId hid{};
    r.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<HatchData>(e))
            hid = e.common.id;
    });
    REQUIRE(hid);
    REQUIRE_FALSE(r.get<HatchData>(hid).sourceBoundary.empty());
    const std::string before = canonicalDump(r.doc);
    // The window takes the right side of the rectangle and the hatch's corners there.
    r.stretch({80, -10}, {120, 60}, {30, 0});
    const HatchData& h = r.get<HatchData>(hid);
    CHECK(hatch::hatchBounds(h).max.x == doctest::Approx(130));
    CHECK_FALSE(h.sourceBoundary.empty());
    // Moving the rectangle again keeps the hatch with it
    r.stretch({-10, -10}, {140, 60}, {0, 10});
    CHECK(hatch::hatchBounds(r.get<HatchData>(hid)).min.y == doctest::Approx(10));
    CHECK(r.host.undo());
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("STRETCH moves a block insert only when its insertion point is inside the window") {
    Rig r;
    EntityId ins{};
    {
        Transaction tx = r.doc.begin("block");
        const BlockId blk = tx.addBlock(BlockDef{.name = "B1"}).value();
        REQUIRE(tx.add(Entity{{}, LineData{{0, 0}, {10, 10}}}, blk).ok());
        InsertData d;
        d.block = blk;
        d.position = {100, 100};
        ins = tx.add(Entity{{}, d}).value();
        tx.commit();
    }
    // geometry inside, insertion point outside: stays
    r.stretch({105, 105}, {120, 120}, {50, 0});
    CHECK(r.get<InsertData>(ins).position.x == doctest::Approx(100));
    // insertion point inside: moves whole
    r.stretch({95, 95}, {101, 101}, {50, 0});
    CHECK(r.get<InsertData>(ins).position.x == doctest::Approx(150));
}

TEST_CASE("STRETCH moves the dimension points inside the window and the measurement follows") {
    Rig r;
    // a free dimension (no line under it): only its own definition points
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dim{};
    r.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dim = e.common.id;
    });
    REQUIRE(dim);
    r.stretch({90, -40}, {110, 10}, {20, 0});
    const DimensionData& d = r.get<DimensionData>(dim);
    CHECK(d.measurement == doctest::Approx(120));
    CHECK(d.defPts[0].x == doctest::Approx(0));
    // everything inside: the dimension moves, the measurement stays
    r.stretch({-10, -50}, {130, 10}, {0, 100});
    CHECK(r.get<DimensionData>(dim).measurement == doctest::Approx(120));
    CHECK(r.get<DimensionData>(dim).defPts[0].y == doctest::Approx(100));
}

TEST_CASE("STRETCH moves multiline vertices, tables and construction lines like AutoCAD") {
    Rig r;
    MlineData ml;
    ml.v = {{0, 0}, {100, 0}, {100, 50}};
    const EntityId m = r.add(ml);
    RayData xl;
    xl.origin = {200, 0};
    xl.dir = {0, 1};
    xl.infinite = true;
    const EntityId x = r.add(xl);
    TableData t;
    t.origin = {300, 0};
    t.colWidths = {20, 20};
    t.rowHeights = {10};
    t.cells.resize(2);
    const EntityId tab = r.add(t);

    r.stretch({90, -10}, {110, 10}, {10, 0});  // one multiline vertex
    CHECK(r.get<MlineData>(m).v[1].x == doctest::Approx(110));
    CHECK(r.get<MlineData>(m).v[2].x == doctest::Approx(100));

    r.stretch({190, -10}, {210, 10}, {5, 0});  // the construction line's base point
    CHECK(r.get<RayData>(x).origin.x == doctest::Approx(205));

    r.stretch({290, -5}, {310, 5}, {0, 7});  // the table's insertion point
    CHECK(r.get<TableData>(tab).origin.y == doctest::Approx(7));
}
