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
