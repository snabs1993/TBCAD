#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/constraints/constraints.h"

using namespace tbcad;
using namespace tbcad::constraints;

namespace {

struct Fixture {
    Document doc;

    EntityId line(Vec2 a, Vec2 b) {
        auto tx = doc.begin("line");
        auto id = tx.add(Entity{{}, LineData{a, b}});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    EntityId circle(Vec2 c, double r) {
        auto tx = doc.begin("circle");
        auto id = tx.add(Entity{{}, CircleData{c, r}});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    EntityId arc(Vec2 c, double r, double a0, double a1) {
        auto tx = doc.begin("arc");
        auto id = tx.add(Entity{{}, ArcData{c, r, a0, a1}});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    LineData getLine(EntityId id) { return *entityAs<LineData>(*doc.find(id)); }
    CircleData getCircle(EntityId id) { return *entityAs<CircleData>(*doc.find(id)); }
    ArcData getArc(EntityId id) { return *entityAs<ArcData>(*doc.find(id)); }

    SolveResult solveWith(const std::vector<Constraint>& cs, const SolveOptions& opt = {}) {
        ConstraintSet set;
        for (const Constraint& c : cs)
            set.add(c);
        auto tx = doc.begin("solve");
        auto res = solve(tx, set, opt);
        REQUIRE(res.ok());
        tx.commit();
        return res.value();
    }
};

Constraint make(Kind k, Ref a, Ref b = {}, double v = 0) {
    Constraint c;
    c.kind = k;
    c.a = a;
    c.b = b;
    c.value = v;
    return c;
}
Ref whole(EntityId e) {
    return {e, Part::Whole};
}
Ref at(EntityId e, Part p) {
    return {e, p};
}

}  // namespace

TEST_CASE("horizontal and vertical lines") {
    Fixture f;
    const EntityId h = f.line({0, 0}, {10, 1.5});
    const EntityId v = f.line({0, 0}, {1.5, 10});
    auto r = f.solveWith({make(Kind::Horizontal, whole(h)), make(Kind::Vertical, whole(v))});
    CHECK(r.converged);
    CHECK(f.getLine(h).a.y == doctest::Approx(f.getLine(h).b.y));
    CHECK(f.getLine(v).a.x == doctest::Approx(f.getLine(v).b.x));
    // Solutions stay close to where the lines were.
    CHECK(f.getLine(h).b.x == doctest::Approx(10).epsilon(0.05));
}

TEST_CASE("coincident joins two end points") {
    Fixture f;
    const EntityId a = f.line({0, 0}, {10, 0});
    const EntityId b = f.line({10.5, 0.7}, {20, 5});
    auto r = f.solveWith({make(Kind::Coincident, at(a, Part::End), at(b, Part::Start))});
    CHECK(r.converged);
    const Vec2 e = f.getLine(a).b, s = f.getLine(b).a;
    CHECK(e.x == doctest::Approx(s.x));
    CHECK(e.y == doctest::Approx(s.y));
}

TEST_CASE("parallel, perpendicular and angle between lines") {
    Fixture f;
    const EntityId a = f.line({0, 0}, {10, 0});
    const EntityId b = f.line({0, 5}, {10, 8});
    const EntityId c = f.line({0, 0}, {3, 9});
    auto r =
        f.solveWith({make(Kind::Parallel, whole(a), whole(b)),
                     make(Kind::Perpendicular, whole(a), whole(c)), make(Kind::Fix, whole(a))});
    CHECK(r.converged);
    const LineData la = f.getLine(a), lb = f.getLine(b), lc = f.getLine(c);
    CHECK(la.a.x == doctest::Approx(0));
    CHECK(la.b.x == doctest::Approx(10));
    const Vec2 da = la.b - la.a, db = lb.b - lb.a, dc = lc.b - lc.a;
    CHECK(da.x * db.y - da.y * db.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(da.x * dc.x + da.y * dc.y == doctest::Approx(0).epsilon(1e-6));

    Fixture g;
    const EntityId p = g.line({0, 0}, {10, 0});
    const EntityId q = g.line({0, 0}, {8, 3});
    auto r2 =
        g.solveWith({make(Kind::Fix, whole(p)), make(Kind::Angle, whole(p), whole(q), kPi / 3)});
    CHECK(r2.converged);
    const LineData lq = g.getLine(q);
    CHECK(std::atan2(lq.b.y - lq.a.y, lq.b.x - lq.a.x) == doctest::Approx(kPi / 3).epsilon(1e-6));
}

TEST_CASE("distance, radius and equal") {
    Fixture f;
    const EntityId l = f.line({0, 0}, {7, 0});
    const EntityId c1 = f.circle({0, 10}, 2);
    const EntityId c2 = f.circle({20, 10}, 5);
    auto r = f.solveWith({make(Kind::Fix, at(l, Part::Start)), make(Kind::Horizontal, whole(l)),
                          make(Kind::Distance, at(l, Part::Start), at(l, Part::End), 25),
                          make(Kind::Radius, whole(c1), {}, 4),
                          make(Kind::Equal, whole(c1), whole(c2))});
    CHECK(r.converged);
    CHECK(f.getLine(l).b.x == doctest::Approx(25));
    CHECK(f.getLine(l).b.y == doctest::Approx(0).epsilon(1e-6));
    CHECK(f.getCircle(c1).radius == doctest::Approx(4));
    CHECK(f.getCircle(c2).radius == doctest::Approx(4));

    Fixture g;
    const EntityId a = g.line({0, 0}, {10, 0});
    const EntityId b = g.line({0, 3}, {6, 3});
    auto r2 = g.solveWith({make(Kind::Equal, whole(a), whole(b)), make(Kind::Fix, whole(a))});
    CHECK(r2.converged);
    const LineData lb = g.getLine(b);
    CHECK(length(lb.b - lb.a) == doctest::Approx(10));
}

TEST_CASE("tangent: line to circle, circle to circle (outside and inside)") {
    Fixture f;
    const EntityId l = f.line({-10, 5}, {10, 5.4});
    const EntityId c = f.circle({0, 0}, 3);
    auto r = f.solveWith({make(Kind::Fix, whole(c)), make(Kind::Tangent, whole(l), whole(c))});
    CHECK(r.converged);
    const LineData ll = f.getLine(l);
    const Vec2 d = ll.b - ll.a;
    const double dist = std::abs(d.x * (0 - ll.a.y) - d.y * (0 - ll.a.x)) / length(d);
    CHECK(dist == doctest::Approx(3));

    Fixture g;
    const EntityId a = g.circle({0, 0}, 3);
    const EntityId b = g.circle({8, 0}, 2);
    auto r2 = g.solveWith({make(Kind::Fix, whole(a)), make(Kind::Tangent, whole(a), whole(b))});
    CHECK(r2.converged);
    CHECK(length(g.getCircle(b).center - g.getCircle(a).center) == doctest::Approx(5));  // outside

    Fixture h;
    const EntityId big = h.circle({0, 0}, 6);
    const EntityId small = h.circle({3.5, 0}, 2);
    auto r3 =
        h.solveWith({make(Kind::Fix, whole(big)), make(Kind::Tangent, whole(big), whole(small))});
    CHECK(r3.converged);
    CHECK(length(h.getCircle(small).center - h.getCircle(big).center) ==
          doctest::Approx(4));  // inside
}

TEST_CASE("arc end points can be joined to lines") {
    Fixture f;
    const EntityId l = f.line({10, 0}, {20, 0});
    const EntityId a = f.arc({0, 0}, 10, 0.2, 1.5);
    auto r = f.solveWith({make(Kind::Fix, whole(l)),
                          make(Kind::Coincident, at(a, Part::Start), at(l, Part::Start))});
    CHECK(r.converged);
    const ArcData arc = f.getArc(a);
    const Vec2 s{arc.center.x + arc.radius * std::cos(arc.startAngle),
                 arc.center.y + arc.radius * std::sin(arc.startAngle)};
    CHECK(s.x == doctest::Approx(10));
    CHECK(s.y == doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("pinned entities stay put while the others follow") {
    Fixture f;
    const EntityId a = f.line({0, 0}, {10, 0});
    const EntityId b = f.line({0, 5}, {10, 5});
    // The user drags line a to a slanted position; b must become parallel to it.
    {
        auto tx = f.doc.begin("drag");
        REQUIRE(tx.modify(a, [](Entity& e) { entityAs<LineData>(e)->b = {10, 4}; }).ok());
        tx.commit();
    }
    SolveOptions opt;
    opt.pinned = {a};
    auto r = f.solveWith({make(Kind::Parallel, whole(a), whole(b))}, opt);
    CHECK(r.converged);
    CHECK(f.getLine(a).b.y == 4);
    const Vec2 db = f.getLine(b).b - f.getLine(b).a;
    CHECK(db.y / db.x == doctest::Approx(0.4).epsilon(1e-6));
}

TEST_CASE("degrees of freedom and conflicts") {
    Fixture f;
    const EntityId l = f.line({0, 0}, {10, 0});
    auto r = f.solveWith({make(Kind::Horizontal, whole(l))});
    CHECK(r.converged);
    CHECK(r.degreesOfFreedom == 3);  // 4 coordinates, one equation

    Fixture g;
    const EntityId m = g.line({0, 0}, {10, 0});
    ConstraintSet set;
    set.add(make(Kind::Fix, whole(m)));
    const auto bad = set.add(make(Kind::Distance, at(m, Part::Start), at(m, Part::End), 30));
    auto tx = g.doc.begin("conflict");
    auto res = solve(tx, set);
    REQUIRE(res.ok());
    CHECK_FALSE(res.value().converged);
    CHECK(res.value().worst == bad);
    CHECK(res.value().changedEntities == 0);
    tx.rollback();
    CHECK(g.getLine(m).b.x == 10);  // a failed solve leaves the drawing alone
}

TEST_CASE("measure gives the current value of a dimension") {
    Fixture f;
    const EntityId a = f.line({0, 0}, {3, 4});
    const EntityId b = f.line({0, 0}, {0, 5});
    const EntityId c = f.circle({0, 0}, 7.5);
    auto d = measure(f.doc, make(Kind::Distance, at(a, Part::Start), at(a, Part::End)));
    REQUIRE(d.ok());
    CHECK(d.value() == doctest::Approx(5));
    CHECK(measure(f.doc, make(Kind::Radius, whole(c))).value() == doctest::Approx(7.5));
    CHECK(measure(f.doc, make(Kind::Angle, whole(a), whole(b))).value() ==
          doctest::Approx(kPi / 2 - std::atan2(4, 3)));
    CHECK_FALSE(measure(f.doc, make(Kind::Parallel, whole(a), whole(b))).ok());
}
