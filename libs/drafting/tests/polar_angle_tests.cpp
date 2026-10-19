// Polar tracking extras: additional angles and angles relative to the last segment.
#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

Vec2 dirDeg(double deg, double r) {
    return {r * std::cos(deg * kPi / 180), r * std::sin(deg * kPi / 180)};
}

Vec2 hoverAfter(World& w, Vec2 cursor, Vec2 base, std::optional<Vec2> lastSegment,
                cmd::FilterFeedback* fb = nullptr) {
    cmd::PointQuery q;
    q.base = base;
    q.lastSegment = lastSegment;
    cmd::FilterFeedback local;
    return w.host.filters().apply(q, cursor, fb ? *fb : local);
}

// THREEPT: three chained points, each based on the one before.
class ThreePoints final : public cmd::Command {
public:
    std::string_view name() const override { return "THREEPT"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        cmd::PointOptions o;
        o.base = a.point;
        auto b = co_await ctx.getPoint("Second point", o);
        if (!b.ok())
            co_return;
        o.base = b.point;
        auto c = co_await ctx.getPoint("Third point", o);
        (void)c;
    }
};

// Records what the runner hands the filters.
class Spy final : public cmd::PointFilter {
public:
    std::optional<Vec2> lastSegment{};
    std::optional<Vec2> filter(const cmd::PointQuery& q, Vec2, cmd::FilterFeedback&) override {
        lastSegment = q.lastSegment;
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("polar additional angles track only themselves, next to the increments") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "90");
    w.set(keys::polarAdditional, "17,127.5");
    const Vec2 base{10, 0};
    CHECK(near(w.hover(base + dirDeg(18, 100), base), base + dirDeg(17, 100), 1e-9));
    CHECK(near(w.hover(base + dirDeg(126, 100), base), base + dirDeg(127.5, 100), 1e-9));
    // 34 is a multiple of 17 but not an additional angle
    CHECK(near(w.hover(base + dirDeg(34, 100), base), base + dirDeg(34, 100)));
    // the increments still work
    CHECK(near(w.hover(base + dirDeg(91, 100), base), base + dirDeg(90, 100), 1e-9));
    // a negative or large angle is the same direction
    w.set(keys::polarAdditional, "-30");
    CHECK(near(w.hover(base + dirDeg(329, 100), base), base + dirDeg(-30, 100), 1e-9));
    w.set(keys::polarAdditional, "");
    CHECK(near(w.hover(base + dirDeg(18, 100), base), base + dirDeg(18, 100)));
    // a bad list is ignored
    w.set(keys::polarAdditional, "abc");
    CHECK(DraftingSettings::read(w.host).polarAdditionalDeg.empty());
    CHECK(parsePolarAngles("15, 400") == std::nullopt);
    CHECK(parsePolarAngles("")->empty());
}

TEST_CASE("relative polar counts angles from the last segment") {
    World w;
    w.set(keys::polar, "1");
    w.set(keys::polarIncrement, "90");
    w.set(keys::polarRelative, "1");
    const Vec2 base{0, 0};
    const Vec2 seg = dirDeg(20, 1);  // the last segment ran at 20 degrees
    cmd::FilterFeedback fb;
    CHECK(near(hoverAfter(w, dirDeg(111, 50), base, seg, &fb), dirDeg(110, 50), 1e-9));
    CHECK(fb.tooltip.find("Relative polar: 50") == 0);
    CHECK(fb.tooltip.find("< 90") != std::string::npos);
    CHECK(near(hoverAfter(w, dirDeg(21, 50), base, seg), dirDeg(20, 50), 1e-9));
    // the absolute axes no longer track
    CHECK(near(hoverAfter(w, dirDeg(1, 50), base, seg), dirDeg(1, 50)));
    // with no last segment the angles are absolute again
    cmd::FilterFeedback fb2;
    CHECK(near(hoverAfter(w, dirDeg(1, 50), base, std::nullopt, &fb2), dirDeg(0, 50), 1e-9));
    CHECK(fb2.tooltip.find("Polar: ") == 0);
    // relative off: absolute
    w.set(keys::polarRelative, "0");
    CHECK(near(hoverAfter(w, dirDeg(1, 50), base, seg), dirDeg(0, 50), 1e-9));
}

TEST_CASE("the runner passes the last segment of the running command to the filters") {
    World w;
    (void)w.reg.add(std::make_unique<ThreePoints>());
    Spy spy;
    w.host.filters().add(&spy, -1000);
    auto move = [&](Vec2 p) {
        cmd::RawInput r;
        r.worldCursor = p;
        w.host.runner().pointerMoved(r);
    };
    w.host.run("THREEPT", {"0,0"});
    move({5, 5});
    CHECK_FALSE(spy.lastSegment);  // the second point has no segment before it
    w.host.feed({"10,0"});
    move({20, 5});
    REQUIRE(spy.lastSegment);
    CHECK(near(*spy.lastSegment, {10, 0}));
    w.host.feed({"20,5"});
    // a new command starts without one
    w.host.run("TWOLINE", {"20,5"});
    move({30, 5});
    CHECK_FALSE(spy.lastSegment);
    w.host.filters().remove(&spy);
}
