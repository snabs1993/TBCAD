#include <chrono>
#include <cmath>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::testing;
using namespace tbcad::modify;

namespace {
double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
Entity wavyPoly(int n) {
    std::vector<PolyVertex> v;
    for (int i = 0; i < n; ++i) {
        const double a = 6.283185307 * i / n;
        v.push_back(PolyVertex{{200 * std::cos(a) * (1 + 0.1 * std::sin(a * 20)), 200 * std::sin(a)}, 0, 0, 0});
    }
    return poly(v, false);
}
}  // namespace

TEST_CASE("a polyline of thousands of vertices becomes a spline through every vertex") {
    for (int n : {300, 2000, 20000}) {
        const Entity p = wavyPoly(n);
        const auto t = std::chrono::steady_clock::now();
        auto sp = polylineToSpline(p);
        REQUIRE(sp.ok());
        CHECK(msSince(t) < 5000.0);
        std::vector<geom::CurveRef> cs;
        REQUIRE(geom::curvesOf(sp.value(), cs));
        const auto& pl = as<PolylineData>(p);
        for (int i = 0; i < n; i += n / 25) {
            double best = 1e300;
            for (const auto& c : cs)
                best = std::min(best, c.distanceTo(pl.v[static_cast<std::size_t>(i)].p));
            CHECK(best < 1e-3);
        }
    }
}

TEST_CASE("PEDIT vertex edits on 20000 vertices are quick") {
    const Entity p = wavyPoly(20000);
    const auto t = std::chrono::steady_clock::now();
    CHECK(insertPolylineVertex(p, {10, 10}).ok());
    CHECK(removePolylineVertex(p, {10, 10}).ok());
    CHECK(movePolylineVertex(p, {10, 10}, {11, 11}).ok());
    CHECK(straightenPolyline(p, {200, 0}, {0, 200}).ok());
    CHECK(taperPolyline(p, 1, 5).ok());
    CHECK(fitPolylineCurve(p).ok());
    CHECK(msSince(t) < 2000.0);
}
