// Typed point modifiers at a point prompt: FROM, M2P, TT, snap overrides, and the PARallel snap.
#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

LineData lastLine(const World& w) {
    LineData out{};
    w.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e))
            out = *l;
    });
    return out;
}

}  // namespace

TEST_CASE("FROM: a base point, then an offset from it") {
    World w;
    REQUIRE(w.host.run("TWOLINE", {"0,0", "FROM", "100,50", "@10,5"}).finished);
    const LineData l = lastLine(w);
    CHECK(near(l.b, {110, 55}));
    // the abbreviation and a picked base work too
    w.host.run("TWOLINE", {"0,0", "fro"});
    w.host.feed({cmd::ScriptItem::click({20, 20}), "@0,-5"});
    CHECK(near(lastLine(w).b, {20, 15}));
}

TEST_CASE("M2P and MTP: the middle of two points") {
    World w;
    REQUIRE(w.host.run("TWOLINE", {"0,0", "M2P", "10,0", "30,40"}).finished);
    CHECK(near(lastLine(w).b, {20, 20}));
    REQUIRE(w.host.run("TWOLINE", {"MTP", "0,0", "0,10", "50,5"}).finished);
    CHECK(near(lastLine(w).a, {0, 5}));
}

TEST_CASE("typed snap overrides set the snap for the next point only") {
    World w;
    w.host.run("TWOLINE", {"END"});
    CHECK(w.host.setting(keys::osnapOnce) == std::optional<std::string>("endpoint"));
    w.host.feed({"par"});
    CHECK(w.host.setting(keys::osnapOnce) == std::optional<std::string>("parallel"));
    w.host.feed({"NON"});
    CHECK(w.host.setting(keys::osnapOnce) == std::optional<std::string>("none"));
    // a word that is no snap name is still an invalid point, not an override
    w.host.feed({"endless"});
    CHECK(w.host.setting(keys::osnapOnce) == std::optional<std::string>("none"));
    TypedPointParser p(&w.env);
    CHECK(p.snapOverride("Mid") == std::optional<std::string>("midpoint"));
    CHECK(p.snapOverride("APPINT") == std::optional<std::string>("apparent"));
    CHECK_FALSE(p.snapOverride("e"));
}

TEST_CASE("TT: a temporary tracking point works for the next point with OTRACK off") {
    World w;
    w.host.run("TWOLINE", {"0,0", "TT", "50,50"});
    REQUIRE(w.env.tempTrack.size() == 1);
    // straight below the tracking point the cursor sits on its vertical ray
    const Vec2 p = w.hover({51, 10}, Vec2{0, 0});
    CHECK(near(p, {50, 10}));
    w.host.feed({cmd::ScriptItem::click({51, 10})});
    CHECK(near(lastLine(w).b, {50, 10}));
    CHECK(w.env.tempTrack.empty());  // used up by the point
}

TEST_CASE("PARallel: resting on an edge, then moving near parallel snaps onto the parallel line") {
    World w;
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, "parallel");
    w.line({100, 0}, {200, 100});  // 45 degrees
    // rest on the edge to acquire it
    w.hover({150, 50}, Vec2{0, 0});
    cmd::FilterFeedback fb;
    const Vec2 p = w.hover({30, 32}, Vec2{0, 0}, 1, &fb);
    CHECK(near(p, {31, 31}));
    CHECK(fb.marker == "parallel");
    // far from parallel: no snap
    CHECK(near(w.hover({30, 0}, Vec2{0, 0}), {30, 0}));
    // a click forgets the edge
    w.hover({30, 32}, Vec2{0, 0}, 1, nullptr, false, true);
    CHECK(near(w.hover({30, 32}, Vec2{0, 0}), {30, 32}));
}
