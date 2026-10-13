#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "helpers.h"
#include "tbcad/core/document.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

HatchData squareHatch(double side, double scale = 1) {
    Scene s;
    s.rectLines({0, 0}, {side, side});
    auto b = findBoundary(s.candidates(), {side / 2, side / 2});
    REQUIRE(b.ok());
    HatchData h;
    h.loops = b.value().loops;
    h.solid = false;
    h.patternScale = scale;
    return h;
}

Pattern oneFamily(double angleDeg, double spacing, std::vector<double> dashes = {}) {
    Pattern p;
    p.name = "TEST";
    PatternFamily f;
    f.angle = angleDeg * kPi / 180;
    f.delta = {0, spacing};
    f.dashes = std::move(dashes);
    p.families.push_back(f);
    return p;
}

double segLength(const std::pair<Vec2, Vec2>& s) {
    return distance(s.first, s.second);
}

bool onSquareBoundary(Vec2 p, double side, double eps = 1e-3) {
    return (std::abs(p.x) < eps || std::abs(p.x - side) < eps || std::abs(p.y) < eps ||
            std::abs(p.y - side) < eps) &&
           p.x > -eps && p.y > -eps && p.x < side + eps && p.y < side + eps;
}

}  // namespace

TEST_CASE("LINE45 on a 100 x 100 square at spacing 10: expected count and end points") {
    HatchData h = squareHatch(100);
    FillResult r = generateFill(h, oneFamily(45, 10));
    REQUIRE(r.mode == FillResult::Mode::Lines);
    // Lines x - y = d with d = 10 * sqrt(2) * k, k = -7..7: 15 lines, one chord each.
    CHECK(r.segments.size() == 15);
    CHECK(r.dots.empty());
    for (const auto& s : r.segments) {
        CHECK(onSquareBoundary(s.first, 100));
        CHECK(onSquareBoundary(s.second, 100));
        // 45 degrees: dx == dy along the segment.
        Vec2 d = s.second - s.first;
        CHECK(std::abs(std::abs(d.x) - std::abs(d.y)) < 1e-6);
    }
    std::vector<double> len;
    for (const auto& s : r.segments)
        len.push_back(segLength(s));
    std::sort(len.begin(), len.end());
    CHECK(len.back() == doctest::Approx(100 * std::sqrt(2.0)).epsilon(1e-4));  // the diagonal
    // Chord k has length sqrt(2) * (100 - 10 * sqrt(2) * |k|).
    for (int k = 0; k <= 7; ++k) {
        double want = std::sqrt(2.0) * (100 - 10 * std::sqrt(2.0) * k);
        CHECK(std::count_if(len.begin(), len.end(), [&](double l) {
                  return std::abs(l - want) < 1e-3;
              }) == (k == 0 ? 1 : 2));
    }
}

TEST_CASE("a hole in the loops splits the lines (even-odd)") {
    Scene s;
    s.rectLines({0, 0}, {100, 100});
    s.add(circle({50, 50}, 22));
    auto b = findBoundary(s.candidates(), {5, 5});
    REQUIRE(b.ok());
    HatchData h;
    h.loops = b.value().loops;
    h.solid = false;
    h.patternOrigin = {0, 5};  // lines at y = 5, 15, ... 95, none on a boundary
    FillResult r = generateFill(h, oneFamily(0, 10));
    REQUIRE(r.mode == FillResult::Mode::Lines);
    // Ten lines; the four with |y - 50| < 22 (35, 45, 55, 65) are cut in two by the circle.
    CHECK(r.segments.size() == 14);
    double total = 0;
    for (const auto& seg : r.segments)
        total += segLength(seg);
    double expect = 10 * 100.0;
    for (double y : {35.0, 45.0, 55.0, 65.0})
        expect -= 2 * std::sqrt(22.0 * 22.0 - (y - 50) * (y - 50));
    CHECK(total == doctest::Approx(expect).epsilon(1e-6));  // arcs are intersected exactly
}

TEST_CASE("dash lists are applied along each line from its own origin") {
    HatchData h = squareHatch(100);
    h.patternOrigin = {0, 5};
    FillResult r = generateFill(h, oneFamily(0, 10, {10, -5}));
    REQUIRE(r.mode == FillResult::Mode::Lines);
    // 10 lines; dashes at 0, 15, ..., 90 -> 7 dashes each, the last one ends exactly at x = 100.
    CHECK(r.segments.size() == 70);
    for (const auto& seg : r.segments) {
        CHECK(segLength(seg) == doctest::Approx(10).epsilon(1e-6));
        double x = std::min(seg.first.x, seg.second.x);
        CHECK(std::fmod(x, 15.0) == doctest::Approx(0).epsilon(1e-6).scale(1));
    }
}

TEST_CASE("dots, and the delta.x stagger") {
    HatchData h = squareHatch(100);
    h.patternOrigin = {0, 5};
    Pattern dots = oneFamily(0, 10, {0, -10});
    FillResult r = generateFill(h, dots);
    REQUIRE(r.mode == FillResult::Mode::Lines);
    CHECK(r.segments.empty());
    CHECK(r.dots.size() == 10 * 11);  // x = 0, 10, ..., 100 on each of the 10 rows
    // A stagger of 5 per row moves every other row's dots by 5.
    dots.families[0].delta.x = 5;
    FillResult st = generateFill(h, dots);
    int atFive = 0;
    for (const Vec2& p : st.dots)
        if (std::abs(std::fmod(p.x, 10.0) - 5) < 1e-6 && std::abs(p.y - 15) < 1e-6)
            ++atFive;
    CHECK(atFive > 0);
}

TEST_CASE("scale, angle and origin move the lines") {
    HatchData h = squareHatch(100);
    h.patternScale = 2;
    FillResult scaled = generateFill(h, oneFamily(45, 5));  // 5 * 2 = spacing 10
    CHECK(scaled.segments.size() == 15);

    h.patternScale = 1;
    h.patternAngle = kPi / 4;  // 45 + 45 = vertical lines
    FillResult rot = generateFill(h, oneFamily(45, 10));
    REQUIRE(rot.mode == FillResult::Mode::Lines);
    for (const auto& s : rot.segments)
        CHECK(std::abs(s.first.x - s.second.x) < 1e-6);
    CHECK(rot.segments.size() >= 10);

    h.patternAngle = 0;
    h.patternOrigin = {0, 3};
    FillResult moved = generateFill(h, oneFamily(0, 10));
    std::vector<double> ys;
    for (const auto& s : moved.segments)
        ys.push_back(s.first.y);
    std::sort(ys.begin(), ys.end());
    REQUIRE_FALSE(ys.empty());
    CHECK(std::fmod(ys.front() - 3 + 1000, 10) == doctest::Approx(0).scale(1).epsilon(1e-6));
}

TEST_CASE("a line along a boundary edge or through a vertex does not break the pairing") {
    HatchData h = squareHatch(100);
    FillResult r = generateFill(h, oneFamily(0, 25));  // y = 0, 25, 50, 75, 100
    REQUIRE(r.mode == FillResult::Mode::Lines);
    // Every drawn segment is a single full-width chord (the two on the boundary may be skipped).
    for (const auto& s : r.segments)
        CHECK(segLength(s) == doctest::Approx(100).epsilon(1e-4));
    CHECK(r.segments.size() >= 3);
    CHECK(r.segments.size() <= 5);
}

TEST_CASE("density guards fall back to a solid tone") {
    HatchData h = squareHatch(100);
    FillOptions opt;
    opt.pixelSizeMm = 0.5;  // one pixel is 0.5 mm; a 0.8 mm spacing is under two pixels
    FillResult dense = generateFill(h, oneFamily(45, 0.8), opt);
    CHECK(dense.mode == FillResult::Mode::SolidTone);
    CHECK(dense.reason == "too dense");
    CHECK_FALSE(dense.rings.empty());
    CHECK(generateFill(h, oneFamily(45, 5), opt).mode == FillResult::Mode::Lines);

    // Few lines but far too many dots: more than 200,000 segments.
    Scene big;
    big.rectLines({0, 0}, {20000, 20000});
    auto b = findBoundary(big.candidates(), {10, 10});
    REQUIRE(b.ok());
    HatchData huge;
    huge.loops = b.value().loops;
    huge.solid = false;
    FillResult capped = generateFill(huge, *findBuiltinPattern("DOTS"));
    CHECK(capped.mode == FillResult::Mode::SolidTone);
    CHECK(capped.reason == "too many segments");
    CHECK(capped.segments.empty());
    CHECK(capped.dots.empty());

    FillOptions few;
    few.maxSegments = 50;
    CHECK(generateFill(h, oneFamily(0, 10), few).mode == FillResult::Mode::Lines);     // 10 lines
    CHECK(generateFill(h, oneFamily(0, 1), few).mode == FillResult::Mode::SolidTone);  // 100
}

TEST_CASE("solid fill gives rings; islands are rings too") {
    Scene s;
    s.add(circle({0, 0}, 50));
    s.add(circle({0, 0}, 20));
    auto b = findBoundary(s.candidates(), {35, 0});
    REQUIRE(b.ok());
    HatchData h;
    h.loops = b.value().loops;
    h.solid = true;
    FillResult r = generateFill(h, *findBuiltinPattern("SOLID"));
    REQUIRE(r.mode == FillResult::Mode::Solid);
    REQUIRE(r.rings.size() == 2);
    auto area = [](const std::vector<Vec2>& ring) {
        double a = 0;
        for (std::size_t i = 0; i < ring.size(); ++i)
            a += cross(ring[i], ring[(i + 1) % ring.size()]);
        return std::abs(a) / 2;
    };
    CHECK(area(r.rings[0]) == doctest::Approx(kPi * 2500).epsilon(1e-3));
    CHECK(area(r.rings[1]) == doctest::Approx(kPi * 400).epsilon(1e-3));
    // A solid hatch ignores the pattern.
    h.solid = true;
    CHECK(generateFill(h, oneFamily(0, 10)).mode == FillResult::Mode::Solid);
}

TEST_CASE("every built-in pattern fills a square with something inside it") {
    HatchData h = squareHatch(60);
    for (const Pattern& p : builtinPatterns()) {
        INFO(p.name);
        FillResult r = generateFill(h, p);
        if (p.solid) {
            CHECK(r.mode == FillResult::Mode::Solid);
            continue;
        }
        REQUIRE(r.mode == FillResult::Mode::Lines);
        CHECK(r.segments.size() + r.dots.size() > 10);
        for (const auto& s : r.segments) {
            CHECK(s.first.x > -1e-3);
            CHECK(s.first.y > -1e-3);
            CHECK(s.second.x < 60 + 1e-3);
            CHECK(s.second.y < 60 + 1e-3);
        }
        for (const Vec2& d : r.dots) {
            CHECK(d.x > -1e-6);
            CHECK(d.y > -1e-6);
            CHECK(d.x < 60 + 1e-6);
            CHECK(d.y < 60 + 1e-6);
        }
    }
}

TEST_CASE("BRICK joints alternate from row to row") {
    HatchData h = squareHatch(48);
    FillResult r = generateFill(h, *findBuiltinPattern("BRICK"));
    REQUIRE(r.mode == FillResult::Mode::Lines);
    int row0 = 0, row1 = 0;
    for (const auto& s : r.segments) {
        if (std::abs(s.first.x - s.second.x) > 1e-6)
            continue;  // a horizontal course line
        double x = s.first.x;
        double yMid = (s.first.y + s.second.y) / 2;
        CHECK(segLength(s) == doctest::Approx(6).epsilon(1e-4));
        bool evenRow = std::fmod(std::floor(yMid / 6), 2.0) == 0;
        // Even rows have joints at multiples of 12, odd rows at 6 + multiples of 12.
        double expectX = evenRow ? 0.0 : 6.0;
        CHECK(std::fmod(x - expectX + 1200, 12.0) == doctest::Approx(0).scale(1).epsilon(1e-6));
        (evenRow ? row0 : row1)++;
    }
    CHECK(row0 > 0);
    CHECK(row1 > 0);
}

TEST_CASE("an arc boundary is intersected exactly: lines meet the circle on the circle") {
    Scene s;
    s.add(circle({0, 0}, 37));
    auto b = findBoundary(s.candidates(), {0, 0});
    REQUIRE(b.ok());
    HatchData h;
    h.loops = b.value().loops;
    h.solid = false;
    h.patternOrigin = {0, 1};
    FillResult r = generateFill(h, oneFamily(30, 3));
    REQUIRE(r.mode == FillResult::Mode::Lines);
    REQUIRE(r.segments.size() > 20);
    for (const auto& seg : r.segments) {
        CHECK(length(seg.first) == doctest::Approx(37).epsilon(1e-9));
        CHECK(length(seg.second) == doctest::Approx(37).epsilon(1e-9));
    }
}

TEST_CASE("the display generator draws hatches through the geom registry") {
    registerHatchDisplay();
    registerHatchDisplay();  // harmless twice
    CHECK(geom::DisplayRegistry::has(EntityKind::Hatch));

    Document doc;
    Scene s;
    s.rectLines({0, 0}, {50, 50});
    auto b = findBoundary(s.candidates(), {25, 25});
    REQUIRE(b.ok());

    EntityId id1, id2;
    {
        Transaction tx = doc.begin("hatches");
        Entity patterned = makeHatchEntity(b.value(), *findBuiltinPattern("LINE45"), 5, 0, {});
        Entity solid = makeHatchEntity(b.value(), *findBuiltinPattern("SOLID"), 1, 0, {});
        solid.common.color = Color::index(1);
        id1 = tx.add(std::move(patterned)).value();
        id2 = tx.add(std::move(solid)).value();
        tx.commit();
    }
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.05;
    DisplayList lines, fill;
    geom::buildDisplay(doc, *doc.find(id1), ctx, lines);
    geom::buildDisplay(doc, *doc.find(id2), ctx, fill);
    REQUIRE_FALSE(lines.items.empty());
    CHECK(std::holds_alternative<DisplayPolyline>(lines.items[0]));
    CHECK(std::get<DisplayPolyline>(lines.items[0]).pts.size() == 2);
    CHECK(std::get<DisplayPolyline>(lines.items[0]).style.pattern == nullptr);
    REQUIRE(fill.items.size() == 1);
    CHECK(std::holds_alternative<DisplayFill>(fill.items[0]));
    CHECK(std::get<DisplayFill>(fill.items[0]).style.color == paletteColor(1));

    BBox2 box = geom::bounds(*doc.find(id1), doc);
    CHECK(box.min.x == doctest::Approx(0));
    CHECK(box.max.y == doctest::Approx(50));

    // Zoomed far out, the same hatch becomes a tone instead of thousands of lines.
    ctx.pixelSizeMm = 10;
    DisplayList far;
    geom::buildDisplay(doc, *doc.find(id1), ctx, far);
    REQUIRE(far.items.size() == 1);
    CHECK(std::holds_alternative<DisplayFill>(far.items[0]));
    CHECK(std::get<DisplayFill>(far.items[0]).style.color.a < 255);
}

TEST_CASE("explodeHatch: lines for patterns, boundary edges for solids") {
    Scene s;
    s.rectLines({0, 0}, {40, 40});
    s.add(circle({20, 20}, 5));
    auto b = findBoundary(s.candidates(), {2, 2});
    REQUIRE(b.ok());
    Document doc;
    Entity pat = makeHatchEntity(b.value(), *findBuiltinPattern("LINE0"), 5, 0, {0, 5});
    pat.common.layer = LayerId{1};
    auto lines = explodeHatch(doc, pat);
    REQUIRE(lines.ok());
    CHECK(lines.value().size() == 4);  // y = 5, 20 (cut in two by the circle), 35
    for (const Entity& e : lines.value()) {
        CHECK(kind(e) == EntityKind::Line);
        CHECK(e.common.layer == LayerId{1});
    }

    Entity solid = makeHatchEntity(b.value(), *findBuiltinPattern("SOLID"), 1, 0, {});
    auto edges = explodeHatch(doc, solid);
    REQUIRE(edges.ok());
    REQUIRE(edges.value().size() == 5);
    CHECK(std::count_if(edges.value().begin(), edges.value().end(),
                        [](const Entity& e) { return kind(e) == EntityKind::Circle; }) == 1);

    CHECK_FALSE(explodeHatch(doc, s.candidates()[0].entity[0]).ok());
}
