#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;

TEST_CASE("TRACE draws wide segments and remembers the width") {
    Fixture f;
    auto r = f.host.run("TRACE", {"5", "0,0", "100,0", "100,50", ""});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    const auto& p = as<PolylineData>(es[0]);
    REQUIRE(p.v.size() == 2);
    CHECK(close(p.v[1].p, {100, 0}));
    CHECK(p.v[0].startWidth == doctest::Approx(5));
    CHECK(p.v[1].endWidth == doctest::Approx(5));
    CHECK(close(as<PolylineData>(es[1]).v[1].p, {100, 50}));
    // The next TRACE offers the last width as its default.
    Fixture g;
    g.host.run("TRACE", {"2", "0,0", "10,0", ""});
    CHECK(g.host.setting("trace.width").value_or("") == "2.000000");
}
