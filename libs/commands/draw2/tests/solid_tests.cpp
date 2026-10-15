#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;

namespace {
const HatchData& hatchOf(const Entity* e) {
    return indirect<HatchData>(e);
}
const std::vector<PolyVertex>& ringOf(const HatchData& h) {
    return std::get<HatchPolylineEdge>(h.loops[0].edges[0]).v;
}
}  // namespace

TEST_CASE("SOLID with four points draws a filled quad in bow-tie order") {
    Fixture f;
    f.host.run("SOLID", {"0,0", "10,0", "0,10", "10,10", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const HatchData& h = hatchOf(es[0]);
    CHECK(h.solid);
    const auto& v = ringOf(h);
    REQUIRE(v.size() == 4);
    CHECK(close(v[2].p, {10, 10}));
    CHECK(close(v[3].p, {0, 10}));
}

TEST_CASE("SOLID with Enter after three points is a triangle - further points continue") {
    Fixture t;
    t.host.run("SO", {"0,0", "10,0", "5,5", ""});
    REQUIRE(t.entities().size() == 1);
    CHECK(ringOf(hatchOf(t.entities()[0])).size() == 3);

    Fixture c;
    c.host.run("SOLID", {"0,0", "10,0", "0,10", "10,10", "0,20", "10,20", ""});
    CHECK(c.entities().size() == 2);
}
