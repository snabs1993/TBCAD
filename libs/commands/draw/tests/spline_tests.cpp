#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

namespace {

const std::vector<Vec2> kPts = {{0, 0}, {10, 10}, {20, 0}, {30, 10}};

void checkPassesThrough(const Entity* e, const std::vector<Vec2>& pts) {
    for (const Vec2& p : pts) {
        CHECK_MESSAGE(geom::distance(*e, p) < 1e-6, "point (", p.x, ", ", p.y, ")");
    }
}

}  // namespace

TEST_CASE("SPLINE through fit points (natural ends)") {
    Fixture f;
    CHECK(f.host.run("SPL", {"0,0", "10,10", "20,0", "30,10", "", "", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.degree == 3);
    CHECK_FALSE(s.closed);
    REQUIRE(s.fit.size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(close(s.fit[i], kPts[i]));
    CHECK(s.knots.size() == s.control.size() + 4);
    checkPassesThrough(es[0], kPts);
}

TEST_CASE("SPLINE fit matches geom::interpolateSpline exactly") {
    Fixture f;
    f.host.run("SPLINE", {"0,0", "10,10", "20,0", "30,10", "", "", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    auto ref = geom::interpolateSpline(kPts);
    REQUIRE(ref.ok());
    REQUIRE(s.control.size() == ref.value().control.size());
    for (std::size_t i = 0; i < s.control.size(); ++i)
        CHECK(close(s.control[i], ref.value().control[i]));
    REQUIRE(s.knots.size() == ref.value().knots.size());
    for (std::size_t i = 0; i < s.knots.size(); ++i)
        CHECK(close(s.knots[i], ref.value().knots[i]));
}

TEST_CASE("SPLINE with a start tangent leaves the first point in that direction") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10", "20,0", "", "10,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    auto curve = geom::SplineCurve::make(s);
    REQUIRE(curve.ok());
    const Vec2 d = curve.value()->derivativeAt(curve.value()->tMin());
    CHECK(std::abs(d.y) < 1e-9 * std::max(1.0, std::abs(d.x)));
    CHECK(d.x > 0);
    checkPassesThrough(es[0], {{0, 0}, {10, 10}, {20, 0}});
}

TEST_CASE("SPLINE with an end tangent arrives at the last point in that direction") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10", "20,0", "", "", "20,-10"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    auto curve = geom::SplineCurve::make(indirect<SplineData>(es[0]));
    REQUIRE(curve.ok());
    const Vec2 d = curve.value()->derivativeAt(curve.value()->tMax());
    CHECK(std::abs(d.x) < 1e-9 * std::max(1.0, std::abs(d.y)));
    CHECK(d.y < 0);
}

TEST_CASE("SPLINE Close makes a closed fit spline without asking for tangents") {
    Fixture f;
    auto r = f.host.run("SPL", {"0,0", "10,10", "20,0", "30,10", "Close"});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.closed);
    checkPassesThrough(es[0], kPts);
    auto curve = geom::SplineCurve::make(s);
    REQUIRE(curve.ok());
    CHECK(close(curve.value()->pointAt(curve.value()->tMin()),
                curve.value()->pointAt(curve.value()->tMax()), 1e-6));
}

TEST_CASE("SPLINE with two fit points is a straight spline") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,0", "", "", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(indirect<SplineData>(es[0]).degree == 1);
    checkPassesThrough(es[0], {{0, 0}, {10, 0}, {5, 0}});
}

TEST_CASE("SPLINE CV uses the points as control vertices") {
    Fixture f;
    CHECK(f.host.run("SPL", {"Method", "CV", "0,0", "10,10", "20,0", "30,10", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.degree == 3);
    CHECK(s.fit.empty());
    REQUIRE(s.control.size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(close(s.control[i], kPts[i]));
    const std::vector<double> knots = {0, 0, 0, 0, 1, 1, 1, 1};
    REQUIRE(s.knots.size() == knots.size());
    for (std::size_t i = 0; i < knots.size(); ++i)
        CHECK(close(s.knots[i], knots[i]));
    // A clamped spline starts and ends at its first and last control vertex.
    auto curve = geom::SplineCurve::make(s);
    REQUIRE(curve.ok());
    CHECK(close(curve.value()->pointAt(curve.value()->tMin()), {0, 0}, 1e-9));
    CHECK(close(curve.value()->pointAt(curve.value()->tMax()), {30, 10}, 1e-9));
}

TEST_CASE("SPLINE CV with five vertices has an interior knot") {
    Fixture f;
    f.host.run("SPL", {"M", "CV", "0,0", "10,10", "20,0", "30,10", "40,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    const std::vector<double> knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    REQUIRE(s.knots.size() == knots.size());
    for (std::size_t i = 0; i < knots.size(); ++i)
        CHECK(close(s.knots[i], knots[i]));
}

TEST_CASE("SPLINE CV with three vertices drops to degree 2") {
    Fixture f;
    f.host.run("SPL", {"M", "CV", "0,0", "10,10", "20,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.degree == 2);
    CHECK(s.knots.size() == 6);
}

TEST_CASE("SPLINE CV Close makes a periodic spline whose ends meet") {
    Fixture f;
    f.host.run("SPL", {"Method", "CV", "0,0", "10,10", "20,0", "30,10", "Close"});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.closed);
    CHECK(s.periodic);
    CHECK(s.control.size() == 4 + 3);
    CHECK(s.knots.size() == s.control.size() + 4);
    auto curve = geom::SplineCurve::make(s);
    REQUIRE(curve.ok());
    CHECK(close(curve.value()->pointAt(curve.value()->tMin()),
                curve.value()->pointAt(curve.value()->tMax()), 1e-6));
}

TEST_CASE("SPLINE Undo removes the last point") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10", "20,0", "30,10", "Undo", "40,0", "", "", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    REQUIRE(s.fit.size() == 4);
    CHECK(close(s.fit[3], {40, 0}));
}

TEST_CASE("SPLINE: Esc draws nothing, one point is not enough") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10", ScriptItem::esc()});
    CHECK(f.entities().empty());
    f.host.run("SPL", {"0,0", ""});
    CHECK(f.entities().empty());
    f.host.run("SPL", {"0,0", "10,10", "", ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("SPLINE previews the curve and is one undo step") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10"});
    cmd::RawInput in;
    in.worldCursor = {20, 0};
    f.host.runner().pointerMoved(in);
    CHECK_FALSE(f.host.preview().items.empty());
    f.host.feed({"20,0", "", "", ""});
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("SPLINE Object converts a polyline into a fit spline and replaces it") {
    Fixture f;
    f.host.run("PLINE", {"0,0", "10,10", "20,0", "30,10", ""});
    REQUIRE(f.entities().size() == 1);
    CHECK(f.host.run("SPL", {"Object", ScriptItem::pick(f.entities()[0]->common.id, {5, 5})})
              .finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    REQUIRE(s.fit.size() == 4);
    checkPassesThrough(es[0], kPts);
}

TEST_CASE("SPLINE Tangents sets both end tangents before Enter") {
    Fixture f;
    f.host.run("SPL", {"0,0", "10,10", "20,0", "Tangents", "10,0", "30,0", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    auto curve = geom::SplineCurve::make(indirect<SplineData>(es[0]));
    REQUIRE(curve.ok());
    const Vec2 d = curve.value()->derivativeAt(curve.value()->tMin());
    CHECK(std::abs(d.y) < 1e-9 * std::max(1.0, std::abs(d.x)));
}

TEST_CASE("SPLINE toLerance drops fit points that lie within the tolerance of the curve") {
    Fixture f;
    // the middle point is 0.05 off a straight run
    f.host.run("SPL", {"0,0", "toLerance", "0.5", "10,0.05", "20,0", "", "", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& s = indirect<SplineData>(es[0]);
    CHECK(s.fit.size() == 2);
    CHECK(geom::distance(*es[0], Vec2{10, 0.05}) <= 0.5);
}
