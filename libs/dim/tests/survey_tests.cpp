// Dimensions far from the origin (survey coordinates) must come out as the same drawing shifted:
// the text is the same and the geometry moves by exactly the offset, to the precision a double
// has at that distance.
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "support.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

namespace {

DimensionData shifted(DimensionData d, Vec2 o) {
    for (Vec2& p : d.defPts)
        p = p + o;
    d.textPos = d.textPos + o;
    return d;
}

std::vector<std::pair<std::string, DimensionData>> samples() {
    std::vector<std::pair<std::string, DimensionData>> v;
    v.push_back({"linear", linear({3.1, 4.7}, {123.456, 9.2}, {60, 30})});
    v.push_back({"linear rotated", linear({3.1, 4.7}, {123.456, 9.2}, {60, 30}, 1.1)});
    v.push_back({"linear vertical", linear({3.1, 4.7}, {123.456, 90.2}, {-20, 30}, kPi / 2)});
    v.push_back({"aligned", aligned({3.1, 4.7}, {123.456, 9.2}, {60, 30})});
    v.push_back({"aligned short", aligned({0, 0}, {1.3, 0.2}, {0.6, 5})});
    v.push_back({"radius", radial(DimKind::Radius, {10, 10}, {40.5, 30})});
    v.push_back({"diameter", radial(DimKind::Diameter, {10, 10}, {70.5, 30})});
    v.push_back({"angular 2 line", angular2({0, 0}, {50, 0}, {0, 0}, {35.355, 35.355}, {20, 12})});
    {
        DimensionData d;
        d.kind = DimKind::Angular3Point;
        d.defPts = {Vec2{50, 0}, Vec2{0, 40}, Vec2{0, 0}, Vec2{25, 25}};
        v.push_back({"angular 3 point", d});
    }
    {
        DimensionData d;
        d.kind = DimKind::ArcLength;
        d.defPts = {Vec2{0, 0}, Vec2{30, 0}, Vec2{0, 30}, Vec2{25, 25}};
        v.push_back({"arc length", d});
    }
    {
        DimensionData d;
        d.kind = DimKind::Jogged;
        d.defPts = {Vec2{0, 0}, Vec2{30, 0}, Vec2{-100, 20}, Vec2{-60, 10}};
        d.textPos = {-50, 20};
        v.push_back({"jogged", d});
    }
    return v;
}

}  // namespace

TEST_CASE("dimensions at survey coordinates are the same drawing, shifted") {
    Document doc;
    for (const auto& [name, base] : samples()) {
        CAPTURE(name);
        DimensionData origin = base;
        refresh(doc, origin);
        const Layout L0 = layoutDimension(doc, origin);
        REQUIRE(L0.text.valid());
        for (double off : {1e5, 1e6, 5e8}) {
            CAPTURE(off);
            const Vec2 o{off, -off * 0.7};
            DimensionData far = shifted(origin, o);
            refresh(doc, far);
            const Layout L1 = layoutDimension(doc, far);
            const double eps = off > 1e8 ? 2e-4 : 1e-6;
            CHECK(L1.text.utf8 == L0.text.utf8);
            CHECK(near(L1.text.center - o, L0.text.center, eps));
            CHECK(L1.text.rotation == doctest::Approx(L0.text.rotation).epsilon(1e-6));
            REQUIRE(L1.lines.size() == L0.lines.size());
            for (std::size_t i = 0; i < L0.lines.size(); ++i) {
                REQUIRE(L1.lines[i].pts.size() == L0.lines[i].pts.size());
                for (std::size_t k = 0; k < L0.lines[i].pts.size(); ++k)
                    CHECK(near(L1.lines[i].pts[k] - o, L0.lines[i].pts[k], eps));
            }
            REQUIRE(L1.arcs.size() == L0.arcs.size());
            for (std::size_t i = 0; i < L0.arcs.size(); ++i) {
                CHECK(near(L1.arcs[i].center - o, L0.arcs[i].center, eps));
                CHECK(L1.arcs[i].radius == doctest::Approx(L0.arcs[i].radius).epsilon(1e-6));
            }
            REQUIRE(L1.arrows.size() == L0.arrows.size());
            for (std::size_t i = 0; i < L0.arrows.size(); ++i) {
                REQUIRE(L1.arrows[i].ring.size() == L0.arrows[i].ring.size());
                for (std::size_t k = 0; k < L0.arrows[i].ring.size(); ++k)
                    CHECK(near(L1.arrows[i].ring[k] - o, L0.arrows[i].ring[k], eps));
            }
        }
    }
}
