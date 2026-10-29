#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

TEST_CASE("DONUT draws rings at each picked centre") {
    Fixture f;
    CHECK(f.host.run("DO", {"5", "10", "0,0", "20,0", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    for (int i = 0; i < 2; ++i) {
        const auto& pl = as<PolylineData>(es[static_cast<std::size_t>(i)]);
        const double cx = i == 0 ? 0 : 20;
        CHECK(pl.closed);
        REQUIRE(pl.v.size() == 2);
        CHECK(close(pl.v[0].p, {cx - 3.75, 0}));
        CHECK(close(pl.v[1].p, {cx + 3.75, 0}));
        CHECK(close(pl.v[0].bulge, 1));
        CHECK(close(pl.v[1].bulge, 1));
        CHECK(close(pl.constantWidth, 2.5));
        CHECK(close(pl.v[0].startWidth, 2.5));
        CHECK(close(pl.v[1].endWidth, 2.5));
    }
}

TEST_CASE("DONUT with a zero inside diameter is a solid disc") {
    Fixture f;
    f.host.run("DONUT", {"0", "10", "1,1", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(close(pl.constantWidth, 5));
    CHECK(close(pl.v[0].p, {1 - 2.5, 1}));
    CHECK(close(pl.v[1].p, {1 + 2.5, 1}));
}

TEST_CASE("DONUT refuses an outside diameter that is not larger") {
    Fixture f;
    f.host.run("DO", {"10", "5"});
    CHECK(f.host.logContains("must be larger"));
    f.host.feed({"20", "0,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).constantWidth, 5));
}

TEST_CASE("DONUT remembers its diameters") {
    Fixture f;
    f.host.run("DO", {"10", "20", ""});
    f.host.run("DO", {"", "", "0,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).constantWidth, 5));
}

TEST_CASE("DONUT: Esc keeps the rings placed - undo removes them in one step") {
    Fixture f;
    f.host.run("DO", {"5", "10", "0,0", "10,0", ScriptItem::esc()});
    CHECK(f.entities().size() == 2);
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("DONUT shows a preview ring at the cursor") {
    Fixture f;
    f.host.run("DO", {"5", "10"});
    cmd::RawInput in;
    in.worldCursor = {3, 3};
    f.host.runner().pointerMoved(in);
    CHECK_FALSE(f.host.preview().items.empty());
    f.host.feed({ScriptItem::esc()});
}

TEST_CASE("DONUT: a diameter given as two points is the distance between them") {
    Fixture f;
    f.host.run("DO", {"0,0", "6,8", "10,10"});
    CHECK(f.host.logContains("second point"));
    f.host.feed({"@30<0", "0,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<PolylineData>(es[0]).constantWidth, 10));
}
