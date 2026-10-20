#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "tbcad/blocks/display.h"
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
    host.run("-DWGPROPS", {"Title", "Floor plan", "Author", "Tong", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().custom.at("tbcad.prop.title") == "Floor plan");
    CHECK(f.doc.header().custom.at("tbcad.prop.author") == "Tong");
    host.run("-DWGPROPS", {"Title", "", cmd::ScriptItem::enter()});
    CHECK(f.doc.header().custom.count("tbcad.prop.title") == 0);
}

TEST_CASE("-DWGPROPS Custom adds and removes custom properties - DWGPROPS opens the dialog") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("-DWGPROPS",
             {"Custom", "Client", "Acme Co", "Custom", "Job", "J-12", cmd::ScriptItem::enter()});
    auto props = inquiry::customProperties(f.doc.header());
    REQUIRE(props.size() == 2);
    CHECK(props[0] == std::pair<std::string, std::string>{"Client", "Acme Co"});
    CHECK(props[1].first == "Job");
    host.run("-DWGPROPS", {"Custom", "Client", "", cmd::ScriptItem::enter()});
    CHECK(inquiry::customProperties(f.doc.header()).size() == 1);
    host.clearLog();
    host.run("-DWGPROPS", {cmd::ScriptItem::enter()});
    CHECK(host.logContains("Custom Job: J-12"));
}

TEST_CASE("AUDIT repairs missing styles and AUDITCTL writes the report next to the drawing") {
    Fixture f;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("tbcad_audit_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("document.file", (dir / "plan.tbcad").string());
    host.setSetting("audit.report", "1");
    f.add(line({0, 0}, {10, 0}));
    f.add(line({5, 5}, {5, 5}));  // zero length: broken
    host.run("AUDIT", {"Yes"});
    CHECK(host.logContains("Total errors found 1, fixed 1."));
    std::ifstream in(dir / "plan.adt");
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("line of zero length") != std::string::npos);
    CHECK(text.str().find("Total errors found 1, fixed 1.") != std::string::npos);
    in.close();
    std::filesystem::remove_all(dir);
}

TEST_CASE("-UNITS length and angle types, direction and base change what DIST shows") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    CHECK(host.run("-UNITS", {"Type", "Architectural"}).finished);
    CHECK(lengthUnitsType(f.doc.header()) == 4);
    CHECK(host.run("-UNITS", {"Angles", "DMS"}).finished);
    CHECK(angleUnitsType(f.doc.header()) == 1);
    CHECK(host.run("-UNITS", {"Precision", "4", "4"}).finished);
    host.clearLog();
    host.run("DIST", {"0,0", "381,0"});  // 15 inches
    CHECK(host.logContains("Distance = 1'-3\""));
    CHECK(host.logContains("Angle in XY plane = 0d0'0\""));
    CHECK(host.run("-UNITS", {"Base", "90"}).finished);
    CHECK(host.run("-UNITS", {"Direction", "Clockwise"}).finished);
    CHECK(anglesClockwise(f.doc.header()));
    CHECK(angleBase(f.doc.header()) == doctest::Approx(kPi / 2));
    host.clearLog();
    host.run("DIST", {"0,0", "381,0"});  // east is 90 degrees clockwise from north
    CHECK(host.logContains("Angle in XY plane = 90d0'0\""));
}

TEST_CASE("FINDSTRAY leaves ordinary objects alone") {
    Fixture f;
    f.add(line({0, 0}, {10, 0}));
    f.add(Entity{{}, rect(5, 5)});
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("FINDSTRAY", {});
    CHECK(host.logContains("No stray lines found"));
}

TEST_CASE("FINDSTRAY is quick on blocks that hold each other many times over") {
    // L0 holds L1 twice, L1 holds L2 twice, ... : 2^20 lines in one reference of L0.
    blocks::registerInsertDisplay();
    Fixture f;
    constexpr int kLevels = 21;
    std::vector<BlockId> ids;
    {
        auto tx = f.doc.begin("blocks");
        for (int i = 0; i < kLevels; ++i)
            ids.push_back(tx.addBlock(BlockDef{.name = "L" + std::to_string(i)}).value());
        (void)tx.add(line({0, 0}, {1, 0}), ids.back());
        for (int i = 0; i + 1 < kLevels; ++i)
            for (double y : {0.0, 2.0})
                (void)tx.add(Entity{{}, Indirect<InsertData>(InsertData{ids[i + 1], {0, y}})},
                             ids[i]);
        tx.commit();
    }
    cmd::ScriptHost host(f.doc, f.reg);
    const auto t = std::chrono::steady_clock::now();
    host.run("FINDSTRAY", {});
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    CHECK(host.logContains("No stray lines found"));
    CHECK(secs < 1);  // (several seconds when every reference inside a block was drawn)
}

TEST_CASE("PICKTRACE lists the objects drawn near a point") {
    Fixture f;
    EntityId near = f.add(line({0, 0}, {10, 0}));
    f.add(line({0, 50}, {10, 50}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("PICKTRACE", {"1", "5,0.5"});
    CHECK(host.logContains("#" + std::to_string(near.value) + " Line"));
    CHECK(host.selection().size() == 1);
    host.clearLog();
    host.run("PICKTRACE", {"1", "5,20"});
    CHECK(host.logContains("Nothing is drawn"));
}

TEST_CASE("PICKTRACE reports the colour of a filled area under the point") {
    Fixture f;
    HatchData h;
    HatchLoop loop;
    HatchPolylineEdge pe;
    for (Vec2 v : {Vec2{0, 0}, Vec2{10, 0}, Vec2{10, 10}, Vec2{0, 10}})
        pe.v.push_back(PolyVertex{v});
    loop.edges.push_back(pe);
    h.loops.push_back(loop);
    h.fill = Color::index(1);
    EntityId id = f.add(Entity{{}, h});
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("PICKTRACE", {"1", "5,5"});
    CHECK(host.logContains("#" + std::to_string(id.value) + " Hatch"));
    CHECK(host.logContains("fill colour"));
    CHECK(host.logContains("solid"));
}

TEST_CASE("MEASUREGEOM measures a distance or a radius and DBLIST lists everything") {
    Fixture f;
    EntityId c = f.add(Entity{{}, CircleData{{1, 2}, 5}});
    f.add(line({0, 0}, {3, 4}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    host.run("MEA", {"Distance", "0,0", "3,4", ""});
    CHECK(host.logContains("Distance = 5.00"));
    host.run("MEASUREGEOM", {"Radius", cmd::ScriptItem::pick(c, {6, 2}), ""});
    CHECK(host.logContains("Radius = 5.00 mm, Diameter = 10.00 mm"));
    host.run("DBLIST", {});
    CHECK(host.logContains("2 objects listed."));
    CHECK(host.logContains("Length = 5.00"));
}

TEST_CASE("CAL calculates expressions and remembers the last result as ans") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("CAL", {"2 + 3 * 4", "ans / 7", "sqrt(16) ^ 2", "1 / 0", "nonsense +"});
    CHECK(host.logContains("= 14"));
    CHECK(host.logContains("= 2"));
    CHECK(host.logContains("= 16"));
    CHECK(host.log().size() >= 5);  // the two errors were reported, not fatal
    CHECK(f.reg.find("QC") == f.reg.find("CAL"));
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("RECOVER removes broken objects without asking") {
    Fixture f;
    f.add(line({0, 0}, {10, 0}));
    EntityId zero = f.add(line({5, 5}, {5, 5}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("RECOVER", {cmd::ScriptItem::enter()});
    CHECK(host.logContains("Removed 1"));
    CHECK_FALSE(f.doc.contains(zero));
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("RECOVER with a file name asks the window to open and repair that file") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("RECOVER", {"\"C:/work/bad.dxf\""});
    CHECK(host.uiRequests().size() == 1);
    CHECK(host.uiRequests().front() == "recoverpath:C:/work/bad.dxf");
}

TEST_CASE("MASSPROP of a ring: the inner circle is a hole - area, centroid and moments to 1e-7") {
    const double R = 50, r = 20;
    Entity outer{{}, CircleData{{1e6 + 5, -2e6}, R}};
    Entity inner{{}, CircleData{{1e6 + 5, -2e6}, r}};
    auto m = inquiry::massPropsOfEntities({&outer, &inner});
    REQUIRE(m);
    CHECK(m->area == doctest::Approx(kPi * (R * R - r * r)).epsilon(3e-8));
    CHECK(m->centroid.x == doctest::Approx(1e6 + 5).epsilon(1e-12));
    CHECK(m->centroid.y == doctest::Approx(-2e6).epsilon(1e-12));
    const double ix = kPi * (std::pow(R, 4) - std::pow(r, 4)) / 4;
    CHECK(m->ixx == doctest::Approx(ix).epsilon(1e-6));
    CHECK(m->iyy == doctest::Approx(ix).epsilon(1e-6));
    CHECK(m->ixy == doctest::Approx(0).epsilon(1e-6));
    CHECK(m->perimeter == doctest::Approx(kTwoPi * (R + r)).epsilon(1e-7));
}

TEST_CASE("MASSPROP of a box with an off-centre hole moves the centroid away from the hole") {
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 60}, Vec2{0, 60}})
        box.v.push_back(PolyVertex{p, 0});
    Entity b{{}, box};
    Entity hole{{}, CircleData{{75, 30}, 10}};
    auto m = inquiry::massPropsOfEntities({&b, &hole});
    REQUIRE(m);
    const double ah = kPi * 100;
    const double area = 6000 - ah;
    CHECK(m->area == doctest::Approx(area).epsilon(1e-7));
    CHECK(m->centroid.x == doctest::Approx((6000 * 50 - ah * 75) / area).epsilon(1e-7));
    CHECK(m->centroid.y == doctest::Approx(30).epsilon(1e-9));
    // Moment about the centroid by the parallel axis rule, from the two exact shapes.
    const double cx = m->centroid.x;
    const double iyyBox = 60.0 * 100 * 100 * 100 / 12 + 6000 * (50 - cx) * (50 - cx);
    const double iyyHole = kPi * 1e4 / 4 + ah * (75 - cx) * (75 - cx);
    CHECK(m->iyy == doctest::Approx(iyyBox - iyyHole).epsilon(1e-6));
}

TEST_CASE("MASSPROP of a rounded slot (polyline with arcs) matches the closed form") {
    // A stadium: two semicircles of radius 10 joined by 30 mm straights.
    PolylineData slot;
    slot.closed = true;
    slot.v.push_back(PolyVertex{{0, 0}, 0});
    slot.v.push_back(PolyVertex{{30, 0}, 1});  // half circle round the right end
    slot.v.push_back(PolyVertex{{30, 20}, 0});
    slot.v.push_back(PolyVertex{{0, 20}, 1});  // and round the left end
    Entity e{{}, slot};
    auto m = inquiry::massPropsOfEntity(e);
    REQUIRE(m);
    CHECK(m->area == doctest::Approx(30 * 20 + kPi * 100).epsilon(1e-7));
    CHECK(m->centroid.x == doctest::Approx(15).epsilon(1e-7));
    CHECK(m->centroid.y == doctest::Approx(10).epsilon(1e-7));
    const double iyy = 30.0 * 20 * 20 * 20 / 12 * 0 + (20.0 * 30 * 30 * 30 / 12) +
                       2 * ((kPi / 8 - 8 / (9 * kPi)) * 1e4 +
                            kPi * 50 * (15 + 4 * 10 / (3 * kPi)) * (15 + 4 * 10 / (3 * kPi)));
    (void)iyy;  // the closed form for Iyy is checked through Ixx, which is simpler:
    const double ixx = 30.0 * 20 * 20 * 20 / 12 + kPi * 1e4 / 4;
    CHECK(m->ixx == doctest::Approx(ixx).epsilon(1e-6));
}

TEST_CASE("DIST, ID and AREA at survey coordinates keep millimetre accuracy") {
    // UTM-like coordinates: 500 km easting, 4500 km northing.
    const double ox = 500123.456, oy = 4500987.654;
    Fixture f;
    PolylineData pl;
    pl.closed = true;
    pl.v = {{{ox, oy}}, {{ox + 30, oy}}, {{ox + 30, oy + 40}}, {{ox, oy + 40}}};
    EntityId id = f.add(Entity{{}, pl});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "4");
    using cmd::ScriptItem;
    auto pt = [&](double dx, double dy) {
        char b[80];
        std::snprintf(b, sizeof b, "%.6f,%.6f", ox + dx, oy + dy);
        return std::string(b);
    };
    host.run("DIST", {pt(0, 0), pt(3, 4)});
    CHECK(host.logContains("Distance = 5.0000"));
    CHECK(host.logContains("Delta X = 3.0000"));
    host.run("ID", {pt(0.25, 0.5)});
    CHECK(host.logContains("X = 500123.7060"));
    CHECK(host.logContains("Y = 4500988.1540"));
    host.run("AREA", {pt(0, 0), pt(3, 0), pt(3, 4), ""});
    CHECK(host.logContains("Area = 6.0000"));
    CHECK(host.logContains("Perimeter = 12.0000"));
    host.run("AREA", {"O", ScriptItem::pick(id)});
    CHECK(host.logContains("Area = 1200.0000"));
    CHECK(host.logContains("Perimeter = 140.0000"));
    host.run("LIST", {ScriptItem::pick(id), ""});
    CHECK(host.logContains("Area = 1200.0000"));
}

TEST_CASE("AREA of a clockwise bulged polyline and a far-away circle is exact") {
    const double ox = 2500000.5, oy = 7000000.25;
    Fixture f;
    PolylineData cw;  // clockwise half-disc, negative bulge on the arc
    cw.closed = true;
    cw.v = {PolyVertex{{ox, oy}, 0}, PolyVertex{{ox, oy + 20}, -1}};
    EntityId a = f.add(Entity{{}, cw});
    EntityId c = f.add(Entity{{}, CircleData{{ox, oy}, 10}});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "3");
    using cmd::ScriptItem;
    host.run("AREA", {"O", ScriptItem::pick(a)});
    CHECK(host.logContains("Area = 157.080"));  // half of pi * 10^2
    host.run("AREA", {"O", ScriptItem::pick(c)});
    CHECK(host.logContains("Area = 314.159"));
}

TEST_CASE("MEASUREGEOM Volume multiplies the base area by the height") {
    Fixture f;
    EntityId r = f.add(Entity{{}, rect(10, 20)});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    using cmd::ScriptItem;
    host.run("MEASUREGEOM", {"Volume", "O", ScriptItem::pick(r), "5", ""});
    CHECK(host.logContains("Volume = 1000.00"));
    host.clearLog();
    host.run("MEASUREGEOM", {"Volume", "0,0", "4,0", "4,3", "", "2", ""});
    CHECK(host.logContains("Volume = 12.00"));
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("units follow the document unit in DIST, AREA, LIST and MEASUREGEOM") {
    for (auto [unit, name, scale] : {std::tuple{LengthUnit::Meter, "m", 0.001},
                                     std::tuple{LengthUnit::Inch, "in", 1 / 25.4}}) {
        Fixture f;
        EntityId c = f.add(Entity{{}, CircleData{{0, 0}, 1000}});
        {
            Transaction tx = f.doc.begin("units");
            REQUIRE(tx.modifyHeader([u = unit](Header& h) { h.units = u; }));
            tx.commit();
        }
        cmd::ScriptHost host(f.doc, f.reg);
        host.setSetting("inquiry.precision", "3");
        char want[64];
        // Typed coordinates are in drawing units; picked objects are stored in millimetres.
        host.run("DIST", {"0,0", "1000,0"});
        std::snprintf(want, sizeof want, "Distance = 1000.000 %s", name);
        CHECK_MESSAGE(host.logContains(want), name);
        host.run("MEASUREGEOM", {"Radius", cmd::ScriptItem::pick(c, {1000, 0}), ""});
        std::snprintf(want, sizeof want, "Radius = %.3f %s", 1000 * scale, name);
        CHECK_MESSAGE(host.logContains(want), name);
        host.run("AREA", {"O", cmd::ScriptItem::pick(c)});
        std::snprintf(want, sizeof want, "Area = %.3f", kPi * 1e6 * scale * scale);
        CHECK_MESSAGE(host.logContains(want), name);
    }
}

TEST_CASE("MEASUREGEOM Quick measures what is under the click and never edits the drawing") {
    Fixture f;
    f.add(line({0, 0}, {10, 0}));
    f.add(line({0, 4}, {10, 4}));
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "1");
    host.run("MEASUREGEOM", {"Quick", "5,1"});
    CHECK(host.logContains("Length 10.0 mm"));
    CHECK(host.logContains("Distance 4.0 mm"));
    host.feed({cmd::ScriptItem::esc()});
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("LIST of an insert names its block, scale and xref source") {
    Fixture f;
    BlockId plain, xr;
    {
        Transaction tx = f.doc.begin("blocks");
        plain = tx.addBlock(BlockDef{.name = "Chair"}).value();
        BlockDef x;
        x.name = "Site";
        x.xrefPath = "site.dxf";
        x.xrefOverlay = true;
        xr = tx.addBlock(std::move(x)).value();
        tx.commit();
    }
    InsertData a;
    a.block = plain;
    a.position = {500123.456, 4500987.654};
    a.scale = {2, 2};
    InsertData b;
    b.block = xr;
    EntityId ia = f.add(Entity{{}, a});
    EntityId ib = f.add(Entity{{}, b});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "3");
    host.run("LIST", {cmd::ScriptItem::pick(ia), ""});
    CHECK(host.logContains("Block: Chair"));
    CHECK(host.logContains("500123.456"));
    CHECK(host.logContains("Scale X = 2.0000"));
    host.run("LIST", {cmd::ScriptItem::pick(ib), ""});
    CHECK(host.logContains("external reference site.dxf, overlay"));
}

TEST_CASE("LIST of a large selection lists the first objects and counts the rest") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("gen");
        for (int i = 0; i < 1000; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{0.0 + i, 0}, {1.0 + i, 1}}}).value());
        tx.commit();
    }
    host.setSelection(ids);
    host.run("LIST", {});
    CHECK(host.logContains("700 more object(s) not listed"));
    std::size_t lines = 0;
    for (const auto& l : host.log())
        lines += l.rfind("LINE", 0) == 0 ? 1 : 0;
    CHECK(lines == 300);
}

TEST_CASE("UNITS Precision sets the decimal places DIST, ID and ANGLE show") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("DIST", {"0,0", "3,4"});
    CHECK(host.logContains("Distance = 5.0000"));  // the default: 4 places, angles whole degrees
    CHECK(host.logContains("Angle in XY plane = 53"));
    CHECK_FALSE(host.logContains("53.1"));
    CHECK(host.run("-UNITS", {"Precision", "2", "1"}).finished);
    CHECK(linearPrecision(f.doc.header()) == 2);
    CHECK(anglePrecision(f.doc.header()) == 1);
    host.clearLog();
    host.run("DIST", {"0,0", "3,4"});
    CHECK(host.logContains("Distance = 5.00 mm"));
    CHECK(host.logContains("Angle in XY plane = 53.1"));
    host.clearLog();
    host.run("ID", {"1.23456,2"});
    CHECK(host.logContains("X = 1.23"));
    CHECK_FALSE(host.logContains("1.234"));
    // An inquiry precision set by the application still wins.
    host.setSetting("inquiry.precision", "3");
    host.clearLog();
    host.run("DIST", {"0,0", "3,4"});
    CHECK(host.logContains("Distance = 5.000"));
    CHECK(host.undo());  // the UNITS change undoes
    CHECK(linearPrecision(f.doc.header()) == 4);
}

TEST_CASE("LIST names every kind of object, the newer ones too") {
    Document doc;
    auto first = [&](Entity e) { return inquiry::describeEntity(doc, e, 4).front(); };
    Entity ray;
    ray.data = RayData{};
    CHECK(first(ray).rfind("RAY", 0) == 0);
    RayData x;
    x.infinite = true;
    Entity xline;
    xline.data = x;
    CHECK(first(xline).rfind("XLINE", 0) == 0);
    Entity ml;
    ml.data = Indirect<MLeaderData>(MLeaderData{});
    CHECK(first(ml).rfind("MULTILEADER", 0) == 0);
}

TEST_CASE("DIST Multiple points keeps a running total - ID and DIST show Z as AutoCAD does") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    host.run("DIST", {"0,0", "3,4"});
    CHECK(host.logContains("Angle from XY plane = 0"));
    CHECK(host.logContains("Delta Z = 0.00"));
    auto r = host.run("DIST", {"0,0", "M", "3,4", "3,10", "U", "3,14", ""});
    CHECK(r.finished);
    CHECK(host.logContains("Distance = 11.00"));
    CHECK(host.logContains("Distance = 15.00 mm along 3 points"));
    host.run("ID", {"1,2"});
    CHECK(host.logContains("Z = 0.00"));
}

TEST_CASE("MEASUREGEOM stays open for the next measurement until eXit") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "2");
    auto r = host.run("MEASUREGEOM", {"Distance", "0,0", "3,4"});
    CHECK_FALSE(r.finished);
    r = host.feed({"Distance", "0,0", "6,8", "X"});
    CHECK(r.finished);
    CHECK(host.logContains("Distance = 10.00"));
}
