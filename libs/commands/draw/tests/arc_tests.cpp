#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

namespace {

// The quarter circle of radius 10 around the origin from (10,0) to (0,10).
void checkQuarter(const Fixture& f) {
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& a = as<ArcData>(es[0]);
    CHECK(close(a.center, {0, 0}));
    CHECK(close(a.radius, 10));
    CHECK(closeAngle(a.startAngle, 0));
    CHECK(closeAngle(a.endAngle, kPi / 2));
}

}  // namespace

TEST_CASE("ARC 3P (default)") {
    Fixture f;
    CHECK(f.host.run("A", {"0,0", "5,5", "10,0"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& a = as<ArcData>(es[0]);
    // Through (0,0), (5,5), (10,0): the upper half of the circle at (5,0), stored
    // counter-clockwise.
    CHECK(close(a.center, {5, 0}));
    CHECK(close(a.radius, 5));
    CHECK(closeAngle(a.startAngle, 0));
    CHECK(closeAngle(a.endAngle, kPi));
}

TEST_CASE("ARC 3P counter-clockwise order") {
    Fixture f;
    f.host.run("ARC", {"10,0", "0,10", "-10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& a = as<ArcData>(es[0]);
    CHECK(close(a.center, {0, 0}));
    CHECK(close(a.radius, 10));
    CHECK(closeAngle(a.startAngle, 0));
    CHECK(closeAngle(a.endAngle, kPi));
}

TEST_CASE("ARC 3P with collinear points draws nothing") {
    Fixture f;
    auto r = f.host.run("ARC", {"0,0", "5,0", "10,0"});
    CHECK(r.finished);
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("collinear"));
}

TEST_CASE("ARC Start-Center-End") {
    Fixture f;
    f.host.run("ARC", {"10,0", "Center", "0,0", "0,10"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-Center-End: the end point only gives a direction") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "0,50"});
    checkQuarter(f);
}

TEST_CASE("ARC Center-Start-End") {
    Fixture f;
    f.host.run("ARC", {"Center", "0,0", "10,0", "0,10"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-Center-Angle") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "Angle", "90"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-Center-Angle with a negative angle sweeps clockwise") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "A", "-90"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& a = as<ArcData>(es[0]);
    CHECK(close(a.center, {0, 0}));
    CHECK(close(a.radius, 10));
    CHECK(closeAngle(a.startAngle, -kPi / 2));
    CHECK(closeAngle(a.endAngle, 0));
}

TEST_CASE("ARC Start-Center-Length") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "Length", "14.142135623730951"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-Center-Length longer than the diameter is refused") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "L", "25"});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("longer than the diameter"));
}

TEST_CASE("ARC Start-End-Angle") {
    Fixture f;
    f.host.run("ARC", {"10,0", "End", "0,10", "Angle", "90"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-End-Direction") {
    Fixture f;
    f.host.run("ARC", {"10,0", "E", "0,10", "Direction", "90"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-End-Radius") {
    Fixture f;
    f.host.run("ARC", {"10,0", "E", "0,10", "Radius", "10"});
    checkQuarter(f);
}

TEST_CASE("ARC Start-End-Radius smaller than half the chord is refused") {
    Fixture f;
    f.host.run("ARC", {"10,0", "E", "0,10", "R", "5"});
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("No arc matches"));
}

TEST_CASE("ARC Start-End-Center") {
    Fixture f;
    f.host.run("ARC", {"10,0", "E", "0,10", "0,0"});
    checkQuarter(f);
}

TEST_CASE("ARC Continue starts at the end of the last line, tangent to it") {
    Fixture f;
    f.host.run("LINE", {"0,0", "10,0", ""});
    auto r = f.host.run("ARC", {"", "10,10"});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& a = as<ArcData>(es[1]);
    // Leaves (10,0) heading east and ends at (10,10): a half circle bulging east.
    CHECK(close(a.center, {10, 5}));
    CHECK(close(a.radius, 5));
    CHECK(closeAngle(a.startAngle, 3 * kPi / 2));
    CHECK(closeAngle(a.endAngle, kPi / 2));
}

TEST_CASE("ARC Continue ignores lines on another level") {
    Fixture f;
    f.host.run("LINE", {"0,0", "10,0", ""});
    f.host.run("LINE", {"0,0", "0,10", ""});
    {  // the newer line moves to another level, which is dimmed while the first level is current
        auto tx = f.doc.begin("level");
        auto lv = tx.addLevel(Level{{}, "Upper", 3000});
        REQUIRE(lv.ok());
        EntityId newest = f.entities().back()->common.id;
        REQUIRE(tx.modify(newest, [&](Entity& e) { e.common.level = lv.value(); }));
        tx.commit();
    }
    auto r = f.host.run("ARC", {"", "10,10"});
    CHECK(r.finished);
    bool found = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* a = std::get_if<ArcData>(&e.data)) {
            found = true;
            CHECK(close(a->center, {10, 5}));  // continues from (10,0), not from (0,10)
        }
    });
    CHECK(found);
}

TEST_CASE("ARC Continue after an arc keeps the tangent") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "0,10"});  // ends at (0,10) heading west
    f.host.run("ARC", {"", "-10,0"});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& a = as<ArcData>(es[1]);
    // The next quarter of the same circle.
    CHECK(close(a.center, {0, 0}));
    CHECK(close(a.radius, 10));
    CHECK(closeAngle(a.startAngle, kPi / 2));
    CHECK(closeAngle(a.endAngle, kPi));
}

TEST_CASE("ARC Continue refuses an end point straight ahead of the tangent") {
    Fixture f;
    f.host.run("LINE", {"0,0", "10,0", ""});
    f.host.run("ARC", {"", "20,0"});
    CHECK(f.entities().size() == 1);
    CHECK(f.host.logContains("cannot be reached"));
}

TEST_CASE("ARC Continue with nothing to continue from") {
    Fixture f;
    auto r = f.host.run("ARC", {""});
    CHECK(r.finished);
    CHECK(f.host.logContains("no line or arc"));
    CHECK(f.entities().empty());
}

TEST_CASE("ARC: Esc at any step leaves the drawing untouched") {
    Fixture f;
    f.host.run("ARC", {"0,0", "5,5", ScriptItem::esc()});
    CHECK(f.entities().empty());
    f.host.run("ARC", {"10,0", "C", "0,0", ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("ARC is one undo step and shows a preview while the end point moves") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0"});
    cmd::RawInput in;
    in.worldCursor = {0, 10};
    f.host.runner().pointerMoved(in);
    CHECK_FALSE(f.host.preview().items.empty());
    f.host.feed({"0,10"});
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}
