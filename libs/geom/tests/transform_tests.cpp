#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {

Entity make(EntityData d) {
    Entity e;
    e.data = std::move(d);
    return e;
}

// Samples the original geometry, maps it, and measures the distance to the transformed entity.
double worstDeviation(const Entity& orig, const Transform2& t) {
    auto r = transformed(orig, t);
    REQUIRE(r.ok());
    std::vector<CurveRef> cs;
    REQUIRE(curvesOf(orig, cs));
    double worst = 0;
    for (const CurveRef& c : cs)
        for (int i = 0; i <= 40; ++i) {
            Vec2 p = t.apply(c.pointAt(c.t0() + (c.t1() - c.t0()) * i / 40.0));
            worst = std::max(worst, distance(r.value(), p));
        }
    return worst;
}

Transform2 randomAffine(std::mt19937& rng, bool allowMirror) {
    std::uniform_real_distribution<double> u(0.4, 3.0), a(0, kTwoPi), p(-20, 20);
    Transform2 t = Transform2::scale(u(rng) * (allowMirror && (rng() & 1) ? -1 : 1), u(rng));
    return Transform2::translate({p(rng), p(rng)}) * Transform2::rotate(a(rng)) * t *
           Transform2::rotate(a(rng));
}

}  // namespace

TEST_CASE("arbitrary affine transforms keep every curve kind on its image") {
    std::mt19937 rng(77);
    ArcData arc{{1, 2}, 4, 0.7, 4.0};
    EllipseData ell;
    ell.center = {-3, 1};
    ell.majorAxis = {3, 1};
    ell.ratio = 0.4;
    ell.startParam = 1.0;
    ell.endParam = 5.0;
    EllipseData fullEll = ell;
    fullEll.startParam = 0;
    fullEll.endParam = kTwoPi;
    Entity entities[] = {
        make(LineData{{0, 0}, {5, 3}}),
        make(CircleData{{2, 2}, 3}),
        make(arc),
        make(ell),
        make(fullEll),
        make(interpolateSpline(std::vector<Vec2>{{0, 0}, {2, 3}, {5, 3}, {7, 0}}).value()),
        make(PolylineData{{{{0, 0}, 0}, {{4, 1}, 0}, {{5, 5}, 0}, {{0, 4}, 0}}, true, 0}),
    };
    for (int i = 0; i < 100; ++i) {
        Transform2 t = randomAffine(rng, true);
        for (const Entity& e : entities)
            CHECK(worstDeviation(e, t) < 1e-8);
    }
}

TEST_CASE("uniform transforms (including mirror) of arcs and polylines with bulges") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> a(0, kTwoPi), s(0.5, 2.5), p(-10, 10);
    Entity arc = make(ArcData{{1, 2}, 4, 0.7, 4.0});
    Entity pl =
        make(PolylineData{{{{0, 0}, 0.5}, {{4, 1}, -0.7}, {{5, 5}, 1.2}, {{0, 4}, 0}}, false, 0});
    for (int i = 0; i < 100; ++i) {
        double sc = s(rng);
        Transform2 t = Transform2::translate({p(rng), p(rng)}) * Transform2::rotate(a(rng)) *
                       Transform2::scale(sc, sc);
        if (i % 2) {
            auto m = Transform2::mirror({p(rng), p(rng)}, {p(rng) + 20, p(rng)});
            REQUIRE(m.ok());
            t = m.value() * t;
        }
        CHECK(worstDeviation(arc, t) < 1e-8);
        CHECK(worstDeviation(pl, t) < 1e-8);
        auto ta = transformed(arc, t);
        REQUIRE(ta.ok());
        // Still an arc (not an ellipse) and still counter-clockwise: the swept angle is unchanged.
        const ArcData* out = entityAs<ArcData>(ta.value());
        REQUIRE(out != nullptr);
        CHECK(out->radius == doctest::Approx(4 * sc));
        double sweep = normalizeAngle(out->endAngle - out->startAngle);
        CHECK(sweep == doctest::Approx(4.0 - 0.7).epsilon(1e-9));
        CHECK(out->startAngle >= 0);
        CHECK(out->startAngle < kTwoPi);
    }
}

TEST_CASE("mirror flips bulge signs and polygon winding") {
    PolylineData sq;
    sq.closed = true;
    sq.v = {{{0, 0}, 1.0}, {{4, 0}, 0}, {{4, 4}, 0}, {{0, 4}, 0}};
    Entity e = make(sq);
    auto m = Transform2::mirror({0, 0}, {1, 0}).value();  // across the x axis
    auto r = transformed(e, m);
    REQUIRE(r.ok());
    const auto* out = entityAs<PolylineData>(r.value());
    CHECK(out->v[0].bulge == -1.0);
    CHECK(signedArea(*out) == doctest::Approx(-signedArea(sq)).epsilon(1e-12));
}

TEST_CASE("non-uniform scale turns a circle into the right ellipse") {
    Entity c = make(CircleData{{10, 20}, 2});
    auto r = transformed(c, Transform2::scale(3, 1, {0, 0}));
    REQUIRE(r.ok());
    const auto* e = entityAs<EllipseData>(r.value());
    REQUIRE(e != nullptr);
    CHECK(e->center.x == doctest::Approx(30));
    CHECK(e->center.y == doctest::Approx(20));
    CHECK(length(e->majorAxis) == doctest::Approx(6));
    CHECK(e->ratio == doctest::Approx(1.0 / 3));
    CHECK(std::abs(e->majorAxis.y) < 1e-9);
    CHECK(e->startParam == 0);
    CHECK(e->endParam == doctest::Approx(kTwoPi));
    // Rotated stretch.
    auto r2 = transformed(c, Transform2::rotate(kPi / 6) * Transform2::scale(1, 4));
    REQUIRE(r2.ok());
    const auto* e2 = entityAs<EllipseData>(r2.value());
    CHECK(length(e2->majorAxis) == doctest::Approx(8));
    CHECK(e2->ratio == doctest::Approx(0.25));
}

TEST_CASE("non-uniform scale of an arc gives an elliptical arc between the mapped end points") {
    ArcData a{{0, 0}, 5, kPi / 6, kPi / 2};
    Entity e = make(a);
    Transform2 t = Transform2::scale(2, 0.5);
    auto r = transformed(e, t);
    REQUIRE(r.ok());
    REQUIRE(entityAs<EllipseData>(r.value()) != nullptr);
    CHECK(worstDeviation(e, t) < 1e-9);
    // End points of the elliptical arc are the mapped end points.
    std::vector<CurveRef> cs;
    curvesOf(r.value(), cs);
    CurveRef orig = CurveRef::arc({0, 0}, 5, kPi / 6, kPi / 2 - kPi / 6);
    Vec2 s = t.apply(orig.startPoint()), en = t.apply(orig.endPoint());
    bool ok = (distance(cs[0].startPoint(), s) < 1e-9 && distance(cs[0].endPoint(), en) < 1e-9) ||
              (distance(cs[0].startPoint(), en) < 1e-9 && distance(cs[0].endPoint(), s) < 1e-9);
    CHECK(ok);
    // Mirror + non-uniform.
    auto tm = Transform2::scale(-2, 0.5);
    CHECK(worstDeviation(e, tm) < 1e-9);
}

TEST_CASE("refused transforms") {
    Transform2 nu = Transform2::scale(2, 1);
    CHECK_FALSE(transformed(make(TextData{}), nu).ok());
    CHECK_FALSE(transformed(make(MTextData{}), nu).ok());
    CHECK_FALSE(transformed(make(DimensionData{}), nu).ok());
    CHECK_FALSE(transformed(make(LeaderData{}), nu).ok());
    PolylineData bulged;
    bulged.v = {{{0, 0}, 1.0}, {{2, 0}, 0}};
    CHECK_FALSE(transformed(make(bulged), nu).ok());
    PolylineData straight;
    straight.v = {{{0, 0}, 0}, {{2, 0}, 0}};
    CHECK(transformed(make(straight), nu).ok());
    Transform2 singular;
    singular.m00 = 0;
    singular.m11 = 0;
    CHECK_FALSE(transformed(make(LineData{{0, 0}, {1, 1}}), singular).ok());
    // A rotated insert cannot absorb a skewing transform.
    InsertData in;
    in.rotation = 0.3;
    CHECK_FALSE(transformed(make(in), Transform2::scale(2, 1)).ok());
    CHECK_FALSE(transformed(make(in), Transform2::rotate(0.4) * Transform2::scale(2, 1) *
                                          Transform2::rotate(0.2))
                    .ok());
}

TEST_CASE("text, inserts, dimensions and hatches follow uniform transforms") {
    Transform2 t =
        Transform2::translate({10, 0}) * Transform2::rotate(kPi / 2) * Transform2::scale(2, 2);
    TextData td;
    td.position = {1, 0};
    td.height = 3;
    td.rotation = 0;
    auto tr = transformed(make(td), t);
    REQUIRE(tr.ok());
    const auto* o = entityAs<TextData>(tr.value());
    CHECK(distance(o->position, Vec2{10, 2}) < 1e-12);
    CHECK(o->height == doctest::Approx(6));
    CHECK(o->rotation == doctest::Approx(kPi / 2));
    // A mirror keeps text readable: the position moves, the rotation stays.
    auto m = Transform2::mirror({0, 0}, {0, 1}).value();  // across the y axis
    auto tm = transformed(make(td), m);
    CHECK(distance(entityAs<TextData>(tm.value())->position, Vec2{-1, 0}) < 1e-12);
    CHECK(entityAs<TextData>(tm.value())->rotation == 0);

    InsertData in;
    in.position = {1, 1};
    in.scale = {2, 3};
    in.rotation = 0.5;
    auto ti = transformed(make(in), m);
    REQUIRE(ti.ok());
    const auto* oi = entityAs<InsertData>(ti.value());
    CHECK(std::abs(oi->scale.x) == doctest::Approx(2));
    CHECK(oi->scale.y * oi->scale.x < 0);  // a mirror shows up as a sign flip
    // The block's local point (1, 0) must land where the transform sends it.
    auto place = [](const InsertData& d, Vec2 p) {
        double c = std::cos(d.rotation), s = std::sin(d.rotation);
        Vec2 q{p.x * d.scale.x, p.y * d.scale.y};
        return d.position + Vec2{c * q.x - s * q.y, s * q.x + c * q.y};
    };
    for (Vec2 p : {Vec2{1, 0}, Vec2{0, 1}, Vec2{2, 3}})
        CHECK(distance(place(*oi, p), m.apply(place(in, p))) < 1e-9);
    // Non-uniform scale that stays axis-aligned is representable.
    InsertData flat;
    flat.position = {1, 1};
    auto tf = transformed(make(flat), Transform2::scale(2, 1));
    REQUIRE(tf.ok());
    CHECK(entityAs<InsertData>(tf.value())->scale.x == doctest::Approx(2));
    CHECK(entityAs<InsertData>(tf.value())->scale.y == doctest::Approx(1));

    DimensionData dd;
    dd.kind = DimKind::Linear;
    dd.defPts = {Vec2{0, 0}, Vec2{4, 0}, Vec2{4, 1}, Vec2{0, 0}};
    dd.measurement = 4;
    auto td2 = transformed(make(dd), t);
    REQUIRE(td2.ok());
    CHECK(entityAs<DimensionData>(td2.value())->measurement == doctest::Approx(8));
    CHECK(entityAs<DimensionData>(td2.value())->rotation == doctest::Approx(kPi / 2));

    HatchData h;
    HatchLoop loop;
    loop.edges.push_back(HatchLineEdge{{0, 0}, {4, 0}});
    loop.edges.push_back(HatchArcEdge{{4, 2}, 2, -kPi / 2, kPi / 2, true});
    loop.edges.push_back(HatchLineEdge{{4, 4}, {0, 4}});
    loop.edges.push_back(HatchLineEdge{{0, 4}, {0, 0}});
    h.loops.push_back(loop);
    h.patternScale = 1;
    auto th = transformed(make(h), t);
    REQUIRE(th.ok());
    const auto& oh = *entityAs<HatchData>(th.value());
    CHECK(oh.patternScale == doctest::Approx(2));
    const auto& arcEdge = std::get<HatchArcEdge>(oh.loops[0].edges[1]);
    CHECK(arcEdge.radius == doctest::Approx(4));
    CHECK(distance(arcEdge.center, t.apply({4, 2})) < 1e-9);
    // Non-uniform scale turns the arc edge into an elliptical edge.
    auto tn = transformed(make(h), Transform2::scale(2, 1));
    REQUIRE(tn.ok());
    CHECK(std::holds_alternative<HatchEllipseEdge>(
        entityAs<HatchData>(tn.value())->loops[0].edges[1]));
}

TEST_CASE("proxy graphics follow the transform") {
    ProxyData px;
    DisplayPolyline pl;
    pl.pts = {{0, 0}, {1, 0}};
    px.graphics.items.push_back(pl);
    auto r = transformed(make(px), Transform2::translate({5, 5}));
    REQUIRE(r.ok());
    const auto& g = std::get<DisplayPolyline>(entityAs<ProxyData>(r.value())->graphics.items[0]);
    CHECK(g.pts[1] == Vec2{6, 5});
}

TEST_CASE("transform keeps the common properties and id") {
    Entity e = make(LineData{{0, 0}, {1, 1}});
    e.common.id = EntityId{42};
    e.common.color = Color::index(3);
    auto r = transformed(e, Transform2::translate({1, 1}));
    CHECK(r.value().common.id == EntityId{42});
    CHECK(r.value().common.color == Color::index(3));
}
