#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/inquiry/inquiry.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    Fixture() { registerAllCommands(reg); }

    EntityId add(Entity e) {
        Transaction tx = doc.begin("setup");
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    }
};

Entity line(Vec2 a, Vec2 b) {
    return Entity{{}, LineData{a, b}};
}

PolylineData rect(double w, double h) {
    PolylineData pl;
    pl.closed = true;
    pl.v = {{{0, 0}}, {{w, 0}}, {{w, h}}, {{0, h}}};
    return pl;
}

}  // namespace

TEST_CASE("commands and aliases are registered") {
    Fixture f;
    for (const char* n : {"DIST", "DI", "AREA", "AA", "ANGLE", "ID", "LIST", "LI", "COPYRESULT"})
        CHECK_MESSAGE(f.reg.find(n) != nullptr, n);
}

TEST_CASE("DIST: 3-4-5 triangle") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    auto r = host.run("DI", {"0,0", "3,4"});
    CHECK(r.finished);
    CHECK(host.logContains("Distance = 5.00"));
    CHECK(host.logContains("Delta X = 3.00"));
    CHECK(host.logContains("Delta Y = 4.00"));
    CHECK(host.logContains("Angle in XY plane = 53.13"));
    CHECK_FALSE(host.undoStack().canUndo());  // inquiries never touch the drawing
}

TEST_CASE("ID prints coordinates") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "1");
    host.run("ID", {"12.5,-3"});
    CHECK(host.logContains("X = 12.5"));
    CHECK(host.logContains("Y = -3.0"));
}

TEST_CASE("AREA by points: 3-4-5 triangle") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    auto r = host.run("AREA", {"0,0", "3,0", "3,4", ""});
    CHECK(r.finished);
    CHECK(host.logContains("Area = 6.00"));
    CHECK(host.logContains("Perimeter = 12.00"));
}

TEST_CASE("AREA by object: circle, polyline, semicircle bulge") {
    Fixture f;
    EntityId c = f.add(Entity{{}, CircleData{{0, 0}, 10}});
    PolylineData sq = rect(10, 10);
    EntityId p = f.add(Entity{{}, sq});
    PolylineData semi;  // chord (0,0)-(2,0) with a semicircle bulge, closed by that same chord
    semi.closed = true;
    semi.v = {{{0, 0}, 1.0}, {{2, 0}}};
    EntityId s = f.add(Entity{{}, semi});

    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "4");
    using cmd::ScriptItem;
    host.run("AREA", {"O", ScriptItem::pick(c)});
    CHECK(host.logContains("Area = 314.1593"));  // pi * 10^2
    CHECK(host.logContains("Perimeter = 62.8319"));
    host.clearLog();
    host.run("AREA", {"O", ScriptItem::pick(p)});
    CHECK(host.logContains("Area = 100.0000"));
    CHECK(host.logContains("Perimeter = 40.0000"));
    host.clearLog();
    host.run("AREA", {"O", ScriptItem::pick(s)});
    CHECK(host.logContains("Area = 1.5708"));       // pi / 2
    CHECK(host.logContains("Perimeter = 5.1416"));  // pi + 2
}

TEST_CASE("AREA by object: ellipse and closed spline") {
    Fixture f;
    EllipseData el;
    el.center = {0, 0};
    el.majorAxis = {5, 0};
    el.ratio = 0.5;
    EntityId e = f.add(Entity{{}, el});
    auto r = inquiry::areaOfEntity(*f.doc.find(e));
    REQUIRE(r);
    CHECK(r->area == doctest::Approx(kPi * 5 * 2.5));

    // A circle written as a closed rational quadratic-ish check is covered by geom; here a
    // periodic polygon-like spline just has to give a positive area.
    SplineData sp;
    sp.degree = 1;
    sp.closed = true;
    sp.control = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}};
    sp.knots = {0, 0, 1, 2, 3, 4, 4};
    EntityId se = f.add(Entity{{}, sp});
    auto sr = inquiry::areaOfEntity(*f.doc.find(se));
    REQUIRE(sr);
    CHECK(sr->area == doctest::Approx(100).epsilon(1e-6));
    CHECK(sr->perimeter == doctest::Approx(40).epsilon(1e-6));
}

TEST_CASE("AREA Add and Subtract keep a running total") {
    Fixture f;
    EntityId big = f.add(Entity{{}, rect(10, 10)});  // 100
    EntityId hole = f.add(Entity{{}, rect(2, 5)});   // 10
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    using cmd::ScriptItem;
    auto r = host.run("AREA", {"A", "O", ScriptItem::pick(big), "S", "O", ScriptItem::pick(hole),
                               ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(host.logContains("Total area = 100.00"));
    CHECK(host.logContains("Total area = 90.00"));
    CHECK_FALSE(host.undoStack().canUndo());

    // Points inside Add mode.
    host.clearLog();
    host.run("AREA",
             {"A", "0,0", "10,0", "10,10", "", "0,0", "5,0", "5,5", "", ScriptItem::enter()});
    CHECK(host.logContains("Total area = 50.00"));
    CHECK(host.logContains("Total area = 62.50"));
}

TEST_CASE("AREA and LIST honour document units") {
    Fixture f;
    EntityId c = f.add(Entity{{}, CircleData{{0, 0}, 500}});
    {
        Transaction tx = f.doc.begin("units");
        REQUIRE(tx.modifyHeader([](Header& h) { h.units = LengthUnit::Meter; }));
        tx.commit();
    }
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "4");
    host.run("AREA", {"O", cmd::ScriptItem::pick(c)});
    CHECK(host.logContains("Area = 0.7854"));  // pi * 0.5^2 m^2
}

TEST_CASE("ANGLE: lines at 30 degrees and three points") {
    Fixture f;
    const double c30 = std::cos(kPi / 6), s30 = std::sin(kPi / 6);
    EntityId a = f.add(line({0, 0}, {100, 0}));
    EntityId b = f.add(line({0, 0}, {100 * c30, 100 * s30}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    using cmd::ScriptItem;
    auto r = host.run("ANGLE",
                      {ScriptItem::pick(a, {80, 0}), ScriptItem::pick(b, {80 * c30, 80 * s30})});
    CHECK(r.finished);
    CHECK(host.logContains("Angle = 30.00"));

    host.clearLog();
    host.run("ANGLE", {"3", "0,0", "10,0", "0,10"});
    CHECK(host.logContains("Angle = 90.00"));

    CHECK(inquiry::angleAtVertex({0, 0}, {1, 0}, {-1, 0}) == doctest::Approx(kPi));
}

TEST_CASE("LIST describes selected entities") {
    Fixture f;
    EntityId l = f.add(line({0, 0}, {3, 4}));
    EntityId c = f.add(Entity{{}, CircleData{{1, 2}, 5}});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    host.setSelection({l, c});
    auto r = host.run("LI", {});
    CHECK(r.finished);
    CHECK(host.logContains("Layer: 0"));
    CHECK(host.logContains("Length = 5.00"));
    CHECK(host.logContains("Radius = 5.00"));
    CHECK(host.logContains("Area = 78.54"));
}

TEST_CASE("COPYRESULT copies the last result") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    host.run("COPYRESULT", {});
    CHECK(host.logContains("no measurement result"));
    host.run("DI", {"0,0", "3,4"});
    host.run("COPYRESULT", {});
    auto clip = host.setting("clipboard.text");
    REQUIRE(clip);
    CHECK(clip->find("Distance = 5.00") != std::string::npos);
}

TEST_CASE("AUDIT reports broken objects and removes them on request") {
    Fixture f;
    f.add(line({0, 0}, {10, 0}));
    EntityId zero = f.add(line({5, 5}, {5, 5}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("AUDIT", {"No"});
    CHECK(host.logContains("1 broken object"));
    CHECK(f.doc.contains(zero));
    host.run("AUDIT", {"Yes"});
    CHECK(host.logContains("Removed 1"));
    CHECK_FALSE(f.doc.contains(zero));
    CHECK(f.doc.modelSpaceCount() == 1);
    host.clearLog();
    host.run("AUDIT", {});
    CHECK(host.logContains("No problems found"));
}

TEST_CASE("MASSPROP: rectangle centroid and moments") {
    Document doc;
    PolylineData pl;
    pl.closed = true;
    for (Vec2 p : {Vec2{10, 20}, Vec2{40, 20}, Vec2{40, 60}, Vec2{10, 60}})
        pl.v.push_back(PolyVertex{p, 0});
    Entity e{{}, pl};
    auto m = inquiry::massPropsOfEntity(e);
    REQUIRE(m);
    CHECK(m->area == doctest::Approx(1200));
    CHECK(m->centroid.x == doctest::Approx(25));
    CHECK(m->centroid.y == doctest::Approx(40));
    CHECK(m->ixx == doctest::Approx(30.0 * 40 * 40 * 40 / 12));
    CHECK(m->iyy == doctest::Approx(40.0 * 30 * 30 * 30 / 12));
    CHECK(m->ixy == doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("MASSPROP: circle moment is pi r^4 / 4") {
    CircleData c;
    c.center = {5, 5};
    c.radius = 10;
    Entity e{{}, c};
    auto m = inquiry::massPropsOfEntity(e);
    REQUIRE(m);
    CHECK(m->ixx == doctest::Approx(kPi * 1e4 / 4).epsilon(1e-3));
    CHECK(m->centroid.x == doctest::Approx(5).epsilon(1e-4));
}

TEST_CASE("DWGPROPS stores the drawing properties in the header") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("DWGPROPS", {"Title", "Floor plan", "Author", "Tong", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().custom.at("tbcad.prop.title") == "Floor plan");
    CHECK(f.doc.header().custom.at("tbcad.prop.author") == "Tong");
    host.run("DWGPROPS", {"Title", "", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().custom.count("tbcad.prop.title") == 0);
}

TEST_CASE("FINDSTRAY leaves ordinary objects alone") {
    Fixture f;
    f.add(line({0, 0}, {10, 0}));
    f.add(Entity{{}, rect(5, 5)});
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("FINDSTRAY", {});
    CHECK(host.logContains("No stray lines found"));
}
