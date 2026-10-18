// Drawings in survey coordinates (UTM metres written in mm: 600 000 000 / 1 500 000 000) must
// edit as exactly as drawings near the origin. Every case runs once near the origin and once
// moved by a huge offset; the results must agree after moving back.
#include <random>

#include "tbcad/geom/polybool.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {

const Vec2 kFar{600000000.0, 1500000000.0};
constexpr double kTol = 1e-3;  // mm; one double step at 1.5e9 is 2.4e-7

Entity moved(const Entity& e, Vec2 by) {
    auto r = geom::transformed(e, Transform2::translate(by));
    REQUIRE(r.ok());
    return std::move(r.value());
}

// A few points along every curve of an entity, enough to tell two results apart.
std::vector<Vec2> samples(const Entity& e, Vec2 shift) {
    std::vector<geom::CurveRef> cs;
    geom::curvesOf(e, cs);
    std::vector<Vec2> out;
    for (const auto& c : cs) {
        const double t0 = c.t0();
        const double t1 = c.isLine() ? 1.0 : t0 + c.sweep();
        for (int i = 0; i <= 4; ++i) {
            const Vec2 p = c.pointAt(t0 + (t1 - t0) * i / 4.0);
            out.push_back({p.x - shift.x, p.y - shift.y});
        }
    }
    return out;
}

bool same(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (distance(a[i], b[i]) > kTol)
            return false;
    return true;
}

// Results of an edit on the near and far copies agree.
void agree(const std::vector<Entity>& near, const std::vector<Entity>& far) {
    REQUIRE(near.size() == far.size());
    for (std::size_t i = 0; i < near.size(); ++i)
        CHECK(same(samples(near[i], {0, 0}), samples(far[i], kFar)));
}

std::vector<const Entity*> ptrs(const std::vector<Entity>& v) {
    std::vector<const Entity*> out;
    for (const auto& e : v)
        out.push_back(&e);
    return out;
}

std::vector<Entity> editResult(const EntityEdit& ed) {
    std::vector<Entity> out;
    if (ed.modified)
        out.push_back(*ed.modified);
    for (const auto& a : ed.added)
        out.push_back(a);
    return out;
}

std::vector<Entity> pairResult(const PairEdit& pe) {
    std::vector<Entity> out;
    if (pe.modifiedA)
        out.push_back(*pe.modifiedA);
    if (pe.modifiedB)
        out.push_back(*pe.modifiedB);
    for (const auto& a : pe.added)
        out.push_back(a);
    return out;
}

}  // namespace

TEST_CASE("far coordinates: trim and extend give the same result as near the origin") {
    const Entity target = line({-40, 3}, {90, 7});
    const std::vector<Entity> edges = {line({10, -50}, {12, 50}), line({60, -50}, {58, 50}),
                                       circle({30, 5}, 8)};
    std::vector<Entity> farEdges;
    for (const auto& e : edges)
        farEdges.push_back(moved(e, kFar));
    const Vec2 pick{35, 5};
    auto n = trimEntity(target, pick, ptrs(edges));
    auto f = trimEntity(moved(target, kFar), pick + kFar, ptrs(farEdges));
    REQUIRE(n.ok());
    REQUIRE(f.ok());
    agree(editResult(n.value()), editResult(f.value()));

    const Entity shortLine = line({0, 0}, {5, 0.5});
    auto en = extendEntity(shortLine, {4, 0.4}, ptrs(edges));
    auto ef = extendEntity(moved(shortLine, kFar), Vec2{4, 0.4} + kFar, ptrs(farEdges));
    REQUIRE(en.ok());
    REQUIRE(ef.ok());
    agree(editResult(en.value()), editResult(ef.value()));
}

TEST_CASE("far coordinates: fillet and chamfer give the same result as near the origin") {
    const Entity a = line({0, 0}, {100, 0}), b = line({100, 10}, {100, 120});
    auto fn = filletEntities(a, {50, 0}, b, {100, 60}, {12.5, true});
    auto ff = filletEntities(moved(a, kFar), Vec2{50, 0} + kFar, moved(b, kFar),
                             Vec2{100, 60} + kFar, {12.5, true});
    REQUIRE(fn.ok());
    REQUIRE(ff.ok());
    agree(pairResult(fn.value()), pairResult(ff.value()));

    const Entity c = circle({40, 20}, 25);
    auto cn = filletEntities(a, {90, 0}, c, {40, 45}, {6, true});
    auto cf = filletEntities(moved(a, kFar), Vec2{90, 0} + kFar, moved(c, kFar),
                             Vec2{40, 45} + kFar, {6, true});
    REQUIRE(cn.ok());
    REQUIRE(cf.ok());
    agree(pairResult(cn.value()), pairResult(cf.value()));

    ChamferOptions co;
    co.distance1 = 7;
    co.distance2 = 11;
    auto hn = chamferEntities(a, {50, 0}, b, {100, 60}, co);
    auto hf = chamferEntities(moved(a, kFar), Vec2{50, 0} + kFar, moved(b, kFar),
                              Vec2{100, 60} + kFar, co);
    REQUIRE(hn.ok());
    REQUIRE(hf.ok());
    agree(pairResult(hn.value()), pairResult(hf.value()));
}

TEST_CASE("far coordinates: offset gives the same result as near the origin") {
    std::vector<PolyVertex> v = pts({{0, 0}, {80, 0}, {80, 50}, {40, 70}, {0, 50}});
    v[1].bulge = 0.3;
    const Entity p = poly(v, true);
    for (double d : {4.0, -6.5}) {
        auto n = offsetEntity(p, d);
        auto f = offsetEntity(moved(p, kFar), d);
        REQUIRE(n.ok());
        REQUIRE(f.ok());
        agree(n.value(), f.value());
    }
    const Entity c = circle({10, 10}, 30);
    auto cn = offsetEntity(c, 5);
    auto cf = offsetEntity(moved(c, kFar), 5);
    REQUIRE(cn.ok());
    REQUIRE(cf.ok());
    agree(cn.value(), cf.value());
}

TEST_CASE("far coordinates: curve intersections stay exact") {
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> u(-100, 100);
    for (int i = 0; i < 200; ++i) {
        const Entity a = (i % 2) ? line({u(rng), u(rng)}, {u(rng), u(rng)})
                                 : circle({u(rng), u(rng)}, 20 + std::abs(u(rng)));
        const Entity b = (i % 3) ? line({u(rng), u(rng)}, {u(rng), u(rng)})
                                 : circle({u(rng), u(rng)}, 20 + std::abs(u(rng)));
        const Entity fa = moved(a, kFar), fb = moved(b, kFar);
        std::vector<geom::CurveRef> ca, cb, cfa, cfb;
        geom::curvesOf(a, ca);
        geom::curvesOf(b, cb);
        geom::curvesOf(fa, cfa);
        geom::curvesOf(fb, cfb);
        std::vector<geom::Intersection> n, f;
        geom::intersect(ca[0], cb[0], n);
        geom::intersect(cfa[0], cfb[0], f);
        INFO("case " << i);
        REQUIRE(n.size() == f.size());
        for (std::size_t k = 0; k < n.size(); ++k)
            CHECK(distance(n[k].p, f[k].p - kFar) < kTol);
    }
}

TEST_CASE("far coordinates: region booleans keep their area") {
    auto rect = [](Vec2 o, double w, double h) {
        return geom::Ring{o, o + Vec2{w, 0}, o + Vec2{w, h}, o + Vec2{0, h}};
    };
    for (Vec2 base : {Vec2{0, 0}, kFar}) {
        geom::Region a{{rect(base, 100, 80)}}, b{{rect(base + Vec2{60, 30}, 100, 80)}};
        INFO("base " << base.x);
        CHECK(geom::area(geom::boolean(a, b, geom::BoolOp::Union)) ==
              doctest::Approx(100 * 80 * 2 - 40 * 50).epsilon(1e-6));
        CHECK(geom::area(geom::boolean(a, b, geom::BoolOp::Intersect)) ==
              doctest::Approx(40 * 50).epsilon(1e-6));
        CHECK(geom::area(geom::boolean(a, b, geom::BoolOp::Subtract)) ==
              doctest::Approx(100 * 80 - 40 * 50).epsilon(1e-6));
    }
}

TEST_CASE("far coordinates: areas are measured from the shape, not from the origin") {
    std::vector<PolyVertex> v = pts({{0, 0}, {100, 0}, {100, 80}, {0, 80}});
    v[1].bulge = 0.4;
    const Entity p = poly(v, true);
    const double want = geom::area(as<PolylineData>(p));
    REQUIRE(want > 8000);
    CHECK(geom::area(as<PolylineData>(moved(p, kFar))) == doctest::Approx(want).epsilon(1e-9));
}
