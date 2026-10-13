#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;

namespace {
void move(Fixture& f, Vec2 p) {
    f.host.runner().pointerMoved(cmd::RawInput{p, {}, 1});
}
}  // namespace

TEST_CASE("SKETCH records points as the pen moves and keeps the stroke on Enter") {
    Fixture f;
    f.host.run("SK", {"0,0"});  // pen down
    move(f, {0.4, 0});          // closer than the increment: not recorded
    move(f, {2, 0});
    move(f, {4, 1});
    move(f, {6, 3});
    f.host.feed({cmd::ScriptItem::click({6, 3})});  // pen up
    f.host.feed({cmd::ScriptItem::enter()});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 4);
    CHECK(close(pl.v[0].p, {0, 0}));
    CHECK(close(pl.v[1].p, {2, 0}));
    CHECK(close(pl.v[3].p, {6, 3}));
    CHECK(f.host.undoStack().undoCount() == 1);
}

TEST_CASE(
    "SKETCH: Increment changes the spacing, Esc and Discard draw nothing, two strokes make two "
    "lines") {
    Fixture f;
    f.host.run("SK", {"Increment", "5", "0,0"});
    move(f, {2, 0});  // below the new increment
    move(f, {6, 0});
    f.host.feed({cmd::ScriptItem::click({6, 0})});
    f.host.feed({cmd::ScriptItem::click({0, 10})});
    move(f, {0, 20});
    f.host.feed({cmd::ScriptItem::click({0, 20})});
    f.host.feed({cmd::ScriptItem::enter()});
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(as<PolylineData>(es[0]).v.size() == 2);

    Fixture g;
    g.host.run("SKETCH", {"0,0"});
    move(g, {5, 5});
    g.host.feed({cmd::ScriptItem::esc()});
    CHECK(g.entities().empty());
    g.host.run("SKETCH", {"0,0", "Discard"});
    CHECK(g.entities().empty());
}
