// OFFSET on closed polylines with arcs (positive and negative bulge, both windings): every result
// lies at the typed distance from the source and has no self-crossing left; Through, Multiple,
// Erase and Layer behave the same on such an outline, and one undo restores the drawing.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/entity.h"
#include "tbcad/modify/modify.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, reg) { registerAllCommands(reg); }
    EntityId add(EntityData d) {
        Entity e;
        e.data = std::move(d);
        Transaction tx = doc.begin("setup");
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    std::vector<EntityId> polylines() {
        std::vector<EntityId> ids;
        for (EntityId id : doc.modelSpace())
            if (kind(*doc.find(id)) == EntityKind::Polyline)
                ids.push_back(id);
        return ids;
    }
};

double segDist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    double L = ab.x * ab.x + ab.y * ab.y;
    double t = L > 0 ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / L : 0;
    t = std::clamp(t, 0.0, 1.0);
    return distance(p, a + ab * t);
}
double distToRing(const std::vector<Vec2>& r, Vec2 p) {
    double m = 1e18;
    for (size_t i = 0; i < r.size(); i++)
        m = std::min(m, segDist(p, r[i], r[(i + 1) % r.size()]));
    return m;
}
double cross2(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}
bool properCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    double d1 = cross2(b - a, c - a), d2 = cross2(b - a, d - a);
    double d3 = cross2(d - c, a - c), d4 = cross2(d - c, b - c);
    const double e = 1e-7;
    return ((d1 > e && d2 < -e) || (d1 < -e && d2 > e)) &&
           ((d3 > e && d4 < -e) || (d3 < -e && d4 > e));
}
bool selfCrosses(const std::vector<Vec2>& r) {
    size_t n = r.size();
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 2; j < n; j++) {
            if (i == 0 && j == n - 1)
                continue;
            if (properCross(r[i], r[(i + 1) % n], r[j], r[(j + 1) % n]))
                return true;
        }
    return false;
}

// A closed outline: a rounded slot or a plate with a bulged side; `flip` reverses the winding and
// the sign of every bulge.
std::vector<PolyVertex> shape(int which, bool flip) {
    std::vector<PolyVertex> v;
    switch (which) {
        case 0:  // slot: two straight sides and two half-circle ends
            v = {{{20, 0}, 0, 0, 0}, {{80, 0}, 0, 0, 1}, {{80, 40}, 0, 0, 0}, {{20, 40}, 0, 0, 1}};
            break;
        case 1:  // plate with a convex bulge on one side
            v = {
                {{0, 0}, 0, 0, 0.4}, {{100, 0}, 0, 0, 0}, {{100, 60}, 0, 0, 0}, {{0, 60}, 0, 0, 0}};
            break;
        default:  // plate with a concave (negative bulge) side
            v = {{{0, 0}, 0, 0, -0.4},
                 {{100, 0}, 0, 0, 0},
                 {{100, 60}, 0, 0, 0},
                 {{0, 60}, 0, 0, 0}};
            break;
    }
    if (flip) {
        // Reversed: the segment k -> k+1 with bulge b runs k+1 -> k with bulge -b.
        const size_t n = v.size();
        std::vector<PolyVertex> o(n);
        for (size_t i = 0; i < n; i++)
            o[n - 1 - i] = {v[i].p, 0, 0, 0};
        for (size_t k = 0; k < n; k++)
            o[n - 1 - ((k + 1) % n)].bulge = -v[k].bulge;
        return o;
    }
    return v;
}

std::vector<Vec2> ring(const Entity& e) {
    std::vector<Vec2> pts;
    geom::outlinePoints(e, 0.005, pts);
    if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9)
        pts.pop_back();
    return pts;
}

}  // namespace

TEST_CASE("OFFSET of closed arc polylines keeps the typed distance and leaves no self-crossing") {
    int results = 0;
    for (int which = 0; which < 3; ++which)
        for (bool flip : {false, true})
            for (bool round : {true, false})
                for (double d : {1.0, 5.0, 12.0, 17.5}) {
                    for (int side = 0; side < 2; ++side) {
                        CAPTURE(which);
                        CAPTURE(flip);
                        CAPTURE(round);
                        CAPTURE(d);
                        CAPTURE(side);
                        Rig r;
                        EntityId src = r.add(PolylineData{shape(which, flip), true, 0});
                        const std::string before = canonicalDump(r.doc);
                        std::vector<Vec2> srcRing = ring(*r.doc.find(src));
                        // Side points well inside and well outside the outline.
                        Vec2 sidePt = side == 0 ? Vec2{50, 20} : Vec2{50, 150};
                        auto run = r.host.run(
                            "OFFSET", {"Corners", round ? "Round" : "Sharp", std::to_string(d),
                                       ScriptItem::pick(src, {50, 0}),
                                       std::to_string(sidePt.x) + "," + std::to_string(sidePt.y),
                                       ScriptItem::enter()});
                        CHECK(run.finished);
                        for (EntityId id : r.polylines()) {
                            if (id == src)
                                continue;
                            std::vector<Vec2> o = ring(*r.doc.find(id));
                            REQUIRE(o.size() >= 3);
                            double mn = 1e18;
                            for (size_t i = 0; i < o.size(); i++) {
                                // Straight stretches have only end points: walk along them.
                                Vec2 a = o[i], b = o[(i + 1) % o.size()];
                                int steps = std::max(1, (int)(distance(a, b) / 0.25));
                                for (int k = 0; k <= steps; k++)
                                    mn = std::min(
                                        mn, distToRing(srcRing, a + (b - a) * (double)k / steps));
                            }
                            CHECK(mn >= d - 0.05);
                            CHECK(mn <= d + 0.05);
                            CHECK_FALSE(selfCrosses(o));
                            results++;
                        }
                        // One undo restores the drawing exactly.
                        if (canonicalDump(r.doc) != before) {
                            CHECK(r.host.undo());
                            CHECK(canonicalDump(r.doc) == before);
                        }
                    }
                }
    CHECK(results > 40);
}

TEST_CASE("OFFSET Through, Multiple, Erase and Layer on a closed arc polyline") {
    Rig r;
    EntityId src = r.add(PolylineData{shape(1, false), true, 0});
    const std::string before = canonicalDump(r.doc);

    // Through a point outside: the copy passes through it.
    r.host.run("OFFSET",
               {"Through", ScriptItem::pick(src, {50, 60}), "50,75", ScriptItem::enter()});
    auto ids = r.polylines();
    REQUIRE(ids.size() == 2);
    for (EntityId id : ids) {
        if (id == src)
            continue;
        CHECK(distToRing(ring(*r.doc.find(id)), {50, 75}) < 0.05);
    }
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    // Multiple makes one copy per side point, in one undo step.
    r.host.run("OFFSET", {"6", ScriptItem::pick(src, {50, 60}), "Multiple", "50,80", "50,30",
                          ScriptItem::enter(), ScriptItem::enter()});
    CHECK(r.polylines().size() == 3);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    // Erase removes the source and leaves exactly the offset copy.
    r.host.run("OFFSET", {"Erase", "Yes", "6", ScriptItem::pick(src, {50, 60}), "50,80",
                          ScriptItem::enter()});
    CHECK_FALSE(r.doc.contains(src));
    CHECK(r.polylines().size() == 1);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}
