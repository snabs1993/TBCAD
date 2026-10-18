#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {
std::vector<const Entity*> ptrs(const std::vector<Entity>& v) {
    std::vector<const Entity*> out;
    for (const auto& e : v)
        out.push_back(&e);
    return out;
}

// Distance from p to the infinite line through a and b.
double toLine(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 d = b - a;
    return std::abs(cross(d, p - a)) / std::sqrt(dot(d, d));
}

constexpr Vec2 kOrigins[] = {{0, 0}, {250000, 5000000}, {-8e6, 3e6}};
}  // namespace

TEST_CASE("accuracy: extended line ends lie on the boundary") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> r(-20, 20), ang(0, kTwoPi);
    for (Vec2 o : kOrigins) {
        for (int i = 0; i < 400; ++i) {
            const Vec2 a = o + Vec2{r(rng), r(rng)};
            const double th = ang(rng);
            const Vec2 b = a + Vec2{std::cos(th), std::sin(th)} * (1 + std::abs(r(rng)));
            // A boundary line ahead of b, not parallel to the line.
            const Vec2 c = b + Vec2{std::cos(th), std::sin(th)} * (3 + std::abs(r(rng)));
            const double ph = th + 0.3 + (ang(rng) / kTwoPi) * 2.5;
            const Vec2 d = c + Vec2{std::cos(ph), std::sin(ph)} * 7;
            const Vec2 c2 = c - Vec2{std::cos(ph), std::sin(ph)} * 7;
            Entity l = line(a, b);
            std::vector<Entity> edges{line(c2, d)};
            auto res = extendEntity(l, b, ptrs(edges));
            REQUIRE(res.ok());
            const auto& out = as<LineData>(*res.value().modified);
            CHECK(near(out.a, a, 1e-9));
            CHECK(toLine(out.b, c2, d) < 1e-7);
            CHECK(toLine(out.b, a, b) < 1e-7);
        }
    }
}

TEST_CASE("accuracy: extended arc ends lie on the circle and the boundary") {
    std::mt19937 rng(12);
    std::uniform_real_distribution<double> r(2, 20), ang(0, kTwoPi);
    for (Vec2 o : kOrigins) {
        for (int i = 0; i < 300; ++i) {
            const double rad = r(rng);
            const double a0 = ang(rng);
            const double sweep = 0.3 + ang(rng) * 0.2;
            Entity a = arc(o, rad, a0, a0 + sweep);
            // A line that crosses the circle beyond the arc end.
            const double target = a0 + sweep + 0.2 + ang(rng) * 0.1;
            const Vec2 hit = o + Vec2{std::cos(target), std::sin(target)} * rad;
            const Vec2 dir = Vec2{-std::sin(target), std::cos(target)} * 0.5 +
                             Vec2{std::cos(target), std::sin(target)} * 0.4;
            std::vector<Entity> edges{line(hit - dir * 10, hit + dir * 10)};
            const Vec2 pe = o + Vec2{std::cos(a0 + sweep), std::sin(a0 + sweep)} * rad;
            auto res = extendEntity(a, pe, ptrs(edges));
            REQUIRE(res.ok());
            const auto& out = as<ArcData>(*res.value().modified);
            const Vec2 np = o + Vec2{std::cos(out.endAngle), std::sin(out.endAngle)} * rad;
            CHECK(toLine(np, hit - dir * 10, hit + dir * 10) < 1e-7);
            CHECK(near(out.startAngle, normalizeAngle(a0), 1e-9));
        }
    }
}

TEST_CASE("accuracy: trimmed pieces end on the cutting edges") {
    std::mt19937 rng(13);
    std::uniform_real_distribution<double> r(-20, 20), ang(0, kTwoPi);
    for (Vec2 o : kOrigins) {
        for (int i = 0; i < 300; ++i) {
            const Vec2 a = o + Vec2{r(rng), r(rng)};
            const double th = ang(rng);
            const Vec2 u{std::cos(th), std::sin(th)};
            const double len = 30;
            const Vec2 b = a + u * len;
            const double t1 = 8 + 3 * (ang(rng) / kTwoPi), t2 = 18 + 3 * (ang(rng) / kTwoPi);
            const double ph = th + 0.5 + (ang(rng) / kTwoPi) * 2.0;
            const Vec2 w{std::cos(ph), std::sin(ph)};
            Entity l = line(a, b);
            const Vec2 e1a = a + u * t1 - w * 6, e1b = a + u * t1 + w * 6;
            const Vec2 e2a = a + u * t2 - w * 6, e2b = a + u * t2 + w * 6;
            std::vector<Entity> edges{line(e1a, e1b), line(e2a, e2b)};
            auto res = trimEntity(l, a + u * ((t1 + t2) / 2), ptrs(edges));
            REQUIRE(res.ok());
            REQUIRE(res.value().modified.has_value());
            REQUIRE(res.value().added.size() == 1);
            const auto& p = as<LineData>(*res.value().modified);
            const auto& q = as<LineData>(res.value().added[0]);
            CHECK(near(p.a, a, 1e-9));
            CHECK(toLine(p.b, e1a, e1b) < 1e-7);
            CHECK(near(q.b, b, 1e-9));
            CHECK(toLine(q.a, e2a, e2b) < 1e-7);
        }
    }
}

TEST_CASE("accuracy: offset keeps its distance far from the origin") {
    for (Vec2 o : kOrigins) {
        Entity a = arc(o, 12, 0.4, 2.2);
        auto res = offsetToward(a, o + Vec2{20, 0}, 3.5);
        REQUIRE(res.ok());
        REQUIRE(res.value().size() == 1);
        const auto& out = as<ArcData>(res.value()[0]);
        CHECK(near(out.radius, 15.5, 1e-7));
        CHECK(near(out.center, o, 1e-9));
        Entity l = line(o + Vec2{-4, 1}, o + Vec2{9, 7});
        auto lr = offsetToward(l, o + Vec2{0, 40}, 2.0);
        REQUIRE(lr.ok());
        const auto& ld = as<LineData>(lr.value()[0]);
        CHECK(near(toLine(ld.a, o + Vec2{-4, 1}, o + Vec2{9, 7}), 2.0, 1e-7));
        CHECK(near(toLine(ld.b, o + Vec2{-4, 1}, o + Vec2{9, 7}), 2.0, 1e-7));
    }
}
