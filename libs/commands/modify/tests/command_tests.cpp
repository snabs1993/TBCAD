#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/modify/modify.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;

    Fixture() : host(doc, reg) { registerAllCommands(reg); }

    EntityId add(EntityData d) {
        Entity e;
        e.data = std::move(d);
        Transaction tx = doc.begin("setup");
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    const Entity& get(EntityId id) {
        const Entity* e = doc.find(id);
        REQUIRE(e != nullptr);
        return *e;
    }
    std::size_t count(EntityKind k) {
        std::size_t n = 0;
        doc.forEachEntity([&](const Entity& e) { n += kind(e) == k ? 1 : 0; });
        return n;
    }
};

bool near(Vec2 a, Vec2 b, double tol = 1e-6) {
    return distance(a, b) <= tol;
}

}  // namespace

TEST_CASE("the modify commands are registered with their short names") {
    Fixture f;
    for (auto [alias, name] :
         std::vector<std::pair<const char*, const char*>>{{"O", "OFFSET"},
                                                          {"TR", "TRIM"},
                                                          {"EX", "EXTEND"},
                                                          {"F", "FILLET"},
                                                          {"CHA", "CHAMFER"},
                                                          {"BR", "BREAK"},
                                                          {"BAP", "BREAKATPOINT"},
                                                          {"J", "JOIN"},
                                                          {"E", "ERASE"}}) {
        REQUIRE(f.reg.find(alias) != nullptr);
        CHECK(f.reg.find(alias) == f.reg.find(name));
    }
}

TEST_CASE("OFFSET by distance, repeated, one undo step; Through; corner setting") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    auto r = f.host.run("OFFSET", {"2", ScriptItem::pick(l, {5, 0}), "5,9", ScriptItem::enter()});
    CHECK(r.finished);
    REQUIRE(f.count(EntityKind::Line) == 2);
    bool found = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* ld = entityAs<LineData>(e);
            ld && near(ld->a, {0, 2}) && near(ld->b, {10, 2}))
            found = true;
    });
    CHECK(found);
    CHECK(f.host.undoStack().undoLabel() == "OFFSET");
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 1);

    // The distance is remembered: Enter accepts it. The other side with a point below.
    auto r2 = f.host.run("OFFSET", {"", ScriptItem::pick(l, {5, 0}), "5,-9", ScriptItem::esc()});
    CHECK(r2.finished);
    CHECK(f.count(EntityKind::Line) == 2);  // Esc keeps what was made
    bool below = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* ld = entityAs<LineData>(e); ld && near(ld->a, {0, -2}))
            below = true;
    });
    CHECK(below);

    // Through a point.
    EntityId c = f.add(CircleData{{50, 50}, 5});
    f.host.run("OFFSET", {"T", ScriptItem::pick(c, {55, 50}), "50,62", ScriptItem::enter()});
    CHECK(f.count(EntityKind::Circle) == 2);

    // Rounded corners on request.
    EntityId p = f.add(PolylineData{
        {{{100, 0}, 0, 0, 0}, {{110, 0}, 0, 0, 0}, {{110, 10}, 0, 0, 0}, {{100, 10}, 0, 0, 0}},
        true,
        0});
    f.host.run("OFFSET",
               {"C", "R", "1", ScriptItem::pick(p, {105, 0}), "105,-5", ScriptItem::enter()});
    std::size_t withArcs = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* pl = entityAs<PolylineData>(e)) {
            for (const auto& v : pl->v)
                withArcs += std::abs(v.bulge) > 1e-9 ? 1 : 0;
        }
    });
    CHECK(withArcs == 4);
}

TEST_CASE("OFFSET reports a collapse and keeps going") {
    Fixture f;
    EntityId c = f.add(CircleData{{0, 0}, 5});
    f.host.run("OFFSET", {"8", ScriptItem::pick(c, {5, 0}), "0,0", ScriptItem::esc()});
    CHECK(f.count(EntityKind::Circle) == 1);
    CHECK(f.host.logContains("collapses"));
}

TEST_CASE("TRIM in fast mode, Shift extends, Undo and Edge") {
    Fixture f;
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    f.add(LineData{{3, -5}, {3, 5}});
    f.add(LineData{{7, -5}, {7, 5}});
    auto r =
        f.host.run("TRIM", {ScriptItem::enter(), ScriptItem::pick(h, {5, 0}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.count(EntityKind::Line) == 4);  // the horizontal became two
    CHECK(near(entityAs<LineData>(f.get(h))->b, {3, 0}));
    CHECK(f.host.undoStack().undoLabel() == "TRIM");
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 3);
    CHECK(near(entityAs<LineData>(f.get(h))->b, {10, 0}));

    // The command's own Undo option.
    f.host.run("TRIM",
               {ScriptItem::enter(), ScriptItem::pick(h, {5, 0}), "U", ScriptItem::enter()});
    CHECK(f.count(EntityKind::Line) == 3);

    // Shift-select extends instead.
    EntityId s = f.add(LineData{{0, 20}, {2, 20}});
    f.add(LineData{{5, 15}, {5, 25}});
    f.host.run("TRIM",
               {ScriptItem::enter(), ScriptItem::pick(s, {1.5, 20}, true), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(s))->b, {5, 20}));
}

TEST_CASE("TRIM with chosen cutting edges, and implied edge extension") {
    Fixture f;
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    EntityId edge = f.add(LineData{{5, 1}, {5, 5}});  // stops short of the line
    f.add(LineData{{8, -5}, {8, 5}});
    // Only `edge` is a cutting edge and it is too short: nothing happens until Edge: Extend.
    f.host.run("TRIM", {ScriptItem::select({edge}), ScriptItem::enter(),
                        ScriptItem::pick(h, {2, 0}), ScriptItem::esc()});
    CHECK(f.count(EntityKind::Line) == 3);
    f.host.run("TRIM", {ScriptItem::select({edge}), ScriptItem::enter(), "E", "E",
                        ScriptItem::pick(h, {2, 0}), ScriptItem::esc()});
    CHECK(near(entityAs<LineData>(f.get(h))->a, {5, 0}));
}

TEST_CASE("TRIM Fence and Crossing") {
    Fixture f;
    EntityId h1 = f.add(LineData{{0, 0}, {10, 0}});
    EntityId h2 = f.add(LineData{{0, 2}, {10, 2}});
    f.add(LineData{{3, -5}, {3, 5}});
    f.add(LineData{{7, -5}, {7, 5}});
    // A fence across the middle of both horizontals removes both middles.
    f.host.run("TRIM",
               {ScriptItem::enter(), "F", "5,-1", "5,3", ScriptItem::enter(), ScriptItem::esc()});
    CHECK(near(entityAs<LineData>(f.get(h1))->b, {3, 0}));
    CHECK(near(entityAs<LineData>(f.get(h2))->b, {3, 2}));
    CHECK(f.count(EntityKind::Line) == 6);

    Fixture g;
    EntityId v = g.add(LineData{{0, 0}, {10, 0}});
    g.add(LineData{{3, -5}, {3, 5}});
    g.add(LineData{{7, -5}, {7, 5}});
    g.host.run("TRIM", {ScriptItem::enter(), "C", "4,-1", "6,1", ScriptItem::esc()});
    CHECK(g.count(EntityKind::Line) == 4);
    CHECK(near(entityAs<LineData>(g.get(v))->b, {3, 0}));
}

TEST_CASE("EXTEND to a boundary, Shift trims") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {5, 0}});
    f.add(LineData{{9, -3}, {9, 3}});
    auto r = f.host.run("EXTEND",
                        {ScriptItem::enter(), ScriptItem::pick(l, {4, 0}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(near(entityAs<LineData>(f.get(l))->b, {9, 0}));
    // Shift-select trims at the boundary instead.
    EntityId m = f.add(LineData{{0, 10}, {12, 10}});
    f.add(LineData{{9, 5}, {9, 15}});
    f.host.run("EXTEND",
               {ScriptItem::enter(), ScriptItem::pick(m, {11, 10}, true), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(m))->b, {9, 10}));
    // Nothing to extend to: a message, the command goes on.
    EntityId lone = f.add(LineData{{0, 50}, {5, 50}});
    f.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(lone, {4, 50}), ScriptItem::esc()});
    CHECK(f.host.logContains("No boundary"));
}

TEST_CASE("FILLET with a radius, trim modes, Shift for a sharp corner, polylines") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 0}, {0, 10}});
    f.host.run("FILLET", {"R", "5", ScriptItem::pick(a, {8, 0}), ScriptItem::pick(b, {0, 8})});
    REQUIRE(f.count(EntityKind::Arc) == 1);
    CHECK(near(entityAs<LineData>(f.get(a))->a, {5, 0}));
    CHECK(near(entityAs<LineData>(f.get(b))->a, {0, 5}));
    CHECK(f.host.undoStack().undoLabel() == "FILLET");
    f.host.undo();
    CHECK(f.count(EntityKind::Arc) == 0);
    CHECK(near(entityAs<LineData>(f.get(a))->a, {0, 0}));

    // No trim: the arc is added, the lines stay.
    f.host.run("FILLET", {"T", "N", ScriptItem::pick(a, {8, 0}), ScriptItem::pick(b, {0, 8})});
    CHECK(f.count(EntityKind::Arc) == 1);
    CHECK(near(entityAs<LineData>(f.get(a))->a, {0, 0}));
    f.host.undo();
    f.host.run("FILLET", {"T", "T"});
    f.host.run("FILLET", {ScriptItem::esc()});

    // Shift on the second pick: a sharp corner even with a radius.
    Fixture g;
    EntityId c = g.add(LineData{{0, 0}, {4, 0}});
    EntityId d = g.add(LineData{{6, -3}, {6, 5}});
    g.host.run("FILLET",
               {"R", "5", ScriptItem::pick(c, {3, 0}), ScriptItem::pick(d, {6, 3}, true)});
    CHECK(g.count(EntityKind::Arc) == 0);
    CHECK(near(entityAs<LineData>(g.get(c))->b, {6, 0}));

    // Two segments of one polyline round their shared corner; the Polyline option does all.
    Fixture h;
    EntityId p = h.add(PolylineData{
        {{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 6}, 0, 0, 0}, {{0, 6}, 0, 0, 0}}, true, 0});
    h.host.run("FILLET", {"R", "2", ScriptItem::pick(p, {5, 0}), ScriptItem::pick(p, {10, 3})});
    CHECK(entityAs<PolylineData>(h.get(p))->v.size() == 5);
    h.host.run("FILLET", {"P", ScriptItem::pick(p, {5, 0})});
    CHECK(entityAs<PolylineData>(h.get(p))->v.size() == 8);
}

TEST_CASE("CHAMFER distances, angle method, polyline corner") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {20, 0}});
    EntityId b = f.add(LineData{{0, 0}, {0, 20}});
    f.host.run("CHAMFER",
               {"D", "5", "5", ScriptItem::pick(a, {15, 0}), ScriptItem::pick(b, {0, 15})});
    REQUIRE(f.count(EntityKind::Line) == 3);
    CHECK(near(entityAs<LineData>(f.get(a))->a, {5, 0}));
    CHECK(near(entityAs<LineData>(f.get(b))->a, {0, 5}));
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 2);

    f.host.run("CHAMFER",
               {"A", "4", "45", ScriptItem::pick(a, {15, 0}), ScriptItem::pick(b, {0, 15})});
    CHECK(near(entityAs<LineData>(f.get(a))->a, {4, 0}));
    CHECK(near(entityAs<LineData>(f.get(b))->a, {0, 4}));

    Fixture g;
    EntityId p =
        g.add(PolylineData{{{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 6}, 0, 0, 0}}, false, 0});
    g.host.run("CHAMFER",
               {"D", "2", "2", ScriptItem::pick(p, {5, 0}), ScriptItem::pick(p, {10, 3})});
    CHECK(entityAs<PolylineData>(g.get(p))->v.size() == 4);
}

TEST_CASE("BREAK and BREAKATPOINT") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    f.host.run("BREAK", {ScriptItem::pick(l, {3, 0}), "7,0"});
    CHECK(f.count(EntityKind::Line) == 2);
    CHECK(near(entityAs<LineData>(f.get(l))->b, {3, 0}));
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 1);

    // First point option: select anywhere, then name both points.
    f.host.run("BREAK", {ScriptItem::pick(l, {9, 0}), "F", "2,0", "4,0"});
    CHECK(f.count(EntityKind::Line) == 2);
    f.host.undo();

    f.host.run("BAP", {ScriptItem::pick(l, {1, 0}), "6,0"});
    CHECK(f.count(EntityKind::Line) == 2);
    CHECK(near(entityAs<LineData>(f.get(l))->b, {6, 0}));

    // Same point twice breaks at the point.
    Fixture g;
    EntityId m = g.add(LineData{{0, 0}, {10, 0}});
    g.host.run("BREAK", {ScriptItem::pick(m, {4, 0}), "4,0"});
    CHECK(g.count(EntityKind::Line) == 2);
}

TEST_CASE("JOIN merges and erases the sources in one undo step") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {4, 0}});
    EntityId b = f.add(LineData{{3, 0}, {8, 0}});
    EntityId c = f.add(LineData{{8, 0}, {8, 5}});
    f.add(CircleData{{100, 100}, 2});
    f.host.run("JOIN", {ScriptItem::select({a, b, c}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Line) == 0);
    REQUIRE(f.count(EntityKind::Polyline) == 1);
    CHECK(f.count(EntityKind::Circle) == 1);
    CHECK(f.host.undoStack().undoLabel() == "JOIN");
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 3);

    Fixture g;
    EntityId x = g.add(LineData{{0, 0}, {1, 0}});
    EntityId y = g.add(LineData{{0, 5}, {1, 5}});
    g.host.run("JOIN", {ScriptItem::select({x, y}), ScriptItem::enter()});
    CHECK(g.host.logContains("Nothing could be joined"));
    CHECK(g.count(EntityKind::Line) == 2);
}

TEST_CASE("ERASE removes the selection") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {4, 0}});
    EntityId b = f.add(CircleData{{0, 0}, 2});
    f.add(LineData{{0, 3}, {4, 3}});
    f.host.run("E", {ScriptItem::select({a, b}), ScriptItem::enter()});
    CHECK(f.doc.entityCount() == 1);
    f.host.undo();
    CHECK(f.doc.entityCount() == 3);
}
