#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
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

TEST_CASE("MATCHPROP copies hatch pattern and transparency; Settings can switch a group off") {
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

TEST_CASE("FIND replaces text, only counts without a replacement, and undoes in one step") {
    Fixture f;
    TextData td;
    td.utf8 = "Room 1 / room 2";
    EntityId t = f.add(Indirect<TextData>(td));
    f.host.run("FIND", {"room", "", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / room 2");
    CHECK(f.host.selection() == std::vector<EntityId>{t});  // the matches are selected
    f.host.run("FIND", {"room", "Hall", "No"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Hall 1 / Hall 2");
    f.host.undo();
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / room 2");
    f.host.run("FIND", {"room", "Hall", "Yes"});
    CHECK(entityAs<TextData>(f.get(t))->utf8 == "Room 1 / Hall 2");
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
