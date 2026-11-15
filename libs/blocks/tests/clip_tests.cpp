#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

const std::vector<Vec2> kBox{{0, 0}, {10, 0}, {10, 10}, {0, 10}};

DisplayList displayOf(const Document& doc, EntityId id) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.1;
    DisplayList l;
    geom::buildDisplay(doc, *doc.find(id), ctx, l);
    return l;
}

}  // namespace

TEST_CASE("clipDisplay cuts lines at the boundary") {
    DisplayList in;
    DisplayPolyline p;
    p.pts = {{-5, 5}, {15, 5}};
    in.items.emplace_back(p);
    DisplayPolyline outside;
    outside.pts = {{20, 0}, {30, 0}};
    in.items.emplace_back(outside);
    DisplayList out = clipDisplay(in, kBox);
    REQUIRE(out.items.size() == 1);
    const auto& q = std::get<DisplayPolyline>(out.items[0]);
    REQUIRE(q.pts.size() == 2);
    CHECK(near(q.pts[0], {0, 5}));
    CHECK(near(q.pts[1], {10, 5}));
}

TEST_CASE("clipDisplay with a concave boundary keeps both visible parts") {
    // A "U": the notch removes the middle of a horizontal line.
    std::vector<Vec2> u{{0, 0}, {30, 0}, {30, 10}, {20, 10}, {20, 4}, {10, 4}, {10, 10}, {0, 10}};
    DisplayList in;
    DisplayPolyline p;
    p.pts = {{-5, 8}, {35, 8}};
    in.items.emplace_back(p);
    DisplayList out = clipDisplay(in, u);
    REQUIRE(out.items.size() == 2);
    CHECK(near(std::get<DisplayPolyline>(out.items[0]).pts.front(), {0, 8}));
    CHECK(near(std::get<DisplayPolyline>(out.items[0]).pts.back(), {10, 8}));
    CHECK(near(std::get<DisplayPolyline>(out.items[1]).pts.front(), {20, 8}));
}

TEST_CASE("clipDisplay clips fills and points") {
    DisplayList in;
    DisplayFill f;
    f.rings.push_back({{-5, -5}, {5, -5}, {5, 5}, {-5, 5}});
    in.items.emplace_back(f);
    DisplayPoints pts;
    pts.pts = {{1, 1}, {50, 50}};
    in.items.emplace_back(pts);
    DisplayList out = clipDisplay(in, kBox);
    REQUIRE(out.items.size() == 2);
    const auto& ring = std::get<DisplayFill>(out.items[0]).rings.at(0);
    BBox2 b;
    for (const Vec2& v : ring)
        b.expand(v);
    CHECK(near(b.min, {0, 0}));
    CHECK(near(b.max, {5, 5}));
    CHECK(std::get<DisplayPoints>(out.items[1]).pts.size() == 1);
}

namespace {
double fillArea(const DisplayList& l) {
    double a = 0;
    for (const auto& it : l.items)
        if (const auto* f = std::get_if<DisplayFill>(&it))
            for (const auto& r : f->rings) {
                double s = 0;
                for (std::size_t i = 0; i < r.size(); ++i)
                    s += cross(r[i], r[(i + 1) % r.size()]);
                a += std::abs(s) / 2;
            }
    return a;
}
}  // namespace

TEST_CASE("clipDisplay cuts a fill exactly with a concave boundary, inside and outside") {
    // A "U" 30 x 10 with a 10 x 6 notch (area 240), over a fill covering 40 x 20 (area 800).
    std::vector<Vec2> u{{0, 0}, {30, 0}, {30, 10}, {20, 10}, {20, 4}, {10, 4}, {10, 10}, {0, 10}};
    DisplayList in;
    DisplayFill f;
    f.rings.push_back({{-5, -5}, {35, -5}, {35, 15}, {-5, 15}});
    in.items.emplace_back(f);
    CHECK(fillArea(clipDisplay(in, u)) == doctest::Approx(240));
    CHECK(fillArea(clipDisplay(in, u, true)) == doctest::Approx(800 - 240));
    // Clockwise boundary: the same.
    std::vector<Vec2> cw(u.rbegin(), u.rend());
    CHECK(fillArea(clipDisplay(in, cw)) == doctest::Approx(240));
    CHECK(fillArea(clipDisplay(in, cw, true)) == doctest::Approx(800 - 240));
}

TEST_CASE("clipDisplay keeps a fill the boundary does not reach whole, glyph and all") {
    std::vector<Vec2> u{{0, 0}, {30, 0}, {30, 10}, {20, 10}, {20, 4}, {10, 4}, {10, 10}, {0, 10}};
    DisplayList in;
    DisplayFill f;
    f.rings.push_back({{1, 1}, {3, 1}, {3, 3}, {1, 3}});
    f.glyphRef = GlyphRef{.glyphId = 7};
    in.items.emplace_back(f);
    DisplayFill notch;  // inside the notch: outside the boundary
    notch.rings.push_back({{12, 6}, {14, 6}, {14, 8}, {12, 8}});
    in.items.emplace_back(notch);
    DisplayList out = clipDisplay(in, u);
    REQUIRE(out.items.size() == 1);
    CHECK(std::get<DisplayFill>(out.items[0]).glyphRef);
    DisplayList inv = clipDisplay(in, u, true);
    REQUIRE(inv.items.size() == 1);
    CHECK(fillArea(inv) == doctest::Approx(4));
}

TEST_CASE("an insert with a clipping boundary shows only the inside, in the insert's frame") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "B", {line(-10, 5, 20, 5)}, {0, 0});
    Entity e = insertOf(b, {100, 0}, {2, 2}, 0);
    auto* d = entityAs<InsertData>(e);
    d->clip = kBox;
    EntityId i = addEntity(doc, e);
    DisplayList l = displayOf(doc, i);
    REQUIRE(l.items.size() == 2);  // the cut line and the boundary (shown, not plotted)
    const auto& p = std::get<DisplayPolyline>(l.items[0]);
    const auto& frame = std::get<DisplayPolyline>(l.items[1]);
    CHECK(frame.closed);
    CHECK(frame.style.noPlot);
    CHECK(near(p.pts.front(), {100, 10}));
    CHECK(near(p.pts.back(), {120, 10}));
    // Switched off, the whole block shows again.
    Transaction tx = doc.begin("off");
    REQUIRE(tx.modify(i, [](Entity& en) { entityAs<InsertData>(en)->clipOn = false; }).ok());
    tx.commit();
    const DisplayList all = displayOf(doc, i);
    const auto& full = std::get<DisplayPolyline>(all.items.at(0));
    CHECK(near(full.pts.front(), {80, 10}));
    CHECK(near(full.pts.back(), {140, 10}));
}

TEST_CASE("insert bounds are limited to the clipping boundary") {
    registerInsertDisplay();
    Document doc;
    BlockId b = addBlock(doc, "B", {line(-10, 5, 20, 5)}, {0, 0});
    Entity e = insertOf(b, {100, 0});
    entityAs<InsertData>(e)->clip = kBox;
    EntityId i = addEntity(doc, e);
    BBox2 box = insertBounds(doc, *doc.find(i));
    CHECK(near(box.min, {100, 5}));
    CHECK(near(box.max, {110, 5}));
}

TEST_CASE("clipDisplay with invert keeps the outside: lines, fills (area adds up) and points") {
    DisplayList in;
    DisplayPolyline p;
    p.pts = {{-5, 5}, {15, 5}};
    in.items.emplace_back(p);
    DisplayList out = clipDisplay(in, kBox, true);
    REQUIRE(out.items.size() == 2);  // the parts left of and right of the box
    CHECK(near(std::get<DisplayPolyline>(out.items[0]).pts.front(), {-5, 5}));
    CHECK(near(std::get<DisplayPolyline>(out.items[0]).pts.back(), {0, 5}));
    CHECK(near(std::get<DisplayPolyline>(out.items[1]).pts.front(), {10, 5}));

    DisplayList fills;
    DisplayFill f;
    f.rings.push_back({{-5, -5}, {15, -5}, {15, 15}, {-5, 15}});  // 20 x 20
    fills.items.emplace_back(f);
    DisplayPoints pts;
    pts.pts = {{1, 1}, {50, 50}};
    fills.items.emplace_back(pts);
    DisplayList o2 = clipDisplay(fills, kBox, true);
    double area = 0;
    for (const auto& ring : std::get<DisplayFill>(o2.items[0]).rings) {
        double s = 0;
        for (std::size_t i = 0; i < ring.size(); ++i)
            s += cross(ring[i], ring[(i + 1) % ring.size()]);
        area += std::abs(s) / 2;
    }
    CHECK(std::abs(area - 300.0) < 1e-6);  // the square minus the 10 x 10 box
    CHECK(std::get<DisplayPoints>(o2.items[1]).pts.size() == 1);
    CHECK(near(std::get<DisplayPoints>(o2.items[1]).pts[0], {50, 50}));
}
