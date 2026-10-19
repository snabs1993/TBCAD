#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

namespace {
const double kBulge = std::tan(kPi / 6);
}

TEST_CASE("REVCLOUD from points divides each edge into arcs no longer than the arc length") {
    Fixture f;
    CHECK(f.host.run("REVC", {"Arclength", "25", "0,0", "100,0", "100,50", "0,50", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    REQUIRE(pl.v.size() == 12);
    CHECK(close(pl.v[0].p, {0, 0}));
    CHECK(close(pl.v[1].p, {25, 0}));
    CHECK(close(pl.v[4].p, {100, 0}));
    CHECK(close(pl.v[6].p, {100, 50}));
    CHECK(close(pl.v[7].p, {75, 50}));
    CHECK(close(pl.v[10].p, {0, 50}));
    CHECK(close(pl.v[11].p, {0, 25}));
    for (const auto& v : pl.v)
        CHECK(close(v.bulge, kBulge));  // counter-clockwise input: positive bulge is outwards
}

TEST_CASE("REVCLOUD bulges outwards for clockwise input too") {
    Fixture f;
    f.host.run("REVCLOUD", {"A", "50", "0,0", "0,50", "100,50", "100,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    for (const auto& v : as<PolylineData>(es[0]).v)
        CHECK(close(v.bulge, -kBulge));
}

TEST_CASE("REVCLOUD remembers the arc length") {
    Fixture f;
    f.host.run("REVC", {"A", "50", ScriptItem::esc()});
    f.host.run("REVC", {"0,0", "100,0", "100,50", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    // Edges 100, 50 and 111.8 long at 50: 2 + 1 + 3 pieces.
    CHECK(as<PolylineData>(es[0]).v.size() == 6);
}

TEST_CASE("REVCLOUD from a rectangle object") {
    Fixture f;
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}}) {
        PolyVertex v;
        v.p = p;
        box.v.push_back(v);
    }
    auto rect = f.addEntity(box);
    f.host.run("REVC", {"A", "25", "Object", ScriptItem::pick(rect, {0, 0}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);  // the rectangle became the cloud
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 12);
    CHECK(close(pl.v[1].p, {25, 0}));
    CHECK(close(pl.v[0].bulge, kBulge));
}

TEST_CASE("REVCLOUD Reverse turns the arcs inwards") {
    Fixture f;
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}}) {
        PolyVertex v;
        v.p = p;
        box.v.push_back(v);
    }
    auto rect = f.addEntity(box);
    f.host.run("REVC", {"A", "50", "O", ScriptItem::pick(rect, {0, 0}), "Yes"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).v[0].bulge, -kBulge));
}

TEST_CASE("REVCLOUD from a circle spaces the vertices evenly on it") {
    Fixture f;
    auto circle = f.addEntity(CircleData{{5, 5}, 10});
    f.host.run("REVC", {"A", "5", "O", ScriptItem::pick(circle, {15, 5}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 13);  // ceil(62.83 / 5)
    for (const auto& v : pl.v)
        CHECK(close(distance(v.p, {5, 5}), 10, 0.05));
}

TEST_CASE("REVCLOUD object mode refuses open or unsuitable objects") {
    Fixture f;
    auto line = f.addEntity(LineData{{0, 0}, {10, 0}});
    f.host.run("REVC", {"O"});
    auto r = f.host.feed({ScriptItem::pick(line, {5, 0})});
    CHECK(r.rejected == 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("REVCLOUD with fewer than three points or no area draws nothing") {
    Fixture f;
    f.host.run("REVC", {"0,0", "10,0", ""});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("at least three points"));
    f.host.run("REVC", {"0,0", "10,0", "20,0", ""});
    CHECK(f.entities().empty());
}

TEST_CASE("REVCLOUD: Esc draws nothing - Undo drops the last point - one undo step") {
    Fixture f;
    f.host.run("REVC", {"0,0", "100,0", "100,50", ScriptItem::esc()});
    CHECK(f.entities().empty());
    f.host.run("REVC", {"0,0", "100,0", "100,50", "5,5", "Undo", ""});
    REQUIRE(f.entities().size() == 1);
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE(
    "REVCLOUD Rectangular draws a cloud around two corners and Style Calligraphy tapers the arcs") {
    Fixture f;
    CHECK(f.host.run("REVCLOUD", {"Rectangular", "0,0", "40,20"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    CHECK(pl.v.size() >= 12);
    CHECK(close(pl.v[0].startWidth, 0));

    CHECK(f.host.run("REVCLOUD", {"Style", "Calligraphy", "Rectangular", "0,0", "40,20"}).finished);
    es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& cal = as<PolylineData>(es[1]);
    CHECK(cal.v[0].startWidth > cal.v[0].endWidth);
    CHECK(cal.v[0].endWidth > 0);
}

TEST_CASE("REVCLOUD Object converts the object: the cloud takes its place and its properties") {
    Fixture f;
    Transaction tx = f.doc.begin("layer");
    const LayerId red = tx.addLayer(Layer{.name = "CLOUDS", .color = Color::index(1)}).value();
    const LayerId locked = tx.addLayer(Layer{.name = "LOCKED", .locked = true}).value();
    tx.commit();
    const EntityId circle = f.addEntity(CircleData{{0, 0}, 10});
    const EntityId other = f.addEntity(CircleData{{100, 0}, 10});
    Transaction tx2 = f.doc.begin("props");
    REQUIRE(tx2.modify(circle, [&](Entity& e) { e.common.layer = red; }));
    REQUIRE(tx2.modify(other, [&](Entity& e) { e.common.layer = locked; }));
    tx2.commit();

    f.host.run("REVC", {"A", "5", "O", ScriptItem::pick(circle, {10, 0}), ""});
    CHECK(f.doc.find(circle) == nullptr);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(es[1]->common.layer == red);
    CHECK(as<PolylineData>(es[1]).closed);
    f.host.undo();  // one step brings the circle back
    CHECK(f.doc.find(circle) != nullptr);
    CHECK(f.entities().size() == 2);

    // An object on a locked layer cannot be picked.
    f.host.run("REVC", {"O"});
    CHECK(f.host.feed({ScriptItem::pick(other, {110, 0})}).rejected == 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.doc.find(other) != nullptr);
}

TEST_CASE("REVCLOUD refuses an arc length that would need too many arcs and skips repeated points") {
    Fixture f;
    f.host.run("REVC", {"A", "0.001", "Rectangular", "0,0", "100000,100000"});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("too small"));

    // A rectangle drawn with its first corner given twice, and closed on the first point.
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 50}, Vec2{0, 50}, Vec2{0, 0}}) {
        PolyVertex v;
        v.p = p;
        box.v.push_back(v);
    }
    const EntityId rect = f.addEntity(box);
    f.host.run("REVC", {"A", "25", "O", ScriptItem::pick(rect, {50, 0}), ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.v.size() == 12);
    for (std::size_t i = 0; i < pl.v.size(); ++i)
        CHECK(distance(pl.v[i].p, pl.v[(i + 1) % pl.v.size()].p) > 1);
}

TEST_CASE("REVCLOUD: Enter at the first prompt selects an object, as in AutoCAD") {
    Fixture f;
    CircleData c;
    c.center = {0, 0};
    c.radius = 20;
    auto circle = f.addEntity(c);
    REQUIRE(f.host.run("REVC", {"", ScriptItem::pick(circle, {20, 0}), ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(as<PolylineData>(es[0]).closed);
}

TEST_CASE("REVCLOUD Rectangular, Polygonal and Freehand set the mode, which is remembered") {
    Fixture f;
    REQUIRE(f.host.run("REVC", {"Rectangular", "0,0", "40,20"}).finished);
    REQUIRE(f.host.run("REVC", {"100,0", "140,20"}).finished);  // still rectangular
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    BBox2 b;
    for (const auto& v : as<PolylineData>(es[1]).v)
        b.expand(v.p);
    CHECK(close(b.min, {100, 0}));
    CHECK(close(b.max, {140, 20}));
    REQUIRE(f.host.run("REVC", {"Polygonal", "0,100", "30,100", "30,130", ""}).finished);
    CHECK(f.entities().size() == 3);
    const auto& ps = f.host.prompts();
    CHECK(std::any_of(ps.begin(), ps.end(), [](const std::string& p) {
        return p.find("(arc length 10, polygonal)") != std::string::npos;
    }));
}

TEST_CASE("REVCLOUD Rectangular previews the cloud to the cursor") {
    Fixture f;
    f.host.run("REVC", {"Rectangular", "0,0"});
    cmd::RawInput in;
    in.worldCursor = {40, 20};
    f.host.runner().pointerMoved(in);
    REQUIRE(f.host.preview().items.size() >= 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().empty());
}

TEST_CASE("REVCLOUD Freehand: the cursor lays arcs, and coming back to the start closes it") {
    Fixture f;
    f.host.run("REVC", {"A", "10", "Freehand", "0,0"});
    for (Vec2 p : {Vec2{30, 0}, Vec2{30, 30}, Vec2{0, 30}}) {
        cmd::RawInput in;
        in.worldCursor = p;
        f.host.runner().pointerMoved(in);
    }
    REQUIRE(f.host.preview().items.size() >= 1);
    REQUIRE(f.host.feed({"0,2"}).finished);  // near the start: closed
    CHECK(f.host.logContains("Revision cloud finished"));
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    CHECK(pl.v.size() == 12);  // every 10 along the four 30-long sides
    for (const auto& v : pl.v)
        CHECK(close(v.bulge, kBulge));
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("REVCLOUD Freehand: Enter ends an open cloud and Reverse turns its arcs") {
    Fixture f;
    REQUIRE(f.host.run("REVC", {"A", "10", "F", "0,0", "40,0", "", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK_FALSE(pl.closed);
    REQUIRE(pl.v.size() == 5);  // 0, 10, 20, 30, 40
    CHECK(close(pl.v.back().p, {40, 0}));
    const double side = pl.v[0].bulge;
    CHECK(std::abs(side) == doctest::Approx(kBulge));

    REQUIRE(f.host.run("REVC", {"0,10", "40,10", "", "Yes"}).finished);  // freehand remembered
    es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(close(as<PolylineData>(es[1]).v[0].bulge, -side));

    // nothing but the start point draws nothing
    REQUIRE(f.host.run("REVC", {"5,5", ""}).finished);
    CHECK(f.entities().size() == 2);
}

namespace {
EntityId square(Fixture& f) {
    PolylineData box;
    box.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 100}, Vec2{0, 100}})
        box.v.push_back(PolyVertex{.p = p});
    return f.addEntity(box);
}
BBox2 boundsOf(const PolylineData& pl) {
    BBox2 b;
    for (const auto& v : pl.v)
        b.expand(v.p);
    return b;
}
}  // namespace

TEST_CASE("REVCLOUD Modify replaces the erased side with a new cloud part") {
    Fixture f;
    const EntityId id = square(f);
    REQUIRE(f.host.run("REVC", {"A", "10", "Modify", ScriptItem::pick(id, {50, 0}), "50,-30",
                                "80,-30", "80,0", "65,1", ""})
                .finished);
    const auto& pl = as<PolylineData>(f.doc.find(id));
    CHECK(pl.closed);
    // kept: 80,0 100,0 100,100 0,100 0,0; new: 50,0 then every 10 along 50,-30 80,-30 80,0
    REQUIRE(pl.v.size() == 14);
    CHECK(close(pl.v[0].p, {80, 0}));
    CHECK(close(pl.v[4].p, {0, 0}));
    CHECK(pl.v[4].bulge == 0);
    CHECK(close(pl.v[5].p, {50, 0}));
    CHECK(close(pl.v[8].p, {50, -30}));
    for (std::size_t i = 5; i < pl.v.size(); ++i)
        CHECK(close(pl.v[i].bulge, kBulge));  // counter-clockwise result: outwards
    const BBox2 b = boundsOf(pl);
    CHECK(close(b.min, {0, -30}));
    CHECK(close(b.max, {100, 100}));
    CHECK(f.host.undoStack().undoCount() == 1);  // one step
    f.host.undo();
    CHECK(as<PolylineData>(f.doc.find(id)).v.size() == 4);
}

TEST_CASE("REVCLOUD Modify erasing the far side keeps only the near part, and Reverse turns in") {
    Fixture f;
    const EntityId id = square(f);
    REQUIRE(f.host.run("REVC", {"A", "10", "M", ScriptItem::pick(id, {50, 0}), "50,-30",
                                "80,-30", "80,0", "50,100", "Yes"})
                .finished);
    const auto& pl = as<PolylineData>(f.doc.find(id));
    REQUIRE(pl.v.size() == 10);  // 50,0 kept, then the new part back from 80,0
    CHECK(close(pl.v[0].p, {50, 0}));
    CHECK(close(pl.v[1].p, {80, 0}));
    const BBox2 b = boundsOf(pl);
    CHECK(close(b.min, {50, -30}));
    CHECK(close(b.max, {80, 0}));
    // clockwise result: outwards would be negative; reversed they are positive
    for (std::size_t i = 1; i < pl.v.size(); ++i)
        CHECK(close(pl.v[i].bulge, kBulge));
}

TEST_CASE("REVCLOUD Modify works on a cloud, Enter ends at the nearest place, Esc changes nothing") {
    Fixture f;
    REQUIRE(f.host.run("REVC", {"A", "10", "Rectangular", "0,0", "40,40"}).finished);
    const EntityId id = f.entities()[0]->common.id;
    const std::size_t before = as<PolylineData>(f.doc.find(id)).v.size();
    f.host.run("REVC", {"M", ScriptItem::pick(id, {20, 0}), "20,-20", ScriptItem::esc()});
    CHECK(as<PolylineData>(f.doc.find(id)).v.size() == before);

    // Enter: the part ends at the nearest place on the cloud to 30,-20 (30,0, a vertex)
    REQUIRE(f.host.run("REVC", {"M", ScriptItem::pick(id, {20, 0}), "20,-20", "30,-20", "",
                                "25,1", ""})
                .finished);
    const auto& pl = as<PolylineData>(f.doc.find(id));
    CHECK(pl.closed);
    const BBox2 b = boundsOf(pl);
    CHECK(b.min.y == doctest::Approx(-20));
    CHECK(b.max.y == doctest::Approx(40));
    for (const auto& v : pl.v)  // the kept arcs and the new ones all face out
        CHECK(v.bulge > 0);
}

TEST_CASE("REVCLOUD Modify refuses open polylines and a part that ends where it starts") {
    Fixture f;
    PolylineData open;
    open.v = {PolyVertex{.p = {0, 0}}, PolyVertex{.p = {10, 0}}};
    const EntityId openId = f.addEntity(open);
    f.host.run("REVC", {"M", ScriptItem::pick(openId, {5, 0})});
    CHECK(f.host.runner().active());  // the open polyline was not taken
    f.host.feed({ScriptItem::esc()});
    const EntityId id = square(f);
    f.host.run("REVC", {"M", ScriptItem::pick(id, {50, 0}), "50,0", ""});
    CHECK(as<PolylineData>(f.doc.find(id)).v.size() == 4);
}
