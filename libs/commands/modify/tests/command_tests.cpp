#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layout/layout.h"
#include "tbcad/modify/modify.h"
#include "tbcad/text/text.h"

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

TEST_CASE("OFFSET by distance, repeated, one undo step - Through - corner setting") {
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

TEST_CASE("OFFSET Multiple offsets the same object to several sides until Enter") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    auto r = f.host.run("OFFSET", {"2", ScriptItem::pick(l, {5, 0}), "M", "5,9", "5,-9",
                                   ScriptItem::enter(), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.count(EntityKind::Line) == 3);
}

TEST_CASE("OFFSET reports a collapse and keeps going") {
    Fixture f;
    EntityId c = f.add(CircleData{{0, 0}, 5});
    f.host.run("OFFSET", {"8", ScriptItem::pick(c, {5, 0}), "0,0", ScriptItem::esc()});
    CHECK(f.count(EntityKind::Circle) == 1);
    CHECK(f.host.logContains("collapses"));
}

TEST_CASE("TRIM ignores cutting edges on another level") {
    Fixture f;
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    EntityId edge = f.add(LineData{{3, -5}, {3, 5}});
    {
        auto tx = f.doc.begin("level");
        auto lv = tx.addLevel(Level{{}, "Upper", 3000});
        REQUIRE(lv.ok());
        REQUIRE(tx.modify(edge, [&](Entity& e) { e.common.level = lv.value(); }));
        tx.commit();
    }
    auto r =
        f.host.run("TRIM", {ScriptItem::enter(), ScriptItem::pick(h, {5, 0}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.count(EntityKind::Line) == 2);  // nothing was trimmed
    CHECK(near(entityAs<LineData>(f.get(h))->b, {10, 0}));
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

TEST_CASE("TRIM and EXTEND use the curves of a block reference as edges") {
    Fixture f;
    BlockId post;
    {
        auto tx = f.doc.begin("block");
        post = tx.addBlock(BlockDef{.name = "Post"}).value();
        Entity e;
        e.data = LineData{{0, -5}, {0, 5}};
        REQUIRE(tx.add(std::move(e), post).ok());
        tx.commit();
    }
    InsertData ins;
    ins.block = post;
    ins.position = {6, 0};
    f.add(ins);  // a vertical line at x = 6
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    // Everything is an edge: the block's line cuts the horizontal.
    f.host.run("TRIM", {ScriptItem::enter(), ScriptItem::pick(h, {8, 0}), ScriptItem::esc()});
    CHECK(near(entityAs<LineData>(f.get(h))->b, {6, 0}));
    // And bounds an extension.
    EntityId e = f.add(LineData{{0, 3}, {2, 3}});
    f.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(e, {1.5, 3}), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(e))->b, {6, 3}));
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

TEST_CASE("EXTEND an elliptical arc and a spline, and undo") {
    Fixture f;
    EllipseData el;
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    el.startParam = 0.3;
    el.endParam = 1.0;
    EntityId ea = f.add(el);
    SplineData sp;
    sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    sp.control = {{0, 30}, {10, 50}, {30, 10}, {50, 40}, {60, 30}};
    EntityId sid = f.add(Indirect<SplineData>(sp));
    f.add(LineData{{-20, 4}, {20, 4}});
    f.add(LineData{{80, -100}, {80, 100}});
    const std::size_t base = f.host.undoStack().undoCount();
    f.host.run("EXTEND",
               {ScriptItem::enter(), ScriptItem::pick(ea, {10 * std::cos(1.0), 5 * std::sin(1.0)}),
                ScriptItem::pick(sid, {59, 30.5}), ScriptItem::enter()});
    CHECK(std::abs(entityAs<EllipseData>(f.get(ea))->endParam - 2.2143) < 1e-3);
    std::vector<geom::CurveRef> cs;
    geom::curvesOf(f.get(sid), cs);
    REQUIRE(cs.size() == 1);
    CHECK(near(cs[0].endPoint(), {80, 10}, 1e-6));
    while (f.host.undoStack().undoCount() > base)
        f.host.undo();
    CHECK(std::abs(entityAs<EllipseData>(f.get(ea))->endParam - 1.0) < 1e-9);
}

TEST_CASE("FILLET a spline with a line, a polyline with a line, and LENGTHEN a spline") {
    Fixture f;
    SplineData sp;
    sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    sp.control = {{0, 0}, {10, 20}, {30, -20}, {50, 10}, {60, 0}};
    EntityId s = f.add(Indirect<SplineData>(sp));
    EntityId l = f.add(LineData{{20, -30}, {20, 30}});
    f.host.run("FILLET", {"R", "4", ScriptItem::pick(s, {8, 7}), ScriptItem::pick(l, {20, 25})});
    CHECK(f.count(EntityKind::Arc) == 1);
    CHECK(entityAs<LineData>(f.get(l))->a.y > -30 + 1e-6);
    f.host.undo();
    CHECK(f.count(EntityKind::Arc) == 0);

    EntityId pl = f.add(
        PolylineData{{{{0, 100}, 0, 0, 0}, {{20, 100}, 0, 0, 0}, {{20, 110}, 0, 0, 0}}, false});
    EntityId wall = f.add(LineData{{10, 95}, {10, 112}});
    f.host.run("FILLET",
               {"R", "2", ScriptItem::pick(pl, {5, 100}), ScriptItem::pick(wall, {10, 108})});
    CHECK(f.count(EntityKind::Arc) == 1);
    CHECK(entityAs<PolylineData>(f.get(pl))->v.back().p.x < 20);
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

TEST_CASE("JOIN joins splines end to end and elliptical arcs of one ellipse") {
    Fixture f;
    SplineData s1, s2;
    for (SplineData* s : {&s1, &s2}) {
        s->degree = 3;
        s->knots = {0, 0, 0, 0, 1, 1, 1, 1};
    }
    s1.control = {{0, 0}, {5, 8}, {10, 8}, {15, 0}};
    s2.control = {{15, 0}, {20, -8}, {25, -8}, {30, 0}};
    EntityId a = f.add(s1), b = f.add(s2);
    EllipseData e1, e2;
    for (EllipseData* e : {&e1, &e2}) {
        e->center = {100, 100};
        e->majorAxis = {20, 0};
        e->ratio = 0.5;
    }
    e1.startParam = 0;
    e1.endParam = 2;
    e2.startParam = 2;
    e2.endParam = 4;
    EntityId c = f.add(e1), d = f.add(e2);
    f.host.run("JOIN", {ScriptItem::select({a, b, c, d}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Spline) == 1);
    CHECK(f.count(EntityKind::Ellipse) == 1);
    f.host.undo();
    CHECK(f.count(EntityKind::Spline) == 2);
    CHECK(f.count(EntityKind::Ellipse) == 2);
}

TEST_CASE("JOIN fills the gap between collinear lines and between arcs of one circle") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {4, 0}});
    EntityId b = f.add(LineData{{6, 0}, {10, 0}});  // a gap of 2 between them
    f.host.run("JOIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
    REQUIRE(f.count(EntityKind::Line) == 1);
    for (EntityId id : f.doc.modelSpace()) {
        const auto* l = entityAs<LineData>(*f.doc.find(id));
        REQUIRE(l);
        CHECK(std::min(l->a.x, l->b.x) == doctest::Approx(0));
        CHECK(std::max(l->a.x, l->b.x) == doctest::Approx(10));
    }
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 2);

    // Lines that are not on one line stay apart, with or without a gap.
    Fixture g;
    EntityId x = g.add(LineData{{0, 0}, {4, 0}});
    EntityId y = g.add(LineData{{6, 0.5}, {10, 0.5}});
    g.host.run("JOIN", {ScriptItem::select({x, y}), ScriptItem::enter()});
    CHECK(g.count(EntityKind::Line) == 2);

    // Two arcs of one circle with a gap become one arc over the gap.
    Fixture h;
    EntityId p = h.add(ArcData{{0, 0}, 5, 0, kPi / 4});
    EntityId q = h.add(ArcData{{0, 0}, 5, kPi / 2, kPi});
    h.host.run("JOIN", {ScriptItem::select({p, q}), ScriptItem::enter()});
    REQUIRE(h.count(EntityKind::Arc) == 1);
    for (EntityId id : h.doc.modelSpace()) {
        const auto* arc = entityAs<ArcData>(*h.doc.find(id));
        REQUIRE(arc);
        CHECK(arc->startAngle == doctest::Approx(0));
        CHECK(arc->endAngle == doctest::Approx(kPi));
    }
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

TEST_CASE("OOPS brings back what the last ERASE removed") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {4, 0}});
    EntityId b = f.add(CircleData{{0, 0}, 2});
    f.host.run("OOPS", {});
    CHECK(f.host.logContains("Nothing has been erased"));
    f.host.run("E", {ScriptItem::select({a, b}), ScriptItem::enter()});
    CHECK(f.doc.entityCount() == 0);
    f.host.run("OOPS", {});
    CHECK(f.doc.entityCount() == 2);
    CHECK(f.count(EntityKind::Circle) == 1);
    CHECK(f.host.undo());
    CHECK(f.doc.entityCount() == 0);
    f.host.run("OOPS", {});  // nothing left to restore a second time
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("the extra modify commands are registered") {
    Fixture f;
    for (auto [alias, name] :
         std::vector<std::pair<const char*, const char*>>{{"LEN", "LENGTHEN"},
                                                          {"MA", "MATCHPROP"},
                                                          {"REVERSE", "REVERSE"},
                                                          {"DIV", "DIVIDE"},
                                                          {"ME", "MEASURE"},
                                                          {"AL", "ALIGN"},
                                                          {"OVERKILL", "OVERKILL"},
                                                          {"PE", "PEDIT"}}) {
        REQUIRE(f.reg.find(alias) != nullptr);
        CHECK(f.reg.find(alias) == f.reg.find(name));
    }
}

TEST_CASE("LENGTHEN by delta, percent and total, one undo step") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    auto r =
        f.host.run("LENGTHEN", {"Delta", "5", ScriptItem::pick(l, {9, 0}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(near(entityAs<LineData>(f.get(l))->b, {15, 0}));
    CHECK(f.host.undoStack().undoLabel() == "LENGTHEN");
    f.host.undo();
    CHECK(near(entityAs<LineData>(f.get(l))->b, {10, 0}));
    f.host.run("LENGTHEN", {"Total", "4", ScriptItem::pick(l, {1, 0}), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(l))->a, {6, 0}));
    f.host.run("LENGTHEN", {"Percent", "200", ScriptItem::pick(l, {9, 0}), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(l))->b, {14, 0}));
    f.host.run("LENGTHEN", {"Dynamic", ScriptItem::pick(l, {13, 0}), "20,5", ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(l))->b, {20, 0}));
}

TEST_CASE("REVERSE and ALIGN") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    f.host.run("REVERSE", {ScriptItem::select({l}), ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(l))->a, {10, 0}));
    f.host.undo();
    CHECK(near(entityAs<LineData>(f.get(l))->a, {0, 0}));

    f.host.run("ALIGN",
               {ScriptItem::select({l}), ScriptItem::enter(), "0,0", "5,5", "10,0", "5,15", "N"});
    const auto* ld = entityAs<LineData>(f.get(l));
    CHECK(near(ld->a, {5, 5}));
    CHECK(near(ld->b, {5, 15}));
}

TEST_CASE("DIVIDE and MEASURE place points") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {12, 0}});
    f.host.run("DIVIDE", {ScriptItem::pick(l, {6, 0}), "4"});
    CHECK(f.count(EntityKind::Point) == 3);
    CHECK(f.host.selection().size() == 3);  // the new points are selected so they can be seen
    CHECK(f.host.logContains("PTYPE"));
    f.host.undo();
    CHECK(f.count(EntityKind::Point) == 0);
    f.host.run("MEASURE", {ScriptItem::pick(l, {11, 0}), "5"});
    REQUIRE(f.count(EntityKind::Point) == 2);
    bool at7 = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PointData>(e))
            at7 = at7 || near(p->p, {7, 0});
    });
    CHECK(at7);  // measured from the end the pick was nearest to
}

TEST_CASE("MATCHPROP copies layer, colour and text height") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 1}, {1, 1}});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(a, [](Entity& e) {
            e.common.color = Color::byLayer();
            e.common.linetypeScale = 3;
        }));
        tx.commit();
    }
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {0.5, 0}), ScriptItem::pick(b, {0.5, 1}), ScriptItem::esc()});
    CHECK(f.get(b).common.linetypeScale == doctest::Approx(3));
}

TEST_CASE("MATCHPROP copies hatch pattern and transparency, Settings can switch a group off") {
    Fixture f;
    HatchData ha;
    ha.solid = false;
    ha.pattern = "ANSI31";
    ha.patternScale = 4;
    ha.patternAngle = 0.5;
    EntityId a = f.add(ha);
    EntityId b = f.add(HatchData{});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(a, [](Entity& e) { e.common.transparency = 40; }));
        tx.commit();
    }
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {0, 0}), ScriptItem::pick(b, {0, 0}), ScriptItem::esc()});
    const auto* hb = entityAs<HatchData>(f.get(b));
    CHECK(hb->pattern == "ANSI31");
    CHECK(hb->patternScale == doctest::Approx(4));
    CHECK(f.get(b).common.transparency == 40);

    EntityId c = f.add(HatchData{});
    f.host.run("MATCHPROP", {ScriptItem::pick(a, {0, 0}), "Settings", "Hatch", ScriptItem::enter(),
                             ScriptItem::pick(c, {0, 0}), ScriptItem::esc()});
    CHECK(entityAs<HatchData>(f.get(c))->pattern.empty());
    CHECK(f.get(c).common.transparency == 40);
}

TEST_CASE("OVERKILL deletes duplicates and merges overlaps") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 0}, {10, 0}});
    EntityId c = f.add(LineData{{8, 0}, {20, 0}});
    f.host.run("OVERKILL", {ScriptItem::select({a, b, c}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Line) == 1);
    CHECK(f.host.undoStack().undoLabel() == "OVERKILL");
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 3);
}

TEST_CASE("PEDIT converts a line, closes, widens and joins") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    EntityId m = f.add(LineData{{10, 0}, {10, 10}});
    f.host.run("PEDIT", {ScriptItem::pick(l, {5, 0}), "Yes", "Width", "2", "Join",
                         ScriptItem::select({m}), ScriptItem::enter(), "Exit"});
    REQUIRE(f.count(EntityKind::Polyline) == 1);
    CHECK(f.count(EntityKind::Line) == 0);
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PolylineData>(e)) {
            CHECK(p->v.size() == 3);
            CHECK_FALSE(p->closed);
        }
    });
}

TEST_CASE("PEDIT Taper, vertex Width and Straighten") {
    Fixture f;
    EntityId p = f.add(PolylineData{
        {{{0, 0}, 0.4, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}, {{20, 10}, 0, 0, 0}},
        false});
    f.host.run("PEDIT", {ScriptItem::pick(p, {5, 0}), "Taper", "0", "6", "Exit"});
    const auto* pl = entityAs<PolylineData>(f.get(p));
    REQUIRE(pl != nullptr);
    CHECK(pl->v[2].endWidth == doctest::Approx(6));
    f.host.run("PEDIT", {ScriptItem::pick(p, {5, 0}), "Vertices", "Width",
                         ScriptItem::click({10, 0}), "1", "2", "Exit", "Exit"});
    pl = entityAs<PolylineData>(f.get(p));
    CHECK(pl->v[1].startWidth == doctest::Approx(1));
    CHECK(pl->v[1].endWidth == doctest::Approx(2));
    f.host.run("PEDIT", {ScriptItem::pick(p, {5, 0}), "Vertices", "Straighten",
                         ScriptItem::click({0, 0}), ScriptItem::click({10, 10}), "Exit", "Exit"});
    pl = entityAs<PolylineData>(f.get(p));
    CHECK(pl->v.size() == 3);
    CHECK(pl->v[0].bulge == 0);
    CHECK(pl->v[1].p.y == doctest::Approx(10));
}

TEST_CASE("PEDIT Multiple closes, widens and joins every selected object") {
    Fixture f;
    EntityId a =
        f.add(PolylineData{{{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}}, false, 0});
    EntityId b = f.add(
        PolylineData{{{{50, 0}, 0, 0, 0}, {{60, 0}, 0, 0, 0}, {{60, 10}, 0, 0, 0}}, false, 0});
    f.host.run("PEDIT", {"Multiple", ScriptItem::select({a, b}), ScriptItem::enter(), "Close",
                         "Width", "3", "Exit"});
    int n = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PolylineData>(e)) {
            ++n;
            CHECK(p->closed);
            for (const auto& v : p->v) {
                CHECK(v.startWidth == doctest::Approx(3));
            }
        }
    });
    CHECK(n == 2);
    CHECK(f.host.undo());
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PolylineData>(e))
            CHECK_FALSE(p->closed);
    });
}

TEST_CASE("PEDIT Multiple joins with a fuzz distance and turns lines into polylines") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{10.3, 0.2}, {10, 10}});  // 0.36 from the end of a
    f.host.run("PEDIT", {"Multiple", ScriptItem::select({a, b}), ScriptItem::enter(), "Yes", "Join",
                         "Exit"});
    CHECK(f.count(EntityKind::Polyline) == 2);  // the default fuzz is too small to join them
    f.host.run("PEDIT", {"Multiple", ScriptItem::select({a, b}), ScriptItem::enter(), "Fuzz", "0.5",
                         "Join", "Exit"});
    CHECK(f.count(EntityKind::Polyline) == 1);
}

TEST_CASE("PEDIT Fit makes arcs through the vertices and Ltypegen OFF is kept") {
    Fixture f;
    EntityId a = f.add(PolylineData{
        {{{0, 0}, 0, 0, 0}, {{10, 8}, 0, 0, 0}, {{20, 0}, 0, 0, 0}, {{30, 8}, 0, 0, 0}}, false, 0});
    f.host.run("PEDIT", {ScriptItem::pick(a, {5, 4}), "Fit", "Ltypegen", "OFF", "Exit"});
    const auto* pl = entityAs<PolylineData>(f.get(a));
    REQUIRE(pl);
    CHECK(pl->v.size() == 7);  // each of the three segments became two arcs
    CHECK(pl->restartLinetype);
    CHECK(near(pl->v[2].p, {10, 8}));
    bool curved = false;
    for (const auto& v : pl->v)
        curved = curved || v.bulge != 0;
    CHECK(curved);
    CHECK(f.host.undo());
    CHECK(entityAs<PolylineData>(f.get(a))->v.size() == 4);
}

TEST_CASE("EXTRIM cuts several objects on one side of an edge") {
    Fixture f;
    EntityId edge = f.add(LineData{{5, -10}, {5, 10}});
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 3}, {10, 3}});
    f.add(LineData{{0, 20}, {4, 20}});
    auto r = f.host.run("EXTRIM", {ScriptItem::pick(edge, {5, 0}), "8,0",
                                   ScriptItem::select({a, b}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(near(entityAs<LineData>(f.get(a))->b, {5, 0}));
    CHECK(near(entityAs<LineData>(f.get(b))->b, {5, 3}));
    CHECK(f.host.undoStack().undoLabel() == "EXTRIM");
    f.host.undo();
    CHECK(near(entityAs<LineData>(f.get(a))->b, {10, 0}));
}

TEST_CASE("BLEND adds a spline between two line ends") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{30, 10}, {20, 10}});
    f.host.run("BLEND", {ScriptItem::pick(a, {9, 0}), ScriptItem::pick(b, {21, 10})});
    CHECK(f.count(EntityKind::Spline) == 1);
    f.host.undo();
    CHECK(f.count(EntityKind::Spline) == 0);
}

TEST_CASE("SCALETEXT and SETBYLAYER") {
    Fixture f;
    TextData t;
    t.height = 2;
    t.utf8 = "abc";
    EntityId id = f.add(Indirect<TextData>(t));
    f.host.run("SCALETEXT", {ScriptItem::select({id}), ScriptItem::enter(), "3"});
    CHECK(entityAs<TextData>(f.get(id))->height == doctest::Approx(6));
    f.host.run("SCALETEXT", {ScriptItem::select({id}), ScriptItem::enter(), "New height", "5"});
    CHECK(entityAs<TextData>(f.get(id))->height == doctest::Approx(5));

    EntityId l = f.add(LineData{{0, 0}, {1, 0}});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(l, [](Entity& e) { e.common.lineweight = LineWeight::mm(0.5); }));
        tx.commit();
    }
    f.host.run("SETBYLAYER", {ScriptItem::select({l}), ScriptItem::enter()});
    CHECK(f.get(l).common.lineweight == LineWeight::byLayer());
}

TEST_CASE("ALIGN and MATCHPROP also work on text") {
    Fixture f;
    TextData t;
    t.position = {1, 1};
    t.utf8 = "x";
    EntityId id = f.add(Indirect<TextData>(t));
    f.host.run("ALIGN", {ScriptItem::select({id}), ScriptItem::enter(), "0,0", "10,10",
                         ScriptItem::enter()});
    CHECK(near(entityAs<TextData>(f.get(id))->position, {11, 11}));
}

TEST_CASE("JUSTIFYTEXT keeps the text where it is") {
    Fixture f;
    TextData t;
    t.position = {10, 20};
    t.height = 2;
    t.utf8 = "hello";
    EntityId id = f.add(Indirect<TextData>(t));
    auto before = text::layoutEntity(f.doc, f.get(id)).frameCorners()[0];
    f.host.run("JUSTIFYTEXT", {ScriptItem::select({id}), ScriptItem::enter(), "MC"});
    const auto* now = entityAs<TextData>(f.get(id));
    CHECK(now->h == HAlign::Center);
    CHECK(now->v == VAlign::Middle);
    auto after = text::layoutEntity(f.doc, f.get(id)).frameCorners()[0];
    CHECK(near(before, after, 1e-6));
}

TEST_CASE("PEDIT edits vertices") {
    Fixture f;
    EntityId p =
        f.add(PolylineData{{{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}}, false, 0});
    f.host.run("PEDIT", {ScriptItem::pick(p, {5, 0}), "Vertices", "Insert", "4,0", "Move", "10,10",
                         "12,12", "Exit", "Exit"});
    const auto* pl = entityAs<PolylineData>(f.get(p));
    REQUIRE(pl->v.size() == 4);
    CHECK(near(pl->v[1].p, {4, 0}));
    CHECK(near(pl->v[3].p, {12, 12}));
}

TEST_CASE("SPLINEDIT adds and removes fit points") {
    Fixture f;
    SplineData s;
    s.fit = {{0, 0}, {5, 4}, {10, 0}, {15, 4}};
    geom::InterpolateOptions io;
    auto fitted = geom::interpolateSpline(s.fit, io);
    REQUIRE(fitted.ok());
    EntityId id = f.add(Indirect<SplineData>(fitted.value()));
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {5, 4}), "Delete", "5,4", "Exit"});
    CHECK(entityAs<SplineData>(f.get(id))->fit.size() == 3);
    f.host.undo();
    CHECK(entityAs<SplineData>(f.get(id))->fit.size() == 4);
}

TEST_CASE("typing ALL at the selection prompt and pressing Enter finishes the selection") {
    Fixture f;
    f.add(LineData{{0, 0}, {1, 0}});
    f.add(LineData{{0, 2}, {1, 2}});
    auto r = f.host.run("GROUP", {"ALL", ScriptItem::enter(), "pair"});
    CHECK(r.finished);
    CHECK(f.host.logContains("2 found"));
    CHECK(readGroups(f.doc).size() == 1);
}

TEST_CASE("GROUP makes a pick select the whole group - UNGROUP dissolves it") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 2}, {1, 2}});
    EntityId c = f.add(LineData{{0, 4}, {1, 4}});
    f.host.run("GROUP", {ScriptItem::select({a, b}), ScriptItem::enter(), "pair"});
    REQUIRE(readGroups(f.doc).size() == 1);
    CHECK(f.host.undoStack().undoLabel() == "GROUP");

    // Erasing one member by a pick removes both.
    f.host.run("ERASE", {ScriptItem::pick(a, {0.5, 0}), ScriptItem::enter()});
    CHECK(f.doc.find(a) == nullptr);
    CHECK(f.doc.find(b) == nullptr);
    CHECK(f.doc.find(c) != nullptr);
    f.host.undo();
    CHECK(f.doc.find(b) != nullptr);

    f.host.run("UNGROUP", {ScriptItem::pick(a, {0.5, 0})});
    CHECK(readGroups(f.doc).empty());
    f.host.run("ERASE", {ScriptItem::pick(a, {0.5, 0}), ScriptItem::enter()});
    CHECK(f.doc.find(a) == nullptr);
    CHECK(f.doc.find(b) != nullptr);
}

TEST_CASE("GROUPEDIT adds and removes members, renames and toggles selectable") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 2}, {1, 2}});
    EntityId c = f.add(LineData{{0, 4}, {1, 4}});
    f.host.run("GROUP", {ScriptItem::select({a}), ScriptItem::enter(), "one"});
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Add", ScriptItem::select({b, c}),
                             ScriptItem::enter()});
    REQUIRE(readGroups(f.doc).size() == 1);
    CHECK(readGroups(f.doc)[0].members.size() == 3);
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Remove", ScriptItem::pick(c, {0.5, 4}),
                             ScriptItem::enter()});
    CHECK(readGroups(f.doc)[0].members.size() == 2);
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Rename", "pair"});
    CHECK(readGroups(f.doc)[0].name == "pair");
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Selectable", "No"});
    CHECK_FALSE(readGroups(f.doc)[0].selectable);
    f.host.undo();
    CHECK(readGroups(f.doc)[0].selectable);
}

TEST_CASE("GROUP names are trimmed and unique ignoring case, a new group takes members over") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 2}, {1, 2}});
    EntityId c = f.add(LineData{{0, 4}, {1, 4}});
    f.host.run("GROUP", {ScriptItem::select({a, b}), ScriptItem::enter(), "  Pair  "});
    REQUIRE(readGroups(f.doc).size() == 1);
    CHECK(readGroups(f.doc)[0].name == "Pair");
    f.host.run("GROUP", {ScriptItem::select({c}), ScriptItem::enter(), "PAIR"});
    CHECK(f.host.logContains("already a group"));
    CHECK(readGroups(f.doc).size() == 1);
    // Picking b selects the whole group, so a new group takes all three over.
    f.host.run("GROUP", {ScriptItem::select({b, c}), ScriptItem::enter(), "other"});
    auto gs = readGroups(f.doc);
    REQUIRE(gs.size() == 1);
    CHECK(gs[0].name == "other");
    CHECK(gs[0].members.size() == 3);
    f.host.run("UNGROUP", {ScriptItem::pick(c, {0.5, 4})});
    CHECK(readGroups(f.doc).empty());
    f.host.run("GROUP", {ScriptItem::select({a}), ScriptItem::enter(), "Pair"});
    // Renaming to an existing name in other case is refused.
    f.host.run("GROUP", {ScriptItem::select({b}), ScriptItem::enter(), "second"});
    f.host.run("GROUPEDIT", {ScriptItem::pick(b, {0.5, 2}), "Rename", "pAiR"});
    CHECK(f.host.logContains("already a group"));
    CHECK(readGroups(f.doc)[1].name == "second");
}

TEST_CASE("A non-selectable group does not expand and removing the last member drops the group") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 2}, {1, 2}});
    f.host.run("GROUP", {ScriptItem::select({a, b}), ScriptItem::enter(), "pair"});
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Selectable", "No"});
    f.host.run("ERASE", {ScriptItem::pick(a, {0.5, 0}), ScriptItem::enter()});
    CHECK(f.doc.find(a) == nullptr);
    CHECK(f.doc.find(b) != nullptr);
    f.host.undo();
    f.host.run("GROUPEDIT", {ScriptItem::pick(a, {0.5, 0}), "Remove", ScriptItem::pick(a, {0.5, 0}),
                             ScriptItem::pick(b, {0.5, 2}), ScriptItem::enter()});
    CHECK(readGroups(f.doc).empty());
    f.host.undo();
    CHECK(readGroups(f.doc).size() == 1);
}

TEST_CASE("DRAWORDER front, back and above/below an object") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId b = f.add(LineData{{0, 1}, {1, 1}});
    EntityId c = f.add(LineData{{0, 2}, {1, 2}});
    auto order = [&] { return f.doc.modelSpace(); };
    CHECK(order() == std::vector<EntityId>{a, b, c});
    f.host.run("DRAWORDER", {ScriptItem::select({a}), ScriptItem::enter(), "Front"});
    CHECK(order() == std::vector<EntityId>{b, c, a});
    f.host.run("DRAWORDER", {ScriptItem::select({a}), ScriptItem::enter(), "Back"});
    CHECK(order() == std::vector<EntityId>{a, b, c});
    f.host.run("DRAWORDER",
               {ScriptItem::select({a}), ScriptItem::enter(), "Above", ScriptItem::pick(b)});
    CHECK(order() == std::vector<EntityId>{b, a, c});
    f.host.run("DRAWORDER",
               {ScriptItem::select({c}), ScriptItem::enter(), "Below", ScriptItem::pick(b)});
    CHECK(order() == std::vector<EntityId>{c, b, a});
    f.host.undo();
    CHECK(order() == std::vector<EntityId>{b, a, c});
}

TEST_CASE("TEXTTOFRONT and HATCHTOBACK reorder by kind and keep the order inside a kind") {
    Fixture f;
    TextData td;
    td.utf8 = "t";
    HatchData hd;
    EntityId t1 = f.add(Indirect<TextData>(td));
    EntityId h = f.add(Indirect<HatchData>(hd));
    EntityId a = f.add(LineData{{0, 0}, {1, 0}});
    EntityId t2 = f.add(Indirect<TextData>(td));
    EntityId b = f.add(LineData{{0, 1}, {1, 1}});
    f.host.run("TEXTTOFRONT", {"Text"});
    CHECK(f.doc.modelSpace() == std::vector<EntityId>{h, a, b, t1, t2});
    f.host.run("HATCHTOBACK", {});
    CHECK(f.doc.modelSpace() == std::vector<EntityId>{h, a, b, t1, t2});
    f.host.run("DRAWORDER", {ScriptItem::select({h}), ScriptItem::enter(), "Front"});
    CHECK(f.doc.modelSpace().back() == h);
    f.host.run("HATCHTOBACK", {});
    CHECK(f.doc.modelSpace().front() == h);
    f.host.undo();
    CHECK(f.doc.modelSpace().back() == h);
}

TEST_CASE("DRAWORDER, TEXTTOFRONT and HATCHTOBACK also work on the objects of a layout sheet") {
    Fixture f;
    {
        auto tx = f.doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Sheet", "A4", true).ok());
        tx.commit();
    }
    const BlockId sheet = layout::layouts(f.doc).front().content;
    TextData td;
    td.utf8 = "title";
    EntityId text, hatch, line;
    {
        auto tx = f.doc.begin("objects");
        Entity t;
        t.data = Indirect<TextData>(td);
        Entity h;
        h.data = Indirect<HatchData>(HatchData{});
        Entity l;
        l.data = LineData{{0, 0}, {10, 0}};
        text = tx.add(std::move(t), sheet).value();
        hatch = tx.add(std::move(h), sheet).value();
        line = tx.add(std::move(l), sheet).value();
        tx.commit();
    }
    auto order = [&] { return f.doc.block(sheet)->entities; };
    CHECK(order() == std::vector<EntityId>{text, hatch, line});
    f.host.run("HATCHTOBACK", {});
    CHECK(order().front() == hatch);
    f.host.run("TEXTTOFRONT", {"Text"});
    CHECK(order().back() == text);
    CHECK(order() == std::vector<EntityId>{hatch, line, text});
    f.host.run("DRAWORDER", {ScriptItem::select({line}), ScriptItem::enter(), "Back"});
    CHECK(order().front() == line);
    f.host.run("DRAWORDER",
               {ScriptItem::select({line}), ScriptItem::enter(), "Above", ScriptItem::pick(hatch)});
    CHECK(order() == std::vector<EntityId>{hatch, line, text});
}

TEST_CASE("FIND replaces text, only counts without a replacement, and undoes in one step") {
    Fixture f;
    TextData td;
    td.utf8 = "Room 1 / room 2";
    EntityId t = f.add(Indirect<TextData>(td));
    f.host.run("FIND", {"room", "", "No", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / room 2");
    CHECK(f.host.selection() == std::vector<EntityId>{t});  // the matches are selected
    f.host.run("FIND", {"room", "Hall", "No", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Hall 1 / Hall 2");
    f.host.undo();
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / room 2");
    f.host.run("FIND", {"room", "Hall", "Yes", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / Hall 2");
}

TEST_CASE("FIND with whole words only leaves matches inside longer words alone") {
    Fixture f;
    TextData td;
    td.utf8 = "room 1, bedroom, room_2, room.";
    EntityId t = f.add(Indirect<TextData>(td));
    MTextData mt;
    mt.utf8 = "roomy and {\\Lroom}";
    EntityId m = f.add(Indirect<MTextData>(mt));
    f.host.run("FIND", {"room", "Hall", "No", "Yes"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Hall 1, bedroom, room_2, Hall.");
    CHECK(entityAs<MTextData>(f.get(m))->utf8 == "roomy and {\\LHall}");
    f.host.run("FIND", {"room", "Hall", "No", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Hall 1, bedHall, Hall_2, Hall.");
}

TEST_CASE("FIND also changes text inside block definitions") {
    Fixture f;
    BlockId room;
    EntityId inner;
    {
        auto tx = f.doc.begin("block");
        room = tx.addBlock(BlockDef{.name = "Room"}).value();
        Entity e;
        TextData td;
        td.utf8 = "room label";
        e.data = Indirect<TextData>(td);
        inner = tx.add(std::move(e), room).value();
        tx.commit();
    }
    f.host.run("FIND", {"room", "Hall", "No", "No"});
    CHECK(entityAs<TextData>(f.get(inner))->utf8 == "Hall label");
    f.host.undo();
    CHECK(entityAs<TextData>(f.get(inner))->utf8 == "room label");
}

TEST_CASE("COPYTOLAYER copies onto the layer of the picked object") {
    Fixture f;
    LayerId target;
    {
        Transaction tx = f.doc.begin("setup");
        Layer l;
        l.name = "Target";
        target = tx.addLayer(l).value();
        tx.commit();
    }
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    Entity e;
    e.data = LineData{{0, 5}, {10, 5}};
    e.common.layer = target;
    EntityId b;
    {
        Transaction tx = f.doc.begin("setup");
        b = tx.add(e).value();
        tx.commit();
    }
    f.host.run("COPYTOLAYER", {ScriptItem::select({a}), ScriptItem::enter(), ScriptItem::pick(b)});
    CHECK(f.count(EntityKind::Line) == 3);
    CHECK(f.get(a).common.layer != target);
}

TEST_CASE("OFFSET Layer option puts the copies on the current layer") {
    Fixture f;
    LayerId walls, other;
    {
        auto tx = f.doc.begin("layers");
        Layer a;
        a.name = "Walls";
        walls = tx.addLayer(a).value();
        Layer b;
        b.name = "Other";
        other = tx.addLayer(b).value();
        tx.commit();
    }
    Entity src;
    src.data = LineData{{0, 0}, {10, 0}};
    src.common.layer = walls;
    {
        auto tx = f.doc.begin("src");
        tx.add(std::move(src)).value();
        tx.commit();
    }
    {
        auto tx = f.doc.begin("current");
        (void)tx.modifyHeader([&](Header& h) { h.currentLayer = other; });
        tx.commit();
    }
    EntityId srcId = f.doc.modelSpace().front();
    f.host.run("OFFSET", {"Layer", "Current", "2", ScriptItem::pick(srcId, {5, 0}), "5,9",
                          ScriptItem::enter()});
    REQUIRE(f.count(EntityKind::Line) == 2);
    int onOther = 0;
    f.doc.forEachEntity([&](const Entity& e) { onOther += e.common.layer == other ? 1 : 0; });
    CHECK(onOther == 1);
}

TEST_CASE("OFFSET Erase removes the source object") {
    Fixture f;
    EntityId l = f.add(LineData{{0, 0}, {10, 0}});
    f.host.run("OFFSET",
               {"Erase", "Yes", "2", ScriptItem::pick(l, {5, 0}), "5,9", ScriptItem::enter()});
    REQUIRE(f.count(EntityKind::Line) == 1);
    CHECK_FALSE(f.doc.contains(l));
    // The setting stays for the next run, and No turns it off again.
    EntityId m = f.add(LineData{{0, 0}, {10, 0}});
    f.host.run("OFFSET",
               {"Erase", "No", "2", ScriptItem::pick(m, {5, 0}), "5,9", ScriptItem::enter()});
    CHECK(f.doc.contains(m));
    CHECK(f.count(EntityKind::Line) == 3);
}

TEST_CASE("TRIM Remove deletes the objects picked") {
    Fixture f;
    EntityId edge = f.add(LineData{{0, -5}, {0, 5}});
    EntityId stray = f.add(LineData{{20, 0}, {30, 0}});
    f.host.run("TRIM", {ScriptItem::enter(), "Remove", ScriptItem::select({stray}),
                        ScriptItem::enter(), ScriptItem::enter()});
    CHECK(f.doc.contains(edge));
    CHECK_FALSE(f.doc.contains(stray));
}

TEST_CASE("DIVIDE and MEASURE with the Block option insert a block along the object") {
    Fixture f;
    BlockId chair;
    {
        auto tx = f.doc.begin("block");
        chair = tx.addBlock(BlockDef{.name = "Chair"}).value();
        tx.commit();
    }
    EntityId l = f.add(LineData{{0, 0}, {0, 12}});  // runs along +Y
    f.host.run("DIVIDE", {ScriptItem::pick(l, {0, 6}), "Block", "Chair", "Yes", "4"});
    std::vector<const InsertData*> inserts;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            inserts.push_back(i);
    });
    REQUIRE(inserts.size() == 3);
    for (const InsertData* i : inserts) {
        CHECK(i->block == chair);
        CHECK(i->rotation == doctest::Approx(kPi / 2));  // turned along the line
        CHECK(i->position.x == doctest::Approx(0));
    }
    CHECK(f.count(EntityKind::Point) == 0);
    f.host.undo();
    CHECK(f.count(EntityKind::Insert) == 0);

    f.host.run("MEASURE", {ScriptItem::pick(l, {0, 1}), "Block", "Chair", "No", "5"});
    inserts.clear();
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            inserts.push_back(i);
    });
    REQUIRE(inserts.size() == 2);
    for (const InsertData* i : inserts)
        CHECK(i->rotation == doctest::Approx(0));
}

TEST_CASE("DIVIDE and MEASURE with blocks follow ellipses, splines, circles and polyline arcs") {
    EllipseData el;
    el.center = {0, 0};
    el.majorAxis = {20, 0};
    el.ratio = 0.5;
    el.startParam = 0.2;
    el.endParam = 2.5;
    SplineData sp;
    sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    sp.control = {{0, 50}, {10, 70}, {30, 30}, {50, 60}, {60, 50}};
    const std::vector<EntityData> shapes{el, Indirect<SplineData>(sp), CircleData{{100, 100}, 15},
                                         PolylineData{{{{0, 200}, 0.5, 0, 0},
                                                       {{20, 200}, 0, 0, 0},
                                                       {{20, 230}, -0.4, 0, 0},
                                                       {{45, 230}, 0, 0, 0}},
                                                      false}};
    for (const EntityData& shape : shapes) {
        Fixture f;
        {
            auto tx = f.doc.begin("block");
            (void)tx.addBlock(BlockDef{.name = "Chair"}).value();
            tx.commit();
        }
        EntityId id = f.add(shape);
        std::vector<geom::CurveRef> cs;
        geom::curvesOf(f.get(id), cs);
        double total = 0;
        for (const auto& c : cs)
            total += c.length();
        const Vec2 pickAt = cs.front().pointAt(cs.front().t0());
        f.host.run("DIVIDE", {ScriptItem::pick(id, pickAt), "Block", "Chair", "Yes", "5"});
        std::vector<const Entity*> placed;
        f.doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Insert)
                placed.push_back(&e);
        });
        const bool closed = kind(f.get(id)) == EntityKind::Circle;
        REQUIRE(placed.size() == (closed ? 5u : 4u));
        for (const Entity* e : placed) {
            const auto* ins = entityAs<InsertData>(*e);
            // On the object, turned along it.
            double best = 1e300;
            const geom::CurveRef* on = nullptr;
            for (const auto& c : cs)
                if (c.distanceTo(ins->position) < best) {
                    best = c.distanceTo(ins->position);
                    on = &c;
                }
            CHECK(best < 1e-6);
            const Vec2 t = on->tangentAt(on->paramAt(ins->position));
            CHECK(std::abs(std::sin(ins->rotation - std::atan2(t.y, t.x))) < 1e-6);
            CHECK(std::cos(ins->rotation - std::atan2(t.y, t.x)) > 0.9999);
        }
        (void)total;
        f.host.undo();
        CHECK(f.count(EntityKind::Insert) == 0);
        // MEASURE from the other end places the first block that far from that end.
        const Vec2 farEnd =
            cs.back().pointAt(cs.back().t0() + (cs.back().isLine() ? 1.0 : cs.back().sweep()));
        if (!closed) {
            f.host.run("MEASURE", {ScriptItem::pick(id, farEnd), "Block", "Chair", "No", "4"});
            Vec2 first{};
            double nearest = 1e300;
            f.doc.forEachEntity([&](const Entity& e) {
                if (const auto* ins = entityAs<InsertData>(e);
                    ins && distance(ins->position, farEnd) < nearest) {
                    nearest = distance(ins->position, farEnd);
                    first = ins->position;
                }
            });
            (void)first;
            CHECK(nearest <= 4.0 + 1e-6);
            f.host.undo();
        }
    }
}

TEST_CASE("TRIM and EXTEND Mode: Quick uses every object as an edge, Standard asks again") {
    Fixture f;
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    EntityId far = f.add(LineData{{8, -5}, {8, 5}});
    EntityId near3 = f.add(LineData{{3, -5}, {3, 5}});
    // Standard: only `far` cuts, so the segment from the pick runs to x = 8.
    f.host.run("TRIM", {ScriptItem::select({far}), ScriptItem::enter(), ScriptItem::pick(h, {5, 0}),
                        ScriptItem::esc()});
    CHECK((near(entityAs<LineData>(f.get(h))->b, {8, 0}) ||
           near(entityAs<LineData>(f.get(h))->a, {8, 0})));
    f.host.undo();
    CHECK(near(entityAs<LineData>(f.get(h))->b, {10, 0}));
    // Quick: no edge prompt; the nearest cuts on both sides (x = 3 and x = 8) apply.
    f.host.run("TRIM", {ScriptItem::enter(), "Mode", "Quick", ScriptItem::pick(h, {5, 0}),
                        ScriptItem::esc()});
    CHECK(f.count(EntityKind::Line) == 4);
    f.host.undo();
    // Back to Standard: the edge prompt returns.
    f.host.run("TRIM", {"Mode", "Standard", ScriptItem::select({near3}), ScriptItem::enter(),
                        ScriptItem::pick(h, {5, 0}), ScriptItem::esc()});
    CHECK((near(entityAs<LineData>(f.get(h))->a, {3, 0}) ||
           near(entityAs<LineData>(f.get(h))->b, {3, 0})));
}

TEST_CASE("TRIM offers Mode at the first prompt for the cutting edges") {
    Fixture f;
    EntityId h = f.add(LineData{{0, 0}, {10, 0}});
    f.add(LineData{{8, -5}, {8, 5}});
    f.add(LineData{{3, -5}, {3, 5}});
    f.host.run("TRIM", {"Mode", "Quick", ScriptItem::pick(h, {5, 0}), ScriptItem::esc()});
    CHECK(f.count(EntityKind::Line) == 4);
}

TEST_CASE("FILLET Multiple keeps asking for pairs until Enter, one undo step") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 0}, {0, 10}});
    EntityId c = f.add(LineData{{30, 0}, {40, 0}});
    EntityId d = f.add(LineData{{30, 0}, {30, 10}});
    auto r = f.host.run("FILLET", {"R", "2", "M", ScriptItem::pick(a, {8, 0}),
                                   ScriptItem::pick(b, {0, 8}), ScriptItem::pick(c, {38, 0}),
                                   ScriptItem::pick(d, {30, 8}), ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.count(EntityKind::Arc) == 2);
    f.host.undo();
    CHECK(f.count(EntityKind::Arc) == 0);
}

TEST_CASE("CHAMFER Multiple adds one chamfer per pair") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 0}, {0, 10}});
    EntityId c = f.add(LineData{{30, 0}, {40, 0}});
    EntityId d = f.add(LineData{{30, 0}, {30, 10}});
    f.host.run("CHAMFER",
               {"D", "2", "2", "Mu", ScriptItem::pick(a, {8, 0}), ScriptItem::pick(b, {0, 8}),
                ScriptItem::pick(c, {38, 0}), ScriptItem::pick(d, {30, 8}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Line) == 6);

    Fixture g;
    EntityId v = g.add(LineData{{0, 0}, {10, 0}});
    g.add(LineData{{3, -5}, {3, 5}});
    g.add(LineData{{7, -5}, {7, 5}});
    g.host.run("TRIM", {ScriptItem::enter(), "C", "4,-1", "6,1", ScriptItem::esc()});
    CHECK(g.count(EntityKind::Line) == 4);
    CHECK(near(entityAs<LineData>(g.get(v))->b, {3, 0}));
}

TEST_CASE(
    "TRIM and EXTEND use nested, rotated, scaled and xref inserts as edges at survey coordinates") {
    Fixture f;
    const Vec2 far{600000000.0, 1500000000.0};
    BlockId post, outer, xr;
    {
        auto tx = f.doc.begin("blocks");
        post = tx.addBlock(BlockDef{.name = "Post"}).value();
        Entity e;
        e.data = LineData{{0, -5}, {0, 5}};  // vertical, 10 long
        REQUIRE(tx.add(std::move(e), post).ok());
        outer = tx.addBlock(BlockDef{.name = "Outer"}).value();
        Entity n;
        n.data = Indirect<InsertData>(InsertData{post, {3, 0}, {1, 1}, 0});
        REQUIRE(tx.add(std::move(n), outer).ok());
        BlockDef x;
        x.name = "Ref";
        x.xrefPath = "ref.tbcad";
        xr = tx.addBlock(std::move(x)).value();
        Entity xe;
        xe.data = LineData{{0, -5}, {0, 5}};
        REQUIRE(tx.add(std::move(xe), xr).ok());
        tx.commit();
    }
    // Outer rotated 90 degrees and scaled 2: the nested post becomes a horizontal line of 20
    // at y = 6 (x = far.x + [-10, 10]).
    InsertData oi;
    oi.block = outer;
    oi.position = far;
    oi.scale = {2, 2};
    oi.rotation = kPi / 2;
    f.add(oi);
    EntityId v = f.add(LineData{{far.x, far.y - 20}, {far.x, far.y + 20}});
    f.host.run("TRIM",
               {ScriptItem::enter(), ScriptItem::pick(v, {far.x, far.y + 15}), ScriptItem::esc()});
    CHECK(near(entityAs<LineData>(f.get(v))->b, {far.x, far.y + 6}, 1e-3));

    // An xref at far coordinates bounds an extension too.
    InsertData xi;
    xi.block = xr;
    xi.position = {far.x + 30, far.y};
    f.add(xi);
    EntityId h = f.add(LineData{{far.x + 10, far.y}, {far.x + 12, far.y}});
    f.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(h, {far.x + 11.5, far.y}),
                          ScriptItem::enter()});
    CHECK(near(entityAs<LineData>(f.get(h))->b, {far.x + 30, far.y}, 1e-3));
}

TEST_CASE("DIVIDE and MEASURE follow a clockwise bulged polyline at survey coordinates") {
    const double ox = 500123.456, oy = 4500987.654;
    Fixture f;
    PolylineData
        pl;  // clockwise half circle of radius 10 from (ox-10,oy) over the top to (ox+10,oy)
    pl.v = {PolyVertex{{ox - 10, oy}, -1}, PolyVertex{{ox + 10, oy}, 0}};
    EntityId id = f.add(std::move(pl));
    f.host.run("DIVIDE", {ScriptItem::pick(id, {ox, oy + 10}), "4"});
    std::vector<Vec2> pts;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PointData>(e))
            pts.push_back(p->p);
    });
    REQUIRE(pts.size() == 3);
    // Quarter points in travel order: 135, 90 and 45 degrees.
    auto has = [&](Vec2 want) {
        for (Vec2 p : pts)
            if (distance(p, want) < 1e-6)
                return true;
        return false;
    };
    CHECK(has({ox - 10 * std::sqrt(0.5), oy + 10 * std::sqrt(0.5)}));
    CHECK(has({ox, oy + 10}));
    CHECK(has({ox + 10 * std::sqrt(0.5), oy + 10 * std::sqrt(0.5)}));
    f.host.undo();
    // MEASURE from the start (pick near it): points every 5 along the arc, first at 5 / 10 rad.
    f.host.run("MEASURE", {ScriptItem::pick(id, {ox - 10, oy}), "5"});
    pts.clear();
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PointData>(e))
            pts.push_back(p->p);
    });
    REQUIRE(pts.size() == 6);  // arc length 31.4159: 5, 10, ..., 30
    const double a = 5.0 / 10.0;
    CHECK(has({ox - 10 * std::cos(a), oy + 10 * std::sin(a)}));
}

TEST_CASE("MEASURE with the Block option rotates blocks along an arc at survey coordinates") {
    const double ox = 2500000.5, oy = 7000000.25;
    Fixture f;
    {
        auto tx = f.doc.begin("block");
        tx.addBlock(BlockDef{.name = "Post"}).value();
        tx.commit();
    }
    ArcData arc;
    arc.center = {ox, oy};
    arc.radius = 20;
    arc.startAngle = 0;
    arc.endAngle = kPi;
    EntityId id = f.add(arc);
    f.host.run("DIVIDE", {ScriptItem::pick(id, {ox, oy + 20}), "Block", "Post", "Yes", "2"});
    int n = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e)) {
            ++n;
            CHECK(distance(i->position, {ox, oy + 20}) < 1e-6);
            CHECK(i->rotation == doctest::Approx(kPi).epsilon(1e-9));  // tangent at the top
        }
    });
    CHECK(n == 1);
}

TEST_CASE("SPLINEDIT edits the control vertices of a CV spline and converts it to a polyline") {
    Fixture f;
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = {{0, 0}, {10, 20}, {30, -20}, {40, 0}};
    EntityId id = f.add(Indirect<SplineData>(sp));
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {10, 20}), "Add", "20,0", "Exit"});
    CHECK(entityAs<SplineData>(f.get(id))->control.size() == 5);
    f.host.undo();
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {10, 20}), "Move", "10,20", "10,30", "Exit"});
    CHECK(near(entityAs<SplineData>(f.get(id))->control[1], {10, 30}));
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {10, 30}), "Weight", "10,30", "4", "Exit"});
    REQUIRE(entityAs<SplineData>(f.get(id))->weights.size() == 4);
    CHECK(entityAs<SplineData>(f.get(id))->weights[1] == doctest::Approx(4));
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {10, 30}), "Polyline", "0.1"});
    CHECK(f.count(EntityKind::Polyline) == 1);
    CHECK(f.count(EntityKind::Spline) == 0);
}

TEST_CASE("SPLINEDIT and PEDIT Tolerance refit a spline through fewer fit points") {
    Fixture f;
    SplineData sp;
    sp.degree = 3;
    for (int i = 0; i <= 40; ++i)
        sp.fit.push_back({i * 2.5, 10 * std::sin(i * 2.5 / 20)});
    auto made = modify::polylineToSpline(Entity{{}, [&] {
                                                    PolylineData p;
                                                    for (const Vec2& q : sp.fit)
                                                        p.v.push_back({q, 0, 0, 0});
                                                    return p;
                                                }()});
    REQUIRE(made.ok());
    EntityId id = f.add(Indirect<SplineData>(*entityAs<SplineData>(made.value())));
    std::size_t before = entityAs<SplineData>(f.get(id))->fit.size();
    f.host.run("SPLINEDIT", {ScriptItem::pick(id, {10, 5}), "Tolerance", "0.05", "Exit"});
    std::size_t after = entityAs<SplineData>(f.get(id))->fit.size();
    CHECK(after < before);
    CHECK(f.host.logContains("Fitted again through"));
    f.host.undo();
    CHECK(entityAs<SplineData>(f.get(id))->fit.size() == before);
    f.host.run("PEDIT", {ScriptItem::pick(id, {10, 5}), "Tolerance", "0.05", "Exit"});
    CHECK(entityAs<SplineData>(f.get(id))->fit.size() == after);
}

TEST_CASE("BREAK and BREAKATPOINT on a construction line and a ray") {
    Fixture f;
    RayData xl;
    xl.origin = {0, 0};
    xl.dir = {1, 0};
    xl.infinite = true;
    EntityId x = f.add(xl);
    f.host.run("BREAK", {ScriptItem::pick(x, {5, 0}), "10,0"});
    CHECK(f.count(EntityKind::Ray) == 2);
    f.host.undo();
    CHECK(f.count(EntityKind::Ray) == 1);
    f.host.run("BREAKATPOINT", {ScriptItem::pick(x, {5, 0}), "5,0"});
    CHECK(f.count(EntityKind::Ray) == 2);

    Fixture g;
    RayData ray;
    ray.origin = {0, 0};
    ray.dir = {1, 0};
    EntityId r = g.add(ray);
    g.host.run("BREAK", {ScriptItem::pick(r, {4, 0}), "8,0"});
    CHECK(g.count(EntityKind::Line) == 1);
    CHECK(g.count(EntityKind::Ray) == 1);
    g.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e))
            CHECK(near(l->b, {4, 0}));
        if (const auto* rr = entityAs<RayData>(e))
            CHECK(near(rr->origin, {8, 0}));
    });
    // A break that starts before the ray's start cuts the start off; one wholly before removes
    // nothing.
    Fixture h;
    EntityId r2 = h.add(ray);
    h.host.run("BREAK", {ScriptItem::pick(r2, {4, 0}), "-5,0"});
    CHECK(h.count(EntityKind::Ray) == 1);
    CHECK(h.count(EntityKind::Line) == 0);
    Fixture k;
    EntityId r3 = k.add(ray);
    k.host.run("BREAKATPOINT", {ScriptItem::pick(r3, {4, 0}), "4,0"});
    CHECK(k.count(EntityKind::Ray) == 1);
    CHECK(k.count(EntityKind::Line) == 1);
}

TEST_CASE("LENGTHEN works on a spline given only by fit points") {
    Fixture f;
    SplineData s;
    s.degree = 3;
    s.fit = {{0, 0}, {10, 10}, {20, -5}, {30, 0}};
    EntityId id = f.add(s);
    f.host.run("LENGTHEN", {"DE", "3", ScriptItem::pick(id, {29, 0}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Spline) == 1);
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE("FILLET and CHAMFER between a ray, a construction line and a line") {
    // A ray from the origin along +x and a vertical line: a sharp corner trims the ray's start
    // side to a finite line and keeps the far side of the vertical line.
    Fixture f;
    RayData ray;
    ray.origin = {0, 0};
    ray.dir = {1, 0};
    EntityId r = f.add(ray);
    EntityId v = f.add(LineData{{10, -20}, {10, 30}});
    f.host.run("FILLET", {"R", "0", ScriptItem::pick(r, {20, 0}), ScriptItem::pick(v, {10, 20})});
    const Entity& r2 = f.get(r);
    REQUIRE(entityAs<RayData>(r2) != nullptr);
    CHECK(near(entityAs<RayData>(r2)->origin, {10, 0}));  // the ray's far part is kept
    const auto* vl = entityAs<LineData>(f.get(v));
    REQUIRE(vl != nullptr);
    CHECK(near(vl->a, {10, 0}));

    // A construction line with a rounded corner: the kept side becomes a ray, plus an arc.
    Fixture g;
    RayData xl;
    xl.origin = {0, 0};
    xl.dir = {1, 0};
    xl.infinite = true;
    EntityId x = g.add(xl);
    EntityId w = g.add(LineData{{10, 0}, {10, 30}});
    g.host.run("FILLET", {"R", "2", ScriptItem::pick(x, {-30, 0}), ScriptItem::pick(w, {10, 20})});
    CHECK(g.count(EntityKind::Arc) == 1);
    const auto* kept = entityAs<RayData>(g.get(x));
    REQUIRE(kept != nullptr);
    CHECK(near(kept->origin, {8, 0}));
    CHECK(near(kept->dir, {-1, 0}));

    // CHAMFER with a ray.
    Fixture h;
    EntityId r3 = h.add(ray);
    EntityId v3 = h.add(LineData{{10, -20}, {10, 30}});
    h.host.run("CHAMFER",
               {"D", "2", "2", ScriptItem::pick(r3, {20, 0}), ScriptItem::pick(v3, {10, 20})});
    CHECK(h.count(EntityKind::Line) == 2);
    CHECK(h.count(EntityKind::Ray) == 1);

    // Parallel construction lines have no corner.
    Fixture k;
    EntityId p1 = k.add(xl);
    RayData up = xl;
    up.origin = {0, 5};
    EntityId p2 = k.add(up);
    k.host.run("FILLET", {"R", "1", ScriptItem::pick(p1, {3, 0}), ScriptItem::pick(p2, {3, 5})});
    CHECK(k.count(EntityKind::Arc) == 0);
}

TEST_CASE("DIVIDE and MEASURE space blocks by length along a spline and a polyline with arcs") {
    SplineData sp;
    sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    sp.control = {{0, 50}, {10, 70}, {30, 30}, {50, 60}, {60, 50}};
    const std::vector<EntityData> shapes{Indirect<SplineData>(sp),
                                         PolylineData{{{{0, 200}, 0.5, 0, 0},
                                                       {{20, 200}, 0, 0, 0},
                                                       {{20, 230}, -0.4, 0, 0},
                                                       {{45, 230}, 0, 0, 0}},
                                                      false}};
    for (const EntityData& shape : shapes) {
        Fixture f;
        {
            auto tx = f.doc.begin("block");
            (void)tx.addBlock(BlockDef{.name = "Tag"}).value();
            tx.commit();
        }
        EntityId id = f.add(shape);
        std::vector<geom::CurveRef> cs;
        geom::curvesOf(f.get(id), cs);
        // The curves in the direction of travel (an arc with a negative bulge is stored the other
        // way round), and the point at a length along them.
        std::vector<bool> reversed;
        Vec2 at{};
        double total = 0;
        for (std::size_t i = 0; i < cs.size(); ++i) {
            if (i == 0) {
                at = cs[i].startPoint();
                reversed.push_back(false);
            } else {
                reversed.push_back(distance(at, cs[i].endPoint()) <
                                   distance(at, cs[i].startPoint()));
            }
            at = reversed.back() ? cs[i].startPoint() : cs[i].endPoint();
            total += cs[i].length();
        }
        auto pointAtLength = [&](double s) {
            for (std::size_t i = 0; i < cs.size(); ++i) {
                const double len = cs[i].length();
                if (s <= len + 1e-9 || i + 1 == cs.size()) {
                    const double local = reversed[i] ? len - s : s;
                    return cs[i].pointAt(cs[i].paramAtLength(std::clamp(local, 0.0, len)));
                }
                s -= len;
            }
            return Vec2{};
        };
        auto placed = [&] {
            std::vector<Vec2> out;
            f.doc.forEachEntity([&](const Entity& e) {
                if (const auto* ins = entityAs<InsertData>(e))
                    out.push_back(ins->position);
            });
            return out;
        };
        auto expectAt = [&](const std::vector<Vec2>& got, double step) {
            REQUIRE(got.size() == 5);
            for (int k = 1; k <= 5; ++k) {
                const Vec2 want = pointAtLength(step * k);
                bool found = false;
                for (const Vec2& g : got)
                    found = found || distance(g, want) < 1e-3;
                CHECK(found);
            }
        };
        const Vec2 start = cs.front().pointAt(cs.front().t0());
        f.host.run("DIVIDE", {ScriptItem::pick(id, start), "Block", "Tag", "Yes", "6"});
        expectAt(placed(), total / 6.0);
        f.host.undo();
        const double step = total / 5.5;
        f.host.run("MEASURE",
                   {ScriptItem::pick(id, start), "Block", "Tag", "Yes", std::to_string(step)});
        expectAt(placed(), step);
    }
}

namespace {

TextData findText(const char* s, Vec2 p = {0, 0}) {
    TextData t;
    t.position = p;
    t.alignPoint = p;
    t.height = 2.5;
    t.utf8 = s;
    return t;
}

}  // namespace

TEST_CASE("FIND replaces in text, mtext, attributes, table cells, dimensions and leaders") {
    Fixture f;
    const EntityId text = f.add(findText("Door D1"));
    MTextData m;
    m.height = 2;
    m.width = 20;
    m.utf8 = "Fire door, steel door";
    const EntityId mtext = f.add(std::move(m));
    InsertData ins;
    {
        Transaction tx = f.doc.begin("blk");
        ins.block = tx.addBlock(BlockDef{.name = "TAG"}).value();
        tx.commit();
    }
    ins.attribs.push_back({"NAME", "door 7"});
    ins.attribs.push_back({"NOTE", "wall"});
    const EntityId insert = f.add(std::move(ins));
    TableData tb;
    tb.colWidths = {10, 10};
    tb.rowHeights = {5, 5};
    tb.cells.resize(4);
    tb.cells[0].utf8 = "door";
    tb.cells[3].utf8 = "Door and door";
    const EntityId table = f.add(std::move(tb));
    DimensionData d;
    d.textOverride = "2 doors";
    const EntityId dim = f.add(std::move(d));
    LeaderData ld;
    ld.vertices = {{0, 0}, {5, 5}};
    ld.utf8 = "see door";
    const EntityId leader = f.add(std::move(ld));
    const EntityId other = f.add(findText("Window W1", {0, 10}));

    (void)f.host.run("FIND", {"door", "gate", "No", "No"});
    CHECK(entityAs<TextData>(f.get(text))->utf8 == "gate D1");
    CHECK(entityAs<TextData>(f.get(other))->utf8 == "Window W1");
    CHECK(entityAs<MTextData>(f.get(mtext))->utf8 == "Fire gate, steel gate");
    CHECK(entityAs<InsertData>(f.get(insert))->attribs[0].value == "gate 7");
    CHECK(entityAs<InsertData>(f.get(insert))->attribs[1].value == "wall");
    const auto* cells = &entityAs<TableData>(f.get(table))->cells;
    CHECK((*cells)[0].utf8 == "gate");
    CHECK((*cells)[3].utf8 == "gate and gate");
    CHECK(entityAs<DimensionData>(f.get(dim))->textOverride == "2 gates");
    CHECK(entityAs<LeaderData>(f.get(leader))->utf8 == "see gate");
    CHECK(f.host.logContains("match(es)"));

    // One undo puts everything back.
    CHECK(f.host.undo());
    CHECK(entityAs<TextData>(f.get(text))->utf8 == "Door D1");
    CHECK(entityAs<TableData>(f.get(table))->cells[3].utf8 == "Door and door");
    CHECK(entityAs<DimensionData>(f.get(dim))->textOverride == "2 doors");
}

TEST_CASE("FIND honours Match case and an empty replacement only counts") {
    Fixture f;
    const EntityId a = f.add(findText("Door door DOOR"));
    (void)f.host.run("FIND", {"door", "", "No", "No"});
    CHECK(entityAs<TextData>(f.get(a))->utf8 == "Door door DOOR");
    CHECK(f.host.logContains("3 match(es) in 1 object(s)."));
    (void)f.host.run("FIND", {"door", "X", "Yes", "No"});
    CHECK(entityAs<TextData>(f.get(a))->utf8 == "Door X DOOR");
    (void)f.host.run("FIND", {"nothing", "X", "No", "No"});
    CHECK(f.host.logContains("No match for"));
}

TEST_CASE("FIND searches the layout sheets too") {
    Fixture f;
    const EntityId model = f.add(findText("Door in model"));
    EntityId sheet;
    {
        Transaction tx = f.doc.begin("sheet");
        const BlockId b = tx.addBlock(BlockDef{.name = "*LAYOUT-7"}).value();
        Entity e;
        e.data = findText("Door on sheet");
        sheet = tx.add(std::move(e), b).value();
        tx.commit();
    }
    (void)f.host.run("FIND", {"Door", "Gate", "No", "No"});
    CHECK(entityAs<TextData>(f.get(model))->utf8 == "Gate in model");
    CHECK(entityAs<TextData>(f.get(sheet))->utf8 == "Gate on sheet");
}

TEST_CASE("DRAWORDER of many objects keeps their order and stays fast") {
    Fixture f;
    std::vector<EntityId> ids;
    for (int i = 0; i < 30000; ++i)
        ids.push_back(f.add(LineData{{double(i), 0}, {i + 1.0, 1}}));
    std::vector<EntityId> some;
    for (int i = 0; i < 30000; i += 3)
        some.push_back(ids[i]);
    const auto t = std::chrono::steady_clock::now();
    f.host.setSelection(some);
    f.host.run("DRAWORDER", {ScriptItem::enter(), "Back"});
    f.host.run("DRAWORDER", {ScriptItem::enter(), "Front"});
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    // The selected third went to the back and then to the front, in their old relative order.
    const auto order = f.doc.modelSpace();
    REQUIRE(order.size() == ids.size());
    CHECK(std::vector<EntityId>(order.end() - static_cast<long>(some.size()), order.end()) == some);
    CHECK(order.front() == ids[1]);
    std::printf("[draworder] 2 x %zu objects: %.2f s\n", some.size(), took);
#ifdef NDEBUG
    CHECK(took < 3.0);
#endif
    CHECK(f.host.undo());
    CHECK(f.host.undo());
    CHECK(f.doc.modelSpace() == ids);
}

TEST_CASE("EXTEND a ray back to a boundary behind its start") {
    Fixture f;
    RayData ray;
    ray.origin = {0, 0};
    ray.dir = {1, 0};
    EntityId r = f.add(ray);
    f.add(LineData{{-7, -3}, {-7, 3}});
    f.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(r, {20, 0}), ScriptItem::enter()});
    const auto* got = entityAs<RayData>(f.get(r));
    REQUIRE(got != nullptr);
    CHECK(near(got->origin, {-7, 0}));
    CHECK(near(got->dir, {1, 0}));
    CHECK(f.host.undo());
    CHECK(near(entityAs<RayData>(f.get(r))->origin, {0, 0}));
    // A boundary ahead of the ray is no use, and a construction line has no end to move.
    Fixture g;
    EntityId r2 = g.add(ray);
    g.add(LineData{{7, -3}, {7, 3}});
    g.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(r2, {20, 0}), ScriptItem::esc()});
    CHECK(near(entityAs<RayData>(g.get(r2))->origin, {0, 0}));
    CHECK(g.host.logContains("No boundary"));
    Fixture h;
    RayData xl = ray;
    xl.infinite = true;
    EntityId x = h.add(xl);
    h.add(LineData{{-7, -3}, {-7, 3}});
    h.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(x, {20, 0}), ScriptItem::esc()});
    CHECK(near(entityAs<RayData>(h.get(x))->origin, {0, 0}));
}

TEST_CASE("JOIN fuses rays and construction lines on one line") {
    auto ray = [](Vec2 o, Vec2 d, bool inf = false) {
        RayData r;
        r.origin = o;
        r.dir = d;
        r.infinite = inf;
        return r;
    };
    {  // same way: the earlier start stays
        Fixture f;
        EntityId a = f.add(ray({0, 0}, {1, 0}));
        EntityId b = f.add(ray({5, 0}, {1, 0}));
        f.host.run("JOIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
        REQUIRE(f.count(EntityKind::Ray) == 1);
        f.doc.forEachEntity([&](const Entity& e) {
            const auto* r = entityAs<RayData>(e);
            CHECK_FALSE(r->infinite);
            CHECK(near(r->origin, {0, 0}));
        });
        CHECK(f.host.undo());
        CHECK(f.count(EntityKind::Ray) == 2);
    }
    {  // facing rays that overlap make a construction line
        Fixture f;
        EntityId a = f.add(ray({0, 0}, {1, 0}));
        EntityId b = f.add(ray({5, 0}, {-1, 0}));
        f.host.run("JOIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
        REQUIRE(f.count(EntityKind::Ray) == 1);
        f.doc.forEachEntity([&](const Entity& e) { CHECK(entityAs<RayData>(e)->infinite); });
    }
    {  // rays pointing away from each other with a gap between them stay apart
        Fixture f;
        EntityId a = f.add(ray({5, 0}, {1, 0}));
        EntityId b = f.add(ray({0, 0}, {-1, 0}));
        f.host.run("JOIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
        CHECK(f.count(EntityKind::Ray) == 2);
    }
    {  // a ray and a construction line on it; a parallel one is left alone
        Fixture f;
        EntityId a = f.add(ray({3, 0}, {1, 0}));
        EntityId b = f.add(ray({0, 0}, {1, 0}, true));
        EntityId c = f.add(ray({0, 4}, {1, 0}, true));
        f.host.run("JOIN", {ScriptItem::select({a, b, c}), ScriptItem::enter()});
        CHECK(f.count(EntityKind::Ray) == 2);
    }
}

TEST_CASE("FILLET and CHAMFER of two segments with an arc between them remove the arc") {
    auto make = [](Fixture& f) {
        return f.add(PolylineData{{{{0, 0}, 0, 0, 0},
                                   {{10, 0}, 0.4142135623730951, 0, 0},
                                   {{12, 2}, 0, 0, 0},
                                   {{12, 10}, 0, 0, 0}},
                                  false, 0});
    };
    {
        Fixture f;
        EntityId p = make(f);
        f.host.run("FILLET", {"R", "1", ScriptItem::pick(p, {3, 0}), ScriptItem::pick(p, {12, 8})});
        const auto* pl = entityAs<PolylineData>(f.get(p));
        REQUIRE(pl != nullptr);
        CHECK(pl->v.size() == 4);  // line, rounded corner, line
        CHECK(f.host.undoStack().undoLabel() == "FILLET");
        f.host.undo();
        CHECK(entityAs<PolylineData>(f.get(p))->v.size() == 4);
        CHECK(near(entityAs<PolylineData>(f.get(p))->v[1].p, {10, 0}));
    }
    {
        Fixture f;
        EntityId p = make(f);
        f.host.run("CHAMFER", {"Distance", "1", "1", ScriptItem::pick(p, {3, 0}),
                               ScriptItem::pick(p, {12, 8})});
        const auto* pl = entityAs<PolylineData>(f.get(p));
        REQUIRE(pl != nullptr);
        REQUIRE(pl->v.size() == 4);
        CHECK(near(pl->v[1].p, {11, 0}));
        CHECK(near(pl->v[2].p, {12, 1}));
    }
}

TEST_CASE("MATCHPROP copies every basic property, the plot style included, and undoes in one step") {
    Fixture f;
    LayerId walls{};
    {
        Transaction tx = f.doc.begin("layer");
        Layer l;
        l.name = "Walls";
        walls = tx.addLayer(l).value();
        tx.commit();
    }
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(CircleData{{0, 5}, 2});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(a, [&](Entity& e) {
            e.common.layer = walls;
            e.common.color = Color::index(3);
            e.common.lineweight = LineWeight::mm(0.5);
            e.common.linetypeScale = 2;
            e.common.transparency = 25;
        }));
        (void)tx.modifyHeader([&](Header& h) { setEntityPlotStyle(h, a, "Heavy"); });
        tx.commit();
    }
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {5, 0}), ScriptItem::pick(b, {2, 5}), ScriptItem::esc()});
    const EntityCommon& c = f.get(b).common;
    CHECK(c.layer == walls);
    CHECK(c.color == Color::index(3));
    CHECK(c.lineweight == LineWeight::mm(0.5));
    CHECK(c.linetypeScale == doctest::Approx(2));
    CHECK(c.transparency == 25);
    CHECK(entityPlotStyle(f.doc.header(), b) == "Heavy");
    CHECK(f.host.undo());
    CHECK(f.get(b).common.layer != walls);
    CHECK(f.get(b).common.transparency == kTransparencyByLayer);
    CHECK(entityPlotStyle(f.doc.header(), b).empty());
}

TEST_CASE("MATCHPROP gives text its height, style, width and slant, and polylines their width") {
    Fixture f;
    TextData ta;
    ta.height = 7;
    ta.widthFactor = 0.8;
    ta.oblique = 0.2;
    ta.utf8 = "one";
    TextData tb;
    tb.utf8 = "two";
    tb.position = {0, 10};
    EntityId a = f.add(ta);
    EntityId b = f.add(tb);
    PolylineData pa;
    pa.constantWidth = 1.5;
    pa.v = {{{0, 0}, 0, 1.5, 1.5}, {{4, 0}, 0, 1.5, 1.5}};
    PolylineData pb;
    pb.v = {{{0, 20}, 0, 0, 0}, {{4, 20}, 0.3, 0.5, 2}, {{8, 25}, 0, 0, 0}};
    EntityId pA = f.add(pa);
    EntityId pB = f.add(pb);
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {0, 0}), ScriptItem::pick(b, {0, 10}), ScriptItem::esc()});
    const auto* t = entityAs<TextData>(f.get(b));
    CHECK(t->height == doctest::Approx(7));
    CHECK(t->widthFactor == doctest::Approx(0.8));
    CHECK(t->oblique == doctest::Approx(0.2));
    CHECK(t->utf8 == "two");
    CHECK(near(t->position, {0, 10}));
    f.host.run("MATCHPROP",
               {ScriptItem::pick(pA, {2, 0}), ScriptItem::pick(pB, {2, 20}), ScriptItem::esc()});
    const auto* p = entityAs<PolylineData>(f.get(pB));
    CHECK(p->constantWidth == doctest::Approx(1.5));
    for (const PolyVertex& v : p->v) {
        CHECK(v.startWidth == doctest::Approx(1.5));
        CHECK(v.endWidth == doctest::Approx(1.5));
    }
    CHECK(near(p->v[1].p, {4, 20}));
    CHECK(p->v[1].bulge == doctest::Approx(0.3));  // the shape does not change
}

TEST_CASE("MATCHPROP between unlike kinds copies only what both have, Settings turn Basic off") {
    Fixture f;
    TextData ta;
    ta.height = 9;
    EntityId a = f.add(ta);
    EntityId b = f.add(LineData{{0, 0}, {5, 0}});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(a, [](Entity& e) { e.common.color = Color::index(1); }));
        tx.commit();
    }
    const LineData before = *entityAs<LineData>(f.get(b));
    f.host.run("MATCHPROP", {ScriptItem::pick(a, {0, 0}), "Settings", "Basic", ScriptItem::enter(),
                             ScriptItem::pick(b, {2, 0}), ScriptItem::esc()});
    CHECK(f.get(b).common.color == Color::byLayer());  // Basic is off
    CHECK(near(entityAs<LineData>(f.get(b))->b, before.b));
    f.host.run("MATCHPROP", {ScriptItem::pick(a, {0, 0}), "Settings", "Basic", ScriptItem::enter(),
                             ScriptItem::pick(b, {2, 0}), ScriptItem::esc()});  // Basic back on
    CHECK(f.get(b).common.color == Color::index(1));
}

TEST_CASE("MATCHPROP does not touch objects on locked layers") {
    Fixture f;
    LayerId locked{};
    {
        Transaction tx = f.doc.begin("layer");
        Layer l;
        l.name = "Locked";
        l.locked = true;
        locked = tx.addLayer(l).value();
        tx.commit();
    }
    EntityId a = f.add(LineData{{0, 0}, {10, 0}});
    EntityId b = f.add(LineData{{0, 3}, {10, 3}});
    {
        Transaction tx = f.doc.begin("set");
        REQUIRE(tx.modify(b, [&](Entity& e) { e.common.layer = locked; }));
        REQUIRE(tx.modify(a, [](Entity& e) { e.common.color = Color::index(2); }));
        tx.commit();
    }
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {5, 0}), ScriptItem::pick(b, {5, 3}), ScriptItem::esc()});
    CHECK(f.get(b).common.color == Color::byLayer());
    CHECK(f.get(b).common.layer == locked);
}

TEST_CASE("MATCHPROP passes on text justification and keeps the text where it hangs") {
    Fixture f;
    TextData ta;
    ta.h = HAlign::Center;
    ta.v = VAlign::Middle;
    ta.position = ta.alignPoint = {0, 0};
    ta.utf8 = "src";
    TextData tb;
    tb.position = tb.alignPoint = {20, 5};
    tb.utf8 = "dst";
    TextData tc;  // Fit uses two points: left alone
    tc.h = HAlign::Fit;
    tc.position = {0, 30};
    tc.alignPoint = {10, 30};
    tc.utf8 = "fit";
    EntityId a = f.add(ta);
    EntityId b = f.add(tb);
    EntityId c = f.add(tc);
    f.host.run("MATCHPROP", {ScriptItem::pick(a, {0, 0}), ScriptItem::pick(b, {20, 5}),
                             ScriptItem::pick(c, {5, 30}), ScriptItem::esc()});
    const auto* t = entityAs<TextData>(f.get(b));
    CHECK(t->h == HAlign::Center);
    CHECK(t->v == VAlign::Middle);
    CHECK(near(t->alignPoint, {20, 5}));
    const auto* u = entityAs<TextData>(f.get(c));
    CHECK(u->h == HAlign::Fit);
    CHECK(near(u->alignPoint, {10, 30}));
    // And back to Left/Baseline: the anchor becomes the insertion point.
    TextData td;
    td.utf8 = "left";
    td.position = td.alignPoint = {50, 50};
    EntityId d = f.add(td);
    f.host.run("MATCHPROP",
               {ScriptItem::pick(d, {50, 50}), ScriptItem::pick(b, {20, 5}), ScriptItem::esc()});
    t = entityAs<TextData>(f.get(b));
    CHECK(t->h == HAlign::Left);
    CHECK(near(t->position, {20, 5}));
}

TEST_CASE("MATCHPROP gives dimensions and leaders the source's style") {
    Fixture f;
    DimStyleId big{};
    {
        Transaction tx = f.doc.begin("style");
        DimStyle st;
        st.name = "Big";
        st.textHeight = 5;
        big = tx.addDimStyle(st).value();
        tx.commit();
    }
    DimensionData da;
    da.defPts = {Vec2{0, 0}, Vec2{10, 0}, Vec2{10, 3}, Vec2{}};
    da.style = big;
    DimensionData db;
    db.defPts = {Vec2{0, 20}, Vec2{10, 20}, Vec2{10, 23}, Vec2{}};
    EntityId a = f.add(da);
    EntityId b = f.add(db);
    LeaderData la;
    la.vertices = {{0, 40}, {5, 45}};
    la.style = big;
    LeaderData lb;
    lb.vertices = {{20, 40}, {25, 45}};
    EntityId c = f.add(la);
    EntityId d = f.add(lb);
    f.host.run("MATCHPROP",
               {ScriptItem::pick(a, {5, 3}), ScriptItem::pick(b, {5, 23}), ScriptItem::esc()});
    CHECK(entityAs<DimensionData>(f.get(b))->style == big);
    CHECK(near(entityAs<DimensionData>(f.get(b))->defPts[1], {10, 20}));
    f.host.run("MATCHPROP",
               {ScriptItem::pick(c, {0, 40}), ScriptItem::pick(d, {20, 40}), ScriptItem::esc()});
    CHECK(entityAs<LeaderData>(f.get(d))->style == big);
}

TEST_CASE("ALIGN asks for a third pair like AutoCAD, scales on request, and dimensions follow") {
    {
        // two pairs, Enter at the third, then Yes: the line is scaled to the destination
        Fixture f;
        EntityId l = f.add(LineData{{0, 0}, {10, 0}});
        REQUIRE(f.host.run("ALIGN", {ScriptItem::select({l}), ScriptItem::enter(), "0,0", "5,5",
                                     "10,0", "5,25", ScriptItem::enter(), "Yes"})
                    .finished);
        const auto* ld = entityAs<LineData>(f.get(l));
        CHECK(near(ld->a, {5, 5}));
        CHECK(near(ld->b, {5, 25}));
        CHECK(f.host.undoStack().undoCount() == 1);
    }
    {
        // a third pair is accepted and ignored in a flat drawing; No keeps the size
        Fixture f;
        EntityId l = f.add(LineData{{0, 0}, {10, 0}});
        REQUIRE(f.host.run("ALIGN", {ScriptItem::select({l}), ScriptItem::enter(), "0,0", "5,5",
                                     "10,0", "5,25", "3,3", "8,8", "No"})
                    .finished);
        const auto* ld = entityAs<LineData>(f.get(l));
        CHECK(near(ld->b, {5, 15}));
        CHECK(f.host.logContains("third pair"));
    }
    {
        // a dimension aligned with its line keeps measuring it
        Fixture f;
        EntityId l = f.add(LineData{{0, 0}, {10, 0}});
        REQUIRE(f.host.run("DIMALIGNED", {"0,0", "10,0", "5,-3"}).finished);
        std::vector<EntityId> all;
        EntityId dim{};
        f.doc.forEachEntity([&](const Entity& e) {
            all.push_back(e.common.id);
            if (entityAs<DimensionData>(e))
                dim = e.common.id;
        });
        REQUIRE(dim);
        REQUIRE(f.host.run("ALIGN", {ScriptItem::select(all), ScriptItem::enter(), "0,0", "0,0",
                                     "10,0", "0,20", "Yes"})
                    .finished);
        CHECK(near(entityAs<LineData>(f.get(l))->b, {0, 20}));
        CHECK(entityAs<DimensionData>(f.get(dim))->measurement == doctest::Approx(20));
    }
}
