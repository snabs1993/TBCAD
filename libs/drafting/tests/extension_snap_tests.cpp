// EXTension object snap: rest on an end, then follow the extension path.
#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

void extOn(World& w, const std::string& modes = "extension") {
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, modes);
    w.set(keys::osnapAperture, "2");
}

}  // namespace

TEST_CASE("extension: a line's end, then along its extension") {
    World w;
    extOn(w);
    w.line({0, 0}, {10, 0});
    // nothing acquired yet: no snap off the line
    CHECK(near(w.hover({30, 1}), {30, 1}));
    w.hover({10.5, 0.5});  // rest on the end
    cmd::FilterFeedback fb;
    CHECK(near(w.hover({30, 1}, std::nullopt, 1, &fb), {30, 0}, 1e-9));
    CHECK(fb.marker == "extension");
    CHECK(fb.tooltip.find("Extension: 20") == 0);
    REQUIRE(fb.guide);
    CHECK(near(fb.guide->first, {10, 0}));
    // off the path: free
    CHECK(near(w.hover({30, 5}), {30, 5}));
    // behind the end (over the line itself): not an extension
    CHECK(near(w.hover({5, 1}), {5, 1}));
    // the other end was not acquired
    CHECK(near(w.hover({-20, 1}), {-20, 1}));
    // a click forgets the acquired end
    w.hover({30, 1}, std::nullopt, 1, nullptr, false, true);
    CHECK(near(w.hover({40, 1}), {40, 1}));
}

TEST_CASE("extension: two acquired ends snap to where their extensions cross") {
    World w;
    extOn(w);
    w.line({0, 0}, {10, 0});
    w.line({20, -20}, {20, -10});
    w.hover({10, 0.5});
    w.hover({20.5, -10});
    cmd::FilterFeedback fb;
    CHECK(near(w.hover({20.7, 0.8}, std::nullopt, 1, &fb), {20, 0}, 1e-9));
    CHECK(fb.tooltip == "Extension intersection");
}

TEST_CASE("extension: an arc continues on its circle, a polyline end segment like a line") {
    World w;
    extOn(w);
    w.add(Entity{{}, ArcData{{0, 0}, 10, 0, kPi / 2}});  // from (10,0) to (0,10)
    w.hover({0.3, 10.2});                                // rest on the end at (0,10)
    const Vec2 onCircle{-10 * std::cos(kPi / 4), 10 * std::sin(kPi / 4)};
    cmd::FilterFeedback fb;
    CHECK(near(w.hover(onCircle * 1.05, std::nullopt, 1, &fb), onCircle, 1e-9));
    CHECK(fb.marker == "extension");
    // within the arc's own sweep it is not an extension
    const Vec2 inside{10 * std::cos(kPi / 4), 10 * std::sin(kPi / 4)};
    CHECK(near(w.hover(inside * 1.05), inside * 1.05));

    World p;
    extOn(p);
    PolylineData pl;
    pl.v = {{{0, 0}}, {{10, 0}}, {{10, 10}}};
    p.add(Entity{{}, pl});
    p.hover({10.3, 10.5});
    CHECK(near(p.hover({10.5, 25}), {10, 25}, 1e-9));
}

TEST_CASE("extension: a real endpoint beats an extension, and the end is still acquired") {
    World w;
    extOn(w, "endpoint,extension");
    w.line({0, 0}, {10, 0});
    w.line({30, -5}, {30, 5});
    cmd::FilterFeedback fb;
    CHECK(near(w.hover({10.5, 0.5}, std::nullopt, 1, &fb), {10, 0}));
    CHECK(fb.marker == "endpoint");
    CHECK(near(w.hover({25, 1}), {25, 0}, 1e-9));
}
