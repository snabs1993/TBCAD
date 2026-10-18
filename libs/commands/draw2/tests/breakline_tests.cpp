#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;

TEST_CASE("BREAKLINE draws a line with a zigzag at the picked place") {
    Fixture f;
    CHECK(f.host.run("BREAKLINE", {"0,0", "100,0", "50,0"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 6);
    CHECK(close(pl.v[0].p, {0, 0}));
    CHECK(close(pl.v[1].p, {45, 0}));
    CHECK(close(pl.v[2].p, {47.5, 5}));
    CHECK(close(pl.v[3].p, {52.5, -5}));
    CHECK(close(pl.v[4].p, {55, 0}));
    CHECK(close(pl.v[5].p, {100, 0}));
}

TEST_CASE("BREAKLINE Size changes the symbol and Enter puts it in the middle") {
    Fixture f;
    CHECK(f.host.run("BRL", {"Size", "20", "0,0", "100,0", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(close(pl.v[1].p, {40, 0}));
    CHECK(close(pl.v[4].p, {60, 0}));
}
