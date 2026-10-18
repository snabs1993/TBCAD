#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

// Stand-ins for the display generators of libs/text and libs/dim (the drafting tests do not link
// them): an outline around what the object snaps to, so the object has a bounding box.
void registerOutlines() {
    static const bool once = [] {
        auto box = [](DisplayList& out, std::vector<Vec2> pts) {
            BBox2 b;
            for (const Vec2& p : pts)
                b.expand(p);
            DisplayPolyline pl;
            pl.closed = true;
            pl.pts = {b.min, {b.max.x, b.min.y}, b.max, {b.min.x, b.max.y}};
            out.items.emplace_back(std::move(pl));
        };
        geom::DisplayRegistry::set(
            EntityKind::Text,
            [box](const Document&, const Entity& e, const geom::DisplayContext&, DisplayList& out) {
                const auto& t = *entityAs<TextData>(e);
                box(out, {t.position, t.alignPoint, t.position + Vec2{20, 3}});
            });
        geom::DisplayRegistry::set(
            EntityKind::MText,
            [box](const Document&, const Entity& e, const geom::DisplayContext&, DisplayList& out) {
                const auto& t = *entityAs<MTextData>(e);
                box(out, {t.position, t.position + Vec2{20, -6}});
            });
        geom::DisplayRegistry::set(
            EntityKind::Dimension,
            [box](const Document&, const Entity& e, const geom::DisplayContext&, DisplayList& out) {
                const auto& d = *entityAs<DimensionData>(e);
                box(out, {d.defPts[0], d.defPts[1], d.defPts[2], d.defPts[3], d.textPos});
            });
        geom::DisplayRegistry::set(
            EntityKind::Leader,
            [box](const Document&, const Entity& e, const geom::DisplayContext&, DisplayList& out) {
                const auto& l = *entityAs<LeaderData>(e);
                std::vector<Vec2> pts = l.vertices;
                pts.push_back(l.textPos);
                box(out, pts);
            });
        return true;
    }();
    (void)once;
}

struct Modes : World {
    Modes() { registerOutlines(); }
};

Vec2 snapped(World& w, Vec2 cursor, const char* modes, std::optional<Vec2> base = std::nullopt,
             cmd::FilterFeedback* fb = nullptr) {
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, modes);
    return w.hover(cursor, base, 1.0, fb);
}

}  // namespace

TEST_CASE("the new snap modes have names, labels and short forms") {
    CHECK(parseSnapMode("insertion") == SnapMode::Insertion);
    CHECK(parseSnapMode("ins") == SnapMode::Insertion);
    CHECK(parseSnapMode("int") == SnapMode::Intersection);
    CHECK(parseSnapMode("app") == SnapMode::ApparentIntersection);
    CHECK(parseSnapMode("apparent") == SnapMode::ApparentIntersection);
    CHECK(parseSnapMode("ext") == SnapMode::Extension);
    CHECK(parseSnapMode("end") == SnapMode::Endpoint);
    CHECK_FALSE(parseSnapMode("e").has_value());  // endpoint or extension
    CHECK(snapModeLabel(SnapMode::ApparentIntersection) == "Apparent intersection");
    for (int i = 0; i < kSnapModeCount; ++i) {
        const auto m = static_cast<SnapMode>(i);
        CHECK(parseSnapMode(snapModeName(m)) == m);
        CHECK((kAllSnapModes & maskOf(m)) != 0);
    }
    // The new modes are opt-in.
    CHECK((kDefaultSnapModes & maskOf(SnapMode::Insertion)) == 0);
    CHECK((kDefaultSnapModes & maskOf(SnapMode::ApparentIntersection)) == 0);
    CHECK((kDefaultSnapModes & maskOf(SnapMode::Extension)) == 0);
    auto mask = parseSnapModes("endpoint, insertion apparent,extension");
    REQUIRE(mask);
    CHECK(parseSnapModes(formatSnapModes(*mask)) == mask);
}

TEST_CASE("insertion snaps to text, mtext and dimension text positions") {
    Modes w;
    TextData t;
    t.position = {10, 10};
    t.alignPoint = {10, 10};
    t.height = 2;
    t.utf8 = "left";
    w.add(Entity{{}, Indirect<TextData>(t)});
    TextData c;
    c.position = {40, 8};  // lower left of the centred text
    c.alignPoint = {50, 10};
    c.h = HAlign::Center;
    c.height = 2;
    c.utf8 = "centred";
    w.add(Entity{{}, Indirect<TextData>(c)});
    MTextData m;
    m.position = {80, 30};
    m.height = 2;
    m.utf8 = "multi";
    w.add(Entity{{}, Indirect<MTextData>(m)});
    cmd::FilterFeedback fb;
    CHECK(near(snapped(w, {12, 11}, "insertion", {}, &fb), {10, 10}));
    CHECK(fb.marker == "insertion");
    CHECK(near(snapped(w, {52, 12}, "insertion"), {50, 10}));  // the alignment point
    CHECK(near(snapped(w, {79, 31}, "insertion"), {80, 30}));
    CHECK(near(snapped(w, {12, 11}, "endpoint"), {12, 11}));  // mode off: nothing
}

TEST_CASE("dimension definition points, leader vertices and ray origins are snappable") {
    Modes w;
    DimensionData d;
    d.kind = DimKind::Linear;
    d.defPts = {Vec2{0, 0}, Vec2{100, 0}, Vec2{50, -30}, Vec2{}};
    d.textPos = {50, -30};
    w.add(Entity{{}, Indirect<DimensionData>(d)});
    LeaderData l;
    l.vertices = {{200, 0}, {220, 20}, {240, 20}};
    l.textPos = {241, 20};
    w.add(Entity{{}, Indirect<LeaderData>(l)});
    RayData r;
    r.origin = {300, 0};
    r.dir = {1, 1};
    w.add(Entity{{}, Indirect<RayData>(r)});
    RayData x;
    x.origin = {400, 0};
    x.dir = {1, 0};
    x.infinite = true;
    w.add(Entity{{}, Indirect<RayData>(x)});
    CHECK(near(snapped(w, {101, 1}, "node"), {100, 0}));
    CHECK(near(snapped(w, {51, -29}, "insertion"), {50, -30}));
    CHECK(near(snapped(w, {221, 19}, "endpoint"), {220, 20}));
    CHECK(near(snapped(w, {210, 11}, "midpoint"), {210, 10}));
    CHECK(near(snapped(w, {301, 1}, "endpoint"), {300, 0}));
    CHECK(near(snapped(w, {320, 21}, "nearest"), {320.5, 20.5}, 1e-6));  // the ray itself
    CHECK(near(snapped(w, {401, 1}, "endpoint"), {401, 1}));             // an xline has no end
    CHECK(near(snapped(w, {450, 3}, "nearest"), {450, 0}, 1e-6));
}

TEST_CASE("apparent intersection: where two objects would meet if extended") {
    World w;
    w.line({0, 0}, {10, 0});
    w.line({13, -6}, {13, -2});  // would cross the first at (13, 0)
    cmd::FilterFeedback fb;
    CHECK(near(snapped(w, {12, 1}, "apparent", {}, &fb), {13, 0}));
    CHECK(fb.marker == "apparent");
    // A real crossing is the intersection mode's, not apparent.
    CHECK(near(snapped(w, {12, 1}, "intersection"), {12, 1}));
    World v;
    v.line({0, 0}, {20, 0});
    v.line({10, -5}, {10, 5});
    CHECK(near(snapped(v, {11, 1}, "apparent"), {11, 1}));
    CHECK(near(snapped(v, {11, 1}, "intersection"), {10, 0}));
    // A line and a circle: the line is too short to reach it.
    World c;
    c.circle({0, 0}, 20);
    c.line({-30, 0}, {-24, 0});
    CHECK(near(snapped(c, {-21, 1}, "apparent"), {-20, 0}));
    // Parallel lines never meet.
    World p;
    p.line({0, 0}, {10, 0});
    p.line({0, 2}, {10, 2});
    CHECK(near(snapped(p, {5, 1}, "apparent"), {5, 1}));
}

TEST_CASE("apparent intersection points lie on both unbounded lines, also at survey coordinates") {
    std::mt19937_64 rng(17);
    std::uniform_real_distribution<double> u(-1, 1);
    for (const Vec2 origin : {Vec2{0, 0}, Vec2{1.0e6, 2.5e6}, Vec2{-2.0e8, 3.0e8}}) {
        World w;
        const double tol = 1e-7 * (1 + std::abs(origin.x) + std::abs(origin.y)) + 1e-6;
        for (int i = 0; i < 40; ++i) {
            const Vec2 c = origin + Vec2{u(rng) * 50, u(rng) * 50};
            const double a1 = u(rng) * kPi, a2 = a1 + 0.4 + std::abs(u(rng)) * 2.0;
            const Vec2 d1{std::cos(a1), std::sin(a1)}, d2{std::cos(a2), std::sin(a2)};
            // Two short segments pointing at a common point `c` that they stop 3 units short of.
            const Vec2 l1a = c - d1 * 8, l1b = c - d1 * 3;
            const Vec2 l2a = c + d2 * 8, l2b = c + d2 * 3;
            Document& doc = w.doc;
            (void)doc;
            const EntityId a = w.line(l1a, l1b);
            const EntityId b = w.line(l2a, l2b);
            w.set(keys::osnap, "1");
            w.set(keys::osnapModes, "apparent");
            SnapRequest rq;
            rq.cursor = c + Vec2{u(rng), u(rng)};
            rq.apertureMm = 6;
            rq.modes = maskOf(SnapMode::ApparentIntersection);
            const auto found = findSnapCandidates(w.doc, w.idx, rq);
            REQUIRE_FALSE(found.empty());
            bool good = false;
            for (const auto& cand : found) {
                if (cand.entity == a || cand.entity == b || cand.entity2 == a ||
                    cand.entity2 == b) {
                    // On both unbounded lines.
                    const double o1 = std::abs(cross(d1, cand.point - l1a));
                    const double o2 = std::abs(cross(d2, cand.point - l2a));
                    if (o1 < tol && o2 < tol && distance(cand.point, c) < tol)
                        good = true;
                }
            }
            CHECK(good);
            // The lines are removed again so the next pair stands alone.
            auto tx = w.doc.begin("erase");
            (void)tx.erase(a);
            (void)tx.erase(b);
            tx.commit();
        }
    }
}
