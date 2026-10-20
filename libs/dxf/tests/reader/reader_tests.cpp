// Unit tests of the DXF reader: geometry mapping, OCS, units, tables, blocks.
#include <algorithm>
#include <cmath>

#include "reader_helpers.h"
#include "tbcad/blocks/array.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/geom/entity.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::dxf {
namespace {

using namespace testing;

constexpr double kDeg = kPi / 180.0;

bool hasEndpoint(const std::vector<Vec2>& pts, Vec2 p) {
    return std::any_of(pts.begin(), pts.end(), [&](Vec2 q) { return near(p, q, 1e-9); });
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// OCS / extrusion (0, 0, -1)

TEST_CASE("extrusion -Z: an arc lands at the mirrored position") {
    Loaded l = loadCorpus("extrusion_neg_z.dxf");
    auto arcs = modelEntities<ArcData>(l.doc());
    REQUIRE(arcs.size() == 1);
    const ArcData& a = *arcs[0];
    // The DXF arc: center (10, 0), radius 5, 30..120 degrees in an OCS whose x axis is -X.
    // World end points are therefore (-(10 + 5 cos t), 5 sin t) for t = 30 and 120 degrees.
    CHECK(near(a.center, Vec2{-10, 0}));
    CHECK(near(a.radius, 5));
    Vec2 p30{-(10 + 5 * std::cos(30 * kDeg)), 5 * std::sin(30 * kDeg)};
    Vec2 p120{-(10 + 5 * std::cos(120 * kDeg)), 5 * std::sin(120 * kDeg)};
    Entity e;
    e.data = a;
    std::vector<Vec2> ends = geom::endpoints(e);
    REQUIRE(ends.size() == 2);
    CHECK(hasEndpoint(ends, p30));
    CHECK(hasEndpoint(ends, p120));
    // Counter-clockwise from start to end: 60 .. 150 degrees about the new center.
    CHECK(near(a.startAngle, 60 * kDeg));
    CHECK(near(a.endAngle, 150 * kDeg));
}

TEST_CASE("extrusion -Z: circle, polyline, text, insert, ellipse and hatch") {
    Loaded l = loadCorpus("extrusion_neg_z.dxf");
    const Document& d = l.doc();

    auto circles = modelEntities<CircleData>(d);
    REQUIRE(circles.size() == 1);
    CHECK(near(circles[0]->center, Vec2{-3, 4}));

    auto polys = modelEntities<PolylineData>(d);
    REQUIRE(polys.size() == 1);
    REQUIRE(polys[0]->v.size() == 3);
    CHECK(near(polys[0]->v[0].p, Vec2{-1, 0}));
    CHECK(near(polys[0]->v[1].p, Vec2{-2, 0}));
    CHECK(near(polys[0]->v[1].bulge, -1));  // a mirror reverses the direction of the arc
    CHECK(near(polys[0]->v[2].p, Vec2{-2, 3}));

    auto texts = modelEntities<TextData>(d);
    REQUIRE(texts.size() == 1);
    CHECK(near(texts[0]->position, Vec2{-5, 5}));
    CHECK(near(normalizeAngle(texts[0]->rotation), 150 * kDeg));

    auto inserts = modelEntities<InsertData>(d);
    REQUIRE(inserts.size() == 1);
    CHECK(near(inserts[0]->position, Vec2{-7, 1}));
    CHECK(near(inserts[0]->scale, Vec2{-1, 1}));
    CHECK(near(inserts[0]->rotation, -90 * kDeg));

    auto ellipses = modelEntities<EllipseData>(d);
    REQUIRE(ellipses.size() == 2);  // the second is the arc in a tilted plane
    if (std::abs(ellipses[0]->majorAxis.x) < 2)
        std::swap(ellipses[0], ellipses[1]);
    // DXF parameters 0..90 degrees run clockwise seen from +Z: world 270..360 degrees.
    CHECK(near(ellipses[0]->startParam, 1.5 * kPi));
    CHECK(near(ellipses[0]->endParam, kTwoPi));
    CHECK(near(ellipses[0]->majorAxis, Vec2{4, 0}));

    auto hatches = modelEntities<HatchData>(d);
    REQUIRE(hatches.size() == 1);
    const auto* edge = std::get_if<HatchPolylineEdge>(&hatches[0]->loops[0].edges[0]);
    REQUIRE(edge != nullptr);
    CHECK(near(edge->v[1].p, Vec2{-4, 10}));
}

static Vec2 pointAt(const EllipseData& e, double t) {
    const Vec2 minor{-e.majorAxis.y * e.ratio, e.majorAxis.x * e.ratio};
    return e.center + e.majorAxis * std::cos(t) + minor * std::sin(t);
}

TEST_CASE("extrusion: an arc in a steeply tilted plane is drawn as the ellipse seen from above") {
    // Normal (0.5, 0, 0.866): object X is world +Y, object Y is (-0.866, 0, 0.5).
    Loaded l = loadCorpus("extrusion_neg_z.dxf");
    CHECK(l.report.countCode(code::kExtrusion3d) == 1);  // one note, not a skip
    const EllipseData* arc = nullptr;
    for (const EllipseData* e : modelEntities<EllipseData>(l.doc()))
        if (std::abs(e->ratio - 0.8660254) < 1e-6)
            arc = e;
    REQUIRE(arc);
    CHECK(near(arc->center, Vec2{0, 0}));
    CHECK(near(pointAt(*arc, arc->startParam), Vec2{0, 1}));
    CHECK(near(pointAt(*arc, arc->endParam), Vec2{-0.8660254, 0}, 1e-6));
}

TEST_CASE("extrusion: steeply tilted circles, ellipses and polylines are projected, not skipped") {
    Pairs p;
    p.section("ENTITIES");
    // A circle in a vertical plane (normal +X) is seen edge-on: a straight piece along Y.
    p.g(0, "CIRCLE").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(40, 5.0);
    p.g(210, 1.0).g(220, 0.0).g(230, 0.0);
    // A polyline with an arc in a plane tilted 60 degrees about the X axis.
    p.g(0, "LWPOLYLINE").g(8, "0").g(90, 2).g(70, 0).g(10, 0.0).g(20, 0.0).g(42, 1.0);
    p.g(10, 10.0).g(20, 0.0).g(210, 0.0).g(220, -0.8660254).g(230, 0.5);
    // An ellipse in the same plane: major axis along world X stays, the minor one shrinks.
    p.g(0, "ELLIPSE").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(11, 6.0).g(21, 0.0).g(31, 0.0).g(210, 0.0).g(220, -0.8660254).g(230, 0.5);
    p.g(40, 0.5).g(41, 0.0).g(42, kTwoPi);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    CHECK(l.report.countCode(code::kExtrusion3d) == 1);
    const auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 2);
    const PolylineData* edge = polys[0]->v.size() == 65 ? polys[0] : polys[1];
    const PolylineData* bent = edge == polys[0] ? polys[1] : polys[0];
    REQUIRE(edge->v.size() == 65);
    for (const PolyVertex& v : edge->v)
        CHECK(std::abs(v.p.x) < 1e-9);  // object X is world +Y, object Y is world Z
    // Object X is world +X; object Y is (0, 0.5, 0.866), so y is halved when seen from above.
    CHECK(near(bent->v.front().p, Vec2{0, 0}));
    CHECK(near(bent->v.back().p, Vec2{10, 0}));
    CHECK(bent->v.size() > 10);  // the half circle became short segments
    double lowest = 0;
    for (const PolyVertex& v : bent->v)
        lowest = std::min(lowest, v.p.y);
    CHECK(lowest == doctest::Approx(-2.5).epsilon(0.01));  // a half circle of radius 5, halved
    const auto ellipses = modelEntities<EllipseData>(l.doc());
    REQUIRE(ellipses.size() == 1);
    CHECK(near(ellipses[0]->majorAxis, Vec2{6, 0}));
    CHECK(ellipses[0]->ratio == doctest::Approx(0.25));
}

TEST_CASE("extrusion: the tilted plane corpus counts what was projected and what was dropped") {
    Loaded l = loadCorpus("tilted_planes.dxf");
    int infos = 0;
    for (const Diagnostic& d : l.report.diagnostics()) {
        if (d.code != code::kExtrusion3d)
            continue;
        CHECK(d.severity == Severity::Info);
        ++infos;
        // 2 circles, arc, ellipse, 2 polylines, hatch, solid and the text.
        CHECK(d.message.rfind("9 ", 0) == 0);
    }
    CHECK(infos == 1);
    CHECK(l.report.countCode(code::kBadEntity) == 1);  // the one-vertex polyline
}

TEST_CASE("extrusion: text in a tilted plane keeps its exact shape seen from above") {
    // Normal (0, -0.6, 0.8): object X is world +X, object Y is (0, 0.8, 0.6).
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "TEXT").g(8, "0").g(10, 5.0).g(20, 5.0).g(30, 0.0).g(40, 2.0).g(1, "flat");
    p.g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "TEXT").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(40, 2.0).g(50, 90.0);
    p.g(1, "up").g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "MTEXT").g(8, "0").g(10, 1.0).g(20, 2.0).g(30, 3.0).g(40, 2.0).g(41, 10.0);
    p.g(1, "m").g(210, 0.0).g(220, -0.6).g(230, 0.8);
    // Edge-on: the baseline runs straight down the view direction.
    p.g(0, "TEXT").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(40, 2.0).g(1, "edge");
    p.g(210, 0.0).g(220, -1.0).g(230, 0.0).g(50, 90.0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto texts = modelEntities<TextData>(l.doc());
    REQUIRE(texts.size() == 2);
    const TextData* flat = texts[0]->utf8 == "flat" ? texts[0] : texts[1];
    const TextData* up = flat == texts[0] ? texts[1] : texts[0];
    CHECK(near(flat->position, Vec2{5, 4}, 1e-9));
    CHECK(near(flat->rotation, 0.0));
    CHECK(flat->height == doctest::Approx(1.6));        // its up direction is foreshortened
    CHECK(flat->widthFactor == doctest::Approx(1.25));  // the baseline is not
    CHECK(near(flat->oblique, 0.0));
    CHECK(near(up->rotation, kPi / 2));
    CHECK(up->height == doctest::Approx(2));
    CHECK(up->widthFactor == doctest::Approx(0.8));
    const auto mtexts = modelEntities<MTextData>(l.doc());
    REQUIRE(mtexts.size() == 1);
    CHECK(near(mtexts[0]->position, Vec2{1, 2}));  // MTEXT points are in world coordinates
    CHECK(mtexts[0]->height == doctest::Approx(1.6));
    CHECK(mtexts[0]->width == doctest::Approx(10));
    CHECK(l.report.countCode(code::kBadEntity) == 1);  // the edge-on text
}

TEST_CASE("extrusion: hatches, solids and 2D polylines in tilted planes are projected") {
    // Normal (0, -0.866, 0.5): object X is world +X, object Y is (0, 0.5, 0.866).
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "SOLID").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(11, 10.0).g(21, 0.0);
    p.g(12, 0.0).g(22, 10.0).g(13, 10.0).g(23, 10.0);
    p.g(210, 0.0).g(220, -0.8660254).g(230, 0.5);
    // A hatch bounded by a full circle of radius 5 (one arc edge).
    p.g(0, "HATCH").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(210, 0.0).g(220, -0.8660254);
    p.g(230, 0.5).g(2, "SOLID").g(70, 1).g(71, 0).g(91, 1).g(92, 1).g(93, 1);
    p.g(72, 2).g(10, 0.0).g(20, 0.0).g(40, 5.0).g(50, 0.0).g(51, 360.0).g(73, 1);
    p.g(97, 0).g(75, 0).g(76, 1).g(98, 0);
    // An old style 2D polyline with a half circle bulge, at elevation 4.
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(10, 0.0).g(20, 0.0).g(30, 4.0).g(70, 0);
    p.g(210, 0.0).g(220, -0.8660254).g(230, 0.5);
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(42, 1.0);
    p.g(0, "VERTEX").g(8, "0").g(10, 10.0).g(20, 0.0).g(0, "SEQEND").g(8, "0");
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    CHECK(l.report.countCode(code::kExtrusion3d) == 1);  // the one info line
    const auto hatches = modelEntities<HatchData>(l.doc());
    REQUIRE(hatches.size() == 2);
    const HatchData* solid = hatches[0]->loops[0].edges.size() == 1 ? hatches[0] : hatches[1];
    const HatchData* round = solid == hatches[0] ? hatches[1] : hatches[0];
    const auto* quad = std::get_if<HatchPolylineEdge>(&solid->loops[0].edges[0]);
    REQUIRE(quad);
    REQUIRE(quad->v.size() == 4);
    CHECK(near(quad->v[2].p, Vec2{10, 5}, 1e-6));
    CHECK(near(quad->v[3].p, Vec2{0, 5}, 1e-6));
    double top = -1e9, left = 1e9;
    for (const HatchEdge& e : round->loops[0].edges) {
        const auto* line = std::get_if<HatchLineEdge>(&e);
        REQUIRE(line);
        top = std::max(top, line->a.y);
        left = std::min(left, line->a.x);
    }
    CHECK(top == doctest::Approx(2.5));
    CHECK(left == doctest::Approx(-5));
    const auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 1);
    // Elevation 4 along the normal moves everything by (0, -3.464).
    CHECK(near(polys[0]->v.front().p, Vec2{0, -3.4641016}, 1e-6));
    CHECK(near(polys[0]->v.back().p, Vec2{10, -3.4641016}, 1e-6));
    CHECK(polys[0]->v.size() > 10);
}

TEST_CASE("extrusion: inserts in a tilted plane become squashed blocks seen from above") {
    // Normal (0, -0.6, 0.8): object X is world +X, object Y is (0, 0.8, 0.6).
    Pairs p;
    p.section("BLOCKS").g(0, "BLOCK").g(2, "B").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 0.0).g(0, "ENDBLK").end();
    p.section("ENTITIES");
    p.g(0, "INSERT").g(8, "0").g(2, "B").g(10, 2.0).g(20, 3.0).g(30, 0.0);
    p.g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "INSERT").g(8, "0").g(2, "B").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(50, 90.0);
    p.g(41, 2.0).g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "INSERT").g(8, "0").g(2, "B").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(50, 45.0);
    p.g(210, 0.0).g(220, -0.6).g(230, 0.8);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto inserts = modelEntities<InsertData>(l.doc());
    REQUIRE(inserts.size() == 3);
    CHECK(near(inserts[0]->position, Vec2{2, 2.4}, 1e-9));
    CHECK(near(inserts[0]->rotation, 0.0));
    CHECK(near(inserts[0]->scale, Vec2{1, 0.8}, 1e-9));
    // Turned 90 degrees: the block's X runs along object Y, which is foreshortened.
    CHECK(near(inserts[1]->rotation, kPi / 2));
    CHECK(near(inserts[1]->scale, Vec2{1.6, 1}, 1e-9));
    // Turned 45 degrees: a shear, split into a block turned inside a rotated, scaled insert.
    const InsertData& outer = *inserts[2];
    const BlockDef* inner = l.doc().block(outer.block);
    REQUIRE(inner);
    CHECK(inner->name.rfind("B$TILT", 0) == 0);
    const auto nested = blockEntities<InsertData>(l.doc(), outer.block);
    REQUIRE(nested.size() == 1);
    auto place = [](const InsertData& in, Vec2 p) {
        p = Vec2{p.x * in.scale.x, p.y * in.scale.y};
        const double c = std::cos(in.rotation), s = std::sin(in.rotation);
        return in.position + Vec2{p.x * c - p.y * s, p.x * s + p.y * c};
    };
    // Object (cos 45, sin 45) seen from above: y shrinks by 0.8.
    CHECK(near(place(outer, place(*nested[0], Vec2{1, 0})), Vec2{0.7071068, 0.5656854}, 1e-6));
    CHECK(near(place(outer, place(*nested[0], Vec2{0, 1})), Vec2{-0.7071068, 0.5656854}, 1e-6));
    CHECK(l.report.countCode(code::kExtrusion3d) == 1);  // the info line only
}

TEST_CASE("polyface and polygon meshes are drawn as their visible edges seen from above") {
    Pairs p;
    p.section("ENTITIES");
    // A polyface: a square of 4 vertices split into two triangles. The diagonal they share is
    // hidden (negative index) in one face and visible in the other: drawn once.
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 64);
    p.g(71, 4).g(72, 2);
    for (const auto& [x, y] : {std::pair{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}})
        p.g(0, "VERTEX").g(8, "0").g(10, x).g(20, y).g(30, 5.0).g(70, 192);
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 128);
    p.g(71, 1).g(72, 2).g(73, 3);
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 128);
    p.g(71, -3).g(72, 4).g(73, -1);  // edges 3-4 hidden, 4-1 visible, 1-3 hidden
    p.g(0, "SEQEND").g(8, "0");
    // A 2 x 3 polygon mesh: 2 rows of 3 points and 3 columns of 2.
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 16);
    p.g(71, 2).g(72, 3);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            p.g(0, "VERTEX")
                .g(8, "0")
                .g(10, 100.0 + j)
                .g(20, static_cast<double>(i))
                .g(30, 1.0)
                .g(70, 64);
    p.g(0, "SEQEND").g(8, "0");
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto lines = modelEntities<LineData>(l.doc());
    CHECK(lines.size() == 4);  // 1-2, 2-3, 3-1 (the diagonal) and 4-1
    bool diagonal = false, hidden = false;
    for (const LineData* ln : lines) {
        const Vec2 lo = ln->a.x + ln->a.y < ln->b.x + ln->b.y ? ln->a : ln->b;
        const Vec2 hi = lo == ln->a ? ln->b : ln->a;
        diagonal = diagonal || (near(lo, Vec2{0, 0}) && near(hi, Vec2{10, 10}));
        hidden = hidden || (near(lo, Vec2{0, 10}) && near(hi, Vec2{10, 10}));
    }
    CHECK(diagonal);
    CHECK_FALSE(hidden);
    const auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 5);
    CHECK(polys[0]->v.size() == 3);
    CHECK(near(polys[1]->v.back().p, Vec2{102, 1}));
    CHECK(polys[2]->v.size() == 2);
    CHECK(l.report.countCode(code::kBadEntity) == 0);
}

TEST_CASE("a MESH entity is kept for saving and drawn from its cage edges") {
    Pairs p;
    p.section("ENTITIES");
    // A square pyramid without edge records: 5 vertices, a quad base and 4 triangles.
    p.g(0, "MESH").g(8, "0").g(100, "AcDbEntity").g(100, "AcDbSubDMesh").g(71, 2).g(72, 0);
    p.g(91, 0).g(92, 5);
    for (const auto& [x, y, z] : {std::tuple{0.0, 0.0, 0.0},
                                  {10.0, 0.0, 0.0},
                                  {10.0, 10.0, 0.0},
                                  {0.0, 10.0, 0.0},
                                  {5.0, 5.0, 8.0}})
        p.g(10, x).g(20, y).g(30, z);
    p.g(93, 21).g(90, 4).g(90, 0).g(90, 1).g(90, 2).g(90, 3);
    for (int i = 0; i < 4; ++i)
        p.g(90, 3).g(90, i).g(90, (i + 1) % 4).g(90, 4);
    p.g(94, 0).g(95, 0).g(90, 0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto proxies = modelEntities<ProxyData>(l.doc());
    REQUIRE(proxies.size() == 1);
    CHECK(proxies[0]->originalType == "MESH");
    CHECK_FALSE(proxyDxfGroups(*proxies[0]).empty());  // written back unchanged
    CHECK(proxies[0]->graphics.items.size() == 8);     // 4 base edges and 4 to the apex
}

TEST_CASE("an OLE2FRAME is kept for saving and shows its frame") {
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "OLE2FRAME").g(8, "0").g(100, "AcDbEntity").g(100, "AcDbOle2Frame").g(70, 2);
    p.g(3, "Microsoft Excel Worksheet").g(10, 0.0).g(20, 50.0).g(30, 0.0).g(11, 80.0).g(21, 0.0);
    p.g(31, 0.0).g(71, 2).g(72, 0).g(90, 4).g(310, "00000000").g(1, "OLE");
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto proxies = modelEntities<ProxyData>(l.doc());
    REQUIRE(proxies.size() == 1);
    CHECK(proxies[0]->originalType == "OLE2FRAME");
    CHECK_FALSE(proxies[0]->graphics.items.empty());
    CHECK_FALSE(proxyDxfGroups(*proxies[0]).empty());
}

TEST_CASE("a spline with too few control points for its degree is drawn with a lower degree") {
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "SPLINE").g(8, "0").g(70, 8).g(71, 3).g(72, 8).g(73, 3);
    for (int i = 0; i < 8; ++i)
        p.g(40, i < 4 ? 0.0 : 1.0);
    p.g(10, 0.0).g(20, 0.0).g(30, 0.0).g(10, 5.0).g(20, 5.0).g(30, 0.0).g(10, 10.0).g(20, 0.0);
    p.g(30, 0.0);
    p.g(0, "SPLINE").g(8, "0").g(70, 8).g(71, 3).g(72, 0).g(73, 1).g(10, 1.0).g(20, 1.0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto splines = modelEntities<SplineData>(l.doc());
    REQUIRE(splines.size() == 1);
    CHECK(splines[0]->degree == 2);
    CHECK(splines[0]->control.size() == 3);
    CHECK(l.report.countCode(code::kBadEntity) == 2);  // one lowered, one dropped
}

TEST_CASE("extrusion: a loose attribute of an insert in a tilted plane is projected") {
    Pairs p;
    p.section("BLOCKS").g(0, "BLOCK").g(2, "B").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 0.0).g(0, "ENDBLK").end();
    p.section("ENTITIES");
    p.g(0, "INSERT").g(8, "0").g(66, 1).g(2, "B").g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "ATTRIB").g(8, "0").g(10, 5.0).g(20, 5.0).g(30, 0.0).g(40, 2.0).g(1, "v").g(2, "NOTE");
    p.g(70, 0).g(210, 0.0).g(220, -0.6).g(230, 0.8);
    p.g(0, "SEQEND").g(8, "0");
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto texts = modelEntities<TextData>(l.doc());
    REQUIRE(texts.size() == 1);
    CHECK(near(texts[0]->position, Vec2{5, 4}, 1e-9));
    CHECK(texts[0]->height == doctest::Approx(1.6));
    CHECK(l.report.countCode(code::kBadEntity) == 0);
}

TEST_CASE("extrusion: a pattern hatch in a tilted plane keeps its angle seen from above") {
    // Normal (0, -0.6, 0.8): object X is world +X, object Y is (0, 0.8, 0.6).
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "HATCH").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(210, 0.0).g(220, -0.6);
    p.g(230, 0.8).g(2, "ANSI31").g(70, 0).g(71, 0).g(91, 1).g(92, 2).g(72, 0).g(73, 1).g(93, 4);
    p.g(10, 0.0).g(20, 0.0).g(10, 10.0).g(20, 0.0).g(10, 10.0).g(20, 10.0).g(10, 0.0).g(20, 10.0);
    p.g(97, 0).g(75, 0).g(76, 1).g(52, 90.0).g(41, 4.0).g(77, 0).g(78, 0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    const auto hatches = modelEntities<HatchData>(l.doc());
    REQUIRE(hatches.size() == 1);
    CHECK(near(hatches[0]->patternAngle, kPi / 2));  // object Y still points up
    CHECK(hatches[0]->patternScale == doctest::Approx(4 * std::sqrt(0.8)));
}

TEST_CASE("extrusion: a slightly tilted plane is drawn as seen from above") {
    // A normal about 4 degrees off the Z axis (real drawings carry such planes): the arbitrary axis
    // algorithm turns the object x axis to -X, so object (10, 5) lies at world (-10, -5).
    Pairs p;
    p.section("ENTITIES").g(0, "CIRCLE").g(8, "0").g(10, 10.0).g(20, 5.0).g(40, 2.0);
    p.g(210, 0.0).g(220, 0.0735).g(230, 0.99730);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    CHECK(l.report.countCode(code::kExtrusion3d) == 0);
    auto circles = modelEntities<CircleData>(l.doc());
    REQUIRE(circles.size() == 1);
    CHECK(near(circles[0]->center, Vec2{-10, -5}));
}

TEST_CASE("extrusion -Z: INSERT array positions are mirrored before and after rotation") {
    Pairs p;
    p.section("BLOCKS").g(0, "BLOCK").g(2, "B").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 0.0).g(0, "ENDBLK").end();
    p.section("ENTITIES").g(0, "INSERT").g(8, "0").g(2, "B").g(10, 1.0).g(20, 2.0).g(50, 90.0);
    p.g(70, 2).g(71, 1).g(44, 5.0).g(45, 0.0).g(210, 0.0).g(220, 0.0).g(230, -1.0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    auto inserts = modelEntities<InsertData>(l.doc());
    REQUIRE(inserts.size() == 1);
    REQUIRE(inserts[0]->array);
    // Column offset (5, 0) rotated by 90 degrees is (0, 5); the whole thing is mirrored in x.
    const InsertData& g = *inserts[0];
    std::vector<Vec2> pos;
    for (const Transform2& t : blocks::arrayItemTransforms(*g.array)) {
        const Vec2 o = t.apply({0, 0});
        const Vec2 s{o.x * g.scale.x, o.y * g.scale.y};
        const double c = std::cos(g.rotation), sn = std::sin(g.rotation);
        pos.push_back(g.position + Vec2{s.x * c - s.y * sn, s.x * sn + s.y * c});
    }
    REQUIRE(pos.size() == 2);
    CHECK(near(pos[0], Vec2{-1, 2}));
    CHECK(near(pos[1], Vec2{-1, 7}));
}

// ---------------------------------------------------------------------------------------------
// Units, colours, line weights

TEST_CASE("units: inches are converted to millimetres and reported") {
    Loaded l = loadCorpus("units_inches.dxf");
    CHECK(l.result.summary.unitScale == doctest::Approx(25.4));
    CHECK(l.result.summary.unitsName == "inches");
    CHECK(l.report.countCode(code::kUnitsScaled) == 1);
    auto lines = modelEntities<LineData>(l.doc());
    REQUIRE(lines.size() == 1);
    CHECK(near(lines[0]->b, Vec2{25.4, 50.8}));
    auto circles = modelEntities<CircleData>(l.doc());
    CHECK(near(circles[0]->radius, 12.7));
    auto texts = modelEntities<TextData>(l.doc());
    CHECK(near(texts[0]->height, 2.54));
    CHECK(l.doc().header().units == LengthUnit::Inch);  // the drawing keeps showing inches
}

TEST_CASE("units: a file without $INSUNITS is taken as millimetres and says so") {
    Loaded l = load(entitiesOnly(
        Pairs().g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 3.0).g(21, 4.0).text));
    CHECK(l.result.summary.unitsAssumed);
    CHECK(l.result.summary.unitScale == 1.0);
    CHECK(l.report.countCode(code::kUnitsAssumed) == 1);
    CHECK(l.doc().header().units == LengthUnit::Millimeter);
}

TEST_CASE("units: the --units override wins over the file") {
    ImportOptions opt;
    opt.insunits = 5;  // centimetres
    Loaded l =
        load(entitiesOnly(
                 Pairs().g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 3.0).g(21, 4.0).text),
             opt);
    CHECK(l.result.summary.unitScale == doctest::Approx(10.0));
    CHECK(near(modelEntities<LineData>(l.doc())[0]->b, Vec2{30, 40}));
    CHECK(l.doc().header().units == LengthUnit::Centimeter);
}

TEST_CASE("colours: ACI 1-7 keep their meaning, true colour and ByLayer/ByBlock map") {
    Pairs p;
    for (int aci : {1, 3, 7, 256, 0})
        p.g(0, "POINT").g(8, "0").g(62, aci).g(10, 0.0).g(20, 0.0);
    p.g(0, "POINT").g(8, "0").g(420, 0xFF8040).g(10, 0.0).g(20, 0.0);
    p.g(0, "POINT").g(8, "0").g(62, 100).g(10, 0.0).g(20, 0.0);
    Loaded l = load(entitiesOnly(p.text));
    std::vector<Color> colors;
    for (EntityId id : l.doc().modelSpace())
        colors.push_back(l.doc().find(id)->common.color);
    REQUIRE(colors.size() == 7);
    CHECK(colors[0] == Color::index(1));
    CHECK(colors[1] == Color::index(3));
    CHECK(colors[2] == Color::index(7));
    CHECK(colors[3] == Color::byLayer());
    CHECK(colors[4] == Color::byBlock());
    CHECK(colors[5] == Color::rgb(0xFF, 0x80, 0x40));
    CHECK(colors[6].kind == ColorKind::Index);
    CHECK(colors[6].indexValue() >= 10);  // a hue entry, found by nearest colour
}

TEST_CASE("line weights: values map directly, negative values to the symbolic weights") {
    Pairs p;
    for (int lw : {25, 0, -1, -2, -3})
        p.g(0, "POINT").g(8, "0").g(370, lw).g(10, 0.0).g(20, 0.0);
    Loaded l = load(entitiesOnly(p.text));
    std::vector<LineWeight> w;
    for (EntityId id : l.doc().modelSpace())
        w.push_back(l.doc().find(id)->common.lineweight);
    REQUIRE(w.size() == 5);
    CHECK(w[0] == LineWeight::hundredths(25));
    CHECK(w[1] == LineWeight::hundredths(0));
    CHECK(w[2] == LineWeight::byLayer());
    CHECK(w[3] == LineWeight::byBlock());
    CHECK(w[4] == LineWeight::defaultWeight());
}

// ---------------------------------------------------------------------------------------------
// Tables and layers

TEST_CASE("layers: colour, lineweight, linetype and the on/frozen/locked/plot switches") {
    Loaded l = loadCorpus("r2004_layers.dxf");
    const Document& d = l.doc();
    auto get = [&](const char* name) {
        const Layer* layer = d.layerByName(name);
        REQUIRE_MESSAGE(layer != nullptr, name);
        return layer;
    };
    const Layer* walls = get("WALLS");
    CHECK(walls->color == Color::index(1));
    CHECK(walls->lineweight == LineWeight::hundredths(50));
    REQUIRE(d.linetype(walls->linetype) != nullptr);
    CHECK(d.linetype(walls->linetype)->name == "DASHED");
    CHECK_FALSE(get("HIDDEN")->on);
    CHECK(get("FROZEN")->frozen);
    CHECK(get("LOCKED")->locked);
    CHECK_FALSE(get("NOPLOT")->plot);
    CHECK(get("TRUE")->color == Color::rgb(0xFF, 0x80, 0x00));
    // A hidden layer is invisible through the effective state.
    CHECK_FALSE(d.effective(get("HIDDEN")->id).visible());
}

TEST_CASE("linetypes: dash patterns are converted to millimetres") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "LTYPE").g(70, 1);
    p.g(0, "LTYPE")
        .g(2, "DASHED")
        .g(70, 0)
        .g(3, "Dashed")
        .g(72, 65)
        .g(73, 2)
        .g(40, 0.75)
        .g(49, 0.5)
        .g(74, 0)
        .g(49, -0.25)
        .g(74, 0);
    p.g(0, "ENDTAB").end();
    ImportOptions opt;
    opt.insunits = 1;  // inches
    Loaded l = load(p.text + Pairs().g(0, "EOF").text, opt);
    const Linetype* lt = l.doc().linetypeByName("dashed");
    REQUIRE(lt != nullptr);
    REQUIRE(lt->pattern.size() == 2);
    CHECK(near(lt->pattern[0], 12.7));
    CHECK(near(lt->pattern[1], -6.35));
}

TEST_CASE("text styles: width factor, oblique, fixed height and font file are kept") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE").g(70, 1);
    p.g(0, "STYLE").g(2, "Narrow").g(70, 0).g(40, 3.5).g(41, 0.8).g(50, 15.0).g(3, "arial.ttf");
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES")
        .g(0, "TEXT")
        .g(8, "0")
        .g(10, 0.0)
        .g(20, 0.0)
        .g(40, 2.0)
        .g(1, "x")
        .g(7, "Narrow")
        .end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const TextStyle* s = l.doc().textStyleByName("Narrow");
    REQUIRE(s != nullptr);
    CHECK(near(s->widthFactor, 0.8));
    CHECK(near(s->height, 3.5));  // a fixed height
    CHECK(near(s->oblique, 15 * kDeg));
    CHECK(s->fontFile == "arial.ttf");
    auto texts = modelEntities<TextData>(l.doc());
    REQUIRE(texts.size() == 1);
    CHECK(texts[0]->style == s->id);
}

TEST_CASE("text styles: SHX fonts map to the TBCAD Stroke font and are reported") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE").g(70, 3);
    p.g(0, "STYLE").g(2, "Standard").g(70, 0).g(40, 0.0).g(41, 1.0).g(3, "TXT.SHX");
    p.g(0, "STYLE")
        .g(2, "Jp")
        .g(70, 0)
        .g(40, 0.0)
        .g(41, 1.0)
        .g(3, "simplex.shx")
        .g(4, "bigfont.shx");
    p.g(0, "STYLE").g(2, "Win").g(70, 0).g(40, 0.0).g(41, 1.0).g(3, "arial.ttf");
    p.g(0, "ENDTAB").end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const TextStyle* a = l.doc().textStyleByName("Standard");
    const TextStyle* b = l.doc().textStyleByName("Jp");
    const TextStyle* c = l.doc().textStyleByName("Win");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(a->fontFamily == "TBCAD Stroke");
    CHECK(a->fontFile.empty());
    CHECK(b->fontFamily == "TBCAD Stroke");
    CHECK(c->fontFamily.empty());
    CHECK(c->fontFile == "arial.ttf");
    CHECK(c->widthFactor == doctest::Approx(1.0));
    CHECK(b->widthFactor == doctest::Approx(1.0 / 0.92));  // the stand-in is widened to SHX size
    CHECK(l.report.countCode(code::kShxFont) == 2);
}

TEST_CASE("text styles: a TrueType family in ACAD extended data is kept") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE").g(70, 2);
    p.g(0, "STYLE").g(2, "Ar").g(70, 0).g(40, 0.0).g(41, 1.0).g(3, "");
    p.g(1001, "ACAD").g(1000, "Arial").g(1071, 34).g(0, "STYLE");
    p.g(2, "Bd").g(70, 0).g(40, 0.0).g(41, 1.0).g(3, "arialbd.ttf");
    p.g(1001, "ACAD").g(1000, "Arial").g(1071, 33554466);
    p.g(0, "ENDTAB").end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const TextStyle* a = l.doc().textStyleByName("Ar");
    const TextStyle* b = l.doc().textStyleByName("Bd");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->fontFamily == "Arial");
    CHECK(a->fontFile.empty());
    CHECK(b->fontFamily == "Arial");
    CHECK(b->fontFile == "arialbd.ttf");  // the file decides; the family finds a stand-in
}

TEST_CASE("text styles: a shape font named without its extension is the stroke font too") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE").g(70, 1);
    p.g(0, "STYLE").g(2, "Plain").g(70, 0).g(40, 0.0).g(41, 1.0).g(3, "RomanS");
    p.g(0, "ENDTAB").end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const TextStyle* a = l.doc().textStyleByName("Plain");
    REQUIRE(a);
    CHECK(a->fontFamily == "TBCAD Stroke");
}

TEST_CASE("sub-layers: TBCAD XDATA restores the tree and the ByParent flags") {
    Loaded l = loadCorpus("xdata_sublayers.dxf");
    const Document& d = l.doc();
    const Layer* a = d.layerByName("A");
    REQUIRE(a != nullptr);
    const Layer* child = d.layerByName(a->id, "child");
    REQUIRE(child != nullptr);
    CHECK(child->parent == a->id);
    CHECK(child->color == Color::byParent());
    CHECK(child->linetype == kLinetypeByParent);
    CHECK(child->lineweight.isByParent());
    const Layer* leaf = d.layerByName(child->id, "leaf");
    REQUIRE(leaf != nullptr);
    CHECK(leaf->color == Color::byParent());
    CHECK(leaf->linetype != kLinetypeByParent);  // flags = 1: only the colour follows the parent
    CHECK(d.layerByName("orphan~x") != nullptr);
}

TEST_CASE("sub-layers: --split-layers keeps each leaf's own properties") {
    ImportOptions opt;
    opt.splitLayers = "~";
    Loaded l = loadCorpus("layers_tilde.dxf", opt);
    const Document& d = l.doc();
    const Layer* floor = d.layerByName("S-FLOOR");
    REQUIRE(floor != nullptr);
    const Layer* edge = d.layerByName(floor->id, "edge");
    REQUIRE(edge != nullptr);
    CHECK(edge->color == Color::index(2));  // its own DXF colour, not inherited
    CHECK(edge->color.kind == ColorKind::Index);
    // The X~y~z chain creates the intermediate layers.
    const Layer* x = d.layerByName("X");
    REQUIRE(x != nullptr);
    const Layer* y = d.layerByName(x->id, "y");
    REQUIRE(y != nullptr);
    CHECK(d.layerByName(y->id, "z") != nullptr);
}

TEST_CASE("layers: an entity on a layer missing from the table creates it") {
    Loaded l = loadCorpus("broken_no_tables.dxf");
    CHECK(l.doc().layerByName("STRAY") != nullptr);
    CHECK(l.report.countCode(code::kMissingLayer) ==
          2);  // STRAY and ALSO; "stray" is the same layer
}

// ---------------------------------------------------------------------------------------------
// Entities

TEST_CASE("polylines: LWPOLYLINE and 2D POLYLINE keep bulges, widths and the closed flag") {
    Loaded l = loadCorpus("r2000_entities.dxf");
    auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 3);
    CHECK(polys[0]->closed);
    CHECK(near(polys[0]->v[0].bulge, 0.5));
    CHECK(near(polys[1]->constantWidth, 0.5));
    CHECK_FALSE(polys[1]->closed);
    CHECK(polys[2]->v.size() == 3);
}

TEST_CASE(
    "polylines: old-style POLYLINE reads its VERTEX records, 3D polylines are drawn flat, meshes "
    "skipped") {
    Pairs p;
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(70, 1);
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(42, 1.0);
    p.g(0, "VERTEX").g(8, "0").g(10, 4.0).g(20, 0.0);
    p.g(0, "VERTEX").g(8, "0").g(10, 4.0).g(20, 3.0);
    p.g(0, "SEQEND").g(8, "0");
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(70, 8);
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 32);
    p.g(0, "VERTEX").g(8, "0").g(10, 1.0).g(20, 1.0).g(30, 5.0).g(70, 32);
    p.g(0, "SEQEND").g(8, "0");
    p.g(0, "POLYLINE").g(8, "0").g(66, 1).g(70, 64);  // polyface mesh
    p.g(0, "VERTEX").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(70, 192);
    p.g(0, "SEQEND").g(8, "0");
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 1.0);
    Loaded l = load(entitiesOnly(p.text));
    auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 2);
    CHECK(polys[0]->closed);
    CHECK(near(polys[0]->v[0].bulge, 1.0));
    REQUIRE(polys[1]->v.size() == 2);  // the 3D polyline, without its Z
    CHECK(near(polys[1]->v[1].p, Vec2{1, 1}));
    CHECK(modelEntities<LineData>(l.doc()).size() == 1);  // the entity after SEQEND still loads
    CHECK(l.report.countCode(code::kBadEntity) == 1);     // the mesh
    CHECK(l.report.countCode(code::kFlattened) == 1);
}

TEST_CASE("ellipse, spline: fit points are interpolated, control points kept") {
    Loaded l = loadCorpus("r2000_entities.dxf");
    auto ellipses = modelEntities<EllipseData>(l.doc());
    REQUIRE(ellipses.size() == 2);
    CHECK(near(ellipses[0]->ratio, 0.5));
    CHECK(near(ellipses[0]->endParam - ellipses[0]->startParam, kTwoPi));
    CHECK(near(ellipses[1]->startParam, 0.5));
    CHECK(near(ellipses[1]->endParam, 2.5));
    auto splines = modelEntities<SplineData>(l.doc());
    REQUIRE(splines.size() == 2);
    CHECK(splines[0]->control.size() >= 4);  // interpolated from the four fit points
    CHECK(splines[0]->fit.size() == 4);
    CHECK(splines[1]->control.size() == 4);
    CHECK(splines[1]->degree == 3);
}

TEST_CASE("text: alignment, rotation, height and percent codes") {
    Pairs p;
    p.g(0, "TEXT").g(8, "0").g(10, 1.0).g(20, 2.0).g(40, 3.0).g(1, "%%c25 %%p0.1 %%d").g(50, 30.0);
    p.g(72, 1).g(73, 2).g(11, 5.0).g(21, 6.0).g(41, 0.9).g(51, 10.0);
    Loaded l = load(entitiesOnly(p.text));
    auto texts = modelEntities<TextData>(l.doc());
    REQUIRE(texts.size() == 1);
    const TextData& t = *texts[0];
    CHECK(t.utf8 == "⌀25 ±0.1 °");
    CHECK(near(t.height, 3));
    CHECK(near(t.rotation, 30 * kDeg));
    CHECK(near(t.widthFactor, 0.9));
    CHECK(near(t.oblique, 10 * kDeg));
    CHECK(t.h == HAlign::Center);
    CHECK(t.v == VAlign::Middle);
    CHECK(near(t.position, Vec2{1, 2}));
    CHECK(near(t.alignPoint, Vec2{5, 6}));
}

TEST_CASE("mtext: attachment, width and rotation from the direction vector") {
    Pairs p;
    p.g(0, "MTEXT")
        .g(8, "0")
        .g(10, 0.0)
        .g(20, 0.0)
        .g(40, 2.0)
        .g(41, 50.0)
        .g(71, 5)
        .g(11, 0.0)
        .g(21, 1.0)
        .g(1, "A");
    Loaded l = load(entitiesOnly(p.text));
    auto m = modelEntities<MTextData>(l.doc());
    REQUIRE(m.size() == 1);
    CHECK(m[0]->attach == Attachment::MiddleCenter);
    CHECK(near(m[0]->width, 50));
    CHECK(near(m[0]->rotation, kPi / 2));
}

TEST_CASE("hatch: edge types, islands, patterns and bulging polyline boundaries") {
    Loaded l = loadCorpus("hatch_variants.dxf");
    auto h = modelEntities<HatchData>(l.doc());
    REQUIRE(h.size() == 5);
    // 0: solid, polyline boundary with one bulge
    CHECK(h[0]->solid);
    REQUIRE(h[0]->loops.size() == 1);
    const auto* pe = std::get_if<HatchPolylineEdge>(&h[0]->loops[0].edges[0]);
    REQUIRE(pe != nullptr);
    CHECK(near(pe->v[1].bulge, 0.5));
    // 1: line, arc, line, arc
    REQUIRE(h[1]->loops.size() == 1);
    REQUIRE(h[1]->loops[0].edges.size() == 4);
    CHECK(std::holds_alternative<HatchLineEdge>(h[1]->loops[0].edges[0]));
    CHECK(std::holds_alternative<HatchArcEdge>(h[1]->loops[0].edges[1]));
    // 2: ellipse
    REQUIRE(h[2]->loops[0].edges.size() == 1);
    CHECK(std::holds_alternative<HatchEllipseEdge>(h[2]->loops[0].edges[0]));
    // 3: spline + line
    REQUIRE(h[3]->loops[0].edges.size() == 2);
    const auto* se = std::get_if<HatchSplineEdge>(&h[3]->loops[0].edges[0]);
    REQUIRE(se != nullptr);
    CHECK(se->spline.control.size() == 4);
    // 4: pattern fill with an island
    CHECK_FALSE(h[4]->solid);
    CHECK(h[4]->pattern == "ANSI31");
    CHECK(near(h[4]->patternScale, 2.0));
    CHECK(near(h[4]->patternAngle, 30 * kDeg));
    REQUIRE(h[4]->loops.size() == 2);
}

TEST_CASE("solid and trace become solid hatches with the bow-tie corner order fixed") {
    Loaded l = loadCorpus("r2000_entities.dxf");
    auto h = modelEntities<HatchData>(l.doc());
    REQUIRE(h.size() == 5);
    const auto* quad = std::get_if<HatchPolylineEdge>(&h[3]->loops[0].edges[0]);
    // The SOLID corners (60,0) (64,0) (60,3) (64,3) are stored 1,2,3,4 and drawn 1,2,4,3.
    REQUIRE(quad != nullptr);
    REQUIRE(quad->v.size() == 4);
    CHECK(near(quad->v[2].p, Vec2{64, 3}));
    CHECK(near(quad->v[3].p, Vec2{60, 3}));
}

TEST_CASE("inserts: nested blocks, MINSERT arrays and attribute values") {
    Loaded l = loadCorpus("r2013_blocks.dxf");
    const Document& d = l.doc();
    const BlockDef* outer = d.blockByName("OUTER");
    REQUIRE(outer != nullptr);
    CHECK(outer->entities.size() == 2);  // a line and the nested INNER insert
    int nested = 0;
    for (EntityId id : outer->entities) {
        if (const auto* ins = entityAs<InsertData>(*d.find(id))) {
            CHECK(d.block(ins->block)->name == "INNER");
            ++nested;
        }
    }
    CHECK(nested == 1);
    auto inserts = modelEntities<InsertData>(d);
    REQUIRE(inserts.size() == 3);
    // The MINSERT is one insert with a 2 rows x 3 columns array at spacing 10, as in AutoCAD.
    const InsertData* grid = nullptr;
    for (const InsertData* i : inserts)
        if (d.block(i->block)->name == "INNER")
            grid = i;
    REQUIRE(grid);
    REQUIRE(grid->array);
    CHECK(grid->array->rows == 2);
    CHECK(grid->array->columns == 3);
    std::vector<Vec2> pos;
    for (const Transform2& t : blocks::arrayItemTransforms(*grid->array))
        pos.push_back(grid->position + t.apply({0, 0}));
    REQUIRE(pos.size() == 6);
    CHECK(hasEndpoint(pos, Vec2{0, 0}));
    CHECK(hasEndpoint(pos, Vec2{20, 0}));
    CHECK(hasEndpoint(pos, Vec2{10, 10}));
    CHECK(hasEndpoint(pos, Vec2{20, 10}));
    CHECK(l.result.summary.blocks == 3);
}

TEST_CASE("inserts: a block defined after its first use is resolved") {
    Pairs p;
    p.section("ENTITIES").g(0, "INSERT").g(8, "0").g(2, "LATE").g(10, 1.0).g(20, 1.0).end();
    p.section("BLOCKS").g(0, "BLOCK").g(2, "LATE").g(70, 0).g(10, 2.0).g(20, 3.0).g(30, 0.0);
    p.g(0, "POINT").g(8, "0").g(10, 0.0).g(20, 0.0).g(0, "ENDBLK").end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const BlockDef* b = l.doc().blockByName("LATE");
    REQUIRE(b != nullptr);
    CHECK(near(b->base, Vec2{2, 3}));
    CHECK(b->entities.size() == 1);
    CHECK(l.report.countCode(code::kMissingBlock) == 0);
}

// The diagnostics that compare TBCAD's dimensions with the drawings stored in the file: the
// linear ones of this file are drawn the same; the summary goes into the report JSON.
TEST_CASE("dimension comparison lists each dimension against its stored block") {
    ImportOptions compare;
    compare.dimensionCompare = true;
    Loaded l = loadCorpus("r2000_dimensions.dxf", compare);
    const auto& all = l.result.summary.dimCompare;
    REQUIRE(all.size() == 6);
    int linear = 0;
    for (const DimComparison& c : all) {
        CHECK(c.handle != 0);
        if (c.kind != "linear")
            continue;
        ++linear;
        CHECK(c.textMatch);
        CHECK(c.textOffset < 0.5);
        CHECK(c.heightRatio == doctest::Approx(1).epsilon(0.05));
        CHECK(c.strayStored < 0.05);
        CHECK(c.strayGenerated < 0.05);
        CHECK(c.arrowsUnmatched == 0);
        // The details name what the dimension was read as.
        CHECK(c.detail.find("measured=10 ") != std::string::npos);
        CHECK(c.detail.find("storedText@(") != std::string::npos);
    }
    CHECK(linear == 2);
    // The diameter sign is the same whichever way the file spells it.
    for (const DimComparison& c : all)
        if (c.kind == "diameter")
            CHECK(c.textMatch);
    const std::string json = l.result.summary.toJson(l.report);
    CHECK(json.find("\"dimCompare\":{\"total\":{\"count\":6") != std::string::npos);
    // Without the option nothing is compared.
    Loaded plain = loadCorpus("r2000_dimensions.dxf");
    CHECK(plain.result.summary.dimCompare.empty());
}

// The details carry what finding a difference needs without the file: the style's text rules,
// points near the dimension rather than far from the origin, and the size the writing program
// measured the stored text at (MTEXT 42 / 43, which AutoCAD measures with its own fonts).
TEST_CASE("dimension comparison details: text rules, relative points, the writer's text size") {
    const std::vector<std::byte> bytes = readFile(corpusPath("reader/r2000_dimensions.dxf"));
    REQUIRE(!bytes.empty());
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::string mtext = "  0\nMTEXT\n";
    std::size_t marked = 0;
    for (std::size_t at = text.find(mtext); at != std::string::npos;
         at = text.find(mtext, at + 1), ++marked)
        text.insert(at + mtext.size(), " 42\n4.5\n 43\n2.5\n");
    REQUIRE(marked > 0);
    ImportOptions compare;
    compare.dimensionCompare = true;
    Loaded l = load(text, compare);
    const auto& all = l.result.summary.dimCompare;
    REQUIRE(!all.empty());
    bool sized = false;
    for (const DimComparison& c : all) {
        CHECK(c.detail.find(" tad=") != std::string::npos);
        CHECK(c.detail.find(" tmove=") != std::string::npos);
        CHECK(c.detail.find(" at=(") != std::string::npos);
        CHECK(c.detail.find(" defPts=(0,0)") != std::string::npos);
        CHECK(!c.style.empty());
        sized = sized || c.detail.find("storedExtent=4.5x2.5") != std::string::npos;
    }
    CHECK(sized);
    CHECK(l.result.summary.toJson(l.report).find("\"byStyle\":{") != std::string::npos);
}

TEST_CASE("dimensions: definition points and the drawn graphics") {
    ImportOptions withGraphics;
    withGraphics.dimensionGraphics = true;
    Loaded l = loadCorpus("r2000_dimensions.dxf", withGraphics);
    const Document& d = l.doc();
    auto dims = modelEntities<DimensionData>(d);
    REQUIRE(dims.size() == 6);
    std::vector<DimKind> kinds;
    for (const DimensionData* dim : dims)
        kinds.push_back(dim->kind);
    auto count = [&](DimKind k) { return std::count(kinds.begin(), kinds.end(), k); };
    // ezdxf (like some CAD programs) writes an aligned dimension as a rotated linear one.
    CHECK(count(DimKind::Linear) == 2);
    CHECK(count(DimKind::Radius) == 1);
    CHECK(count(DimKind::Diameter) == 1);
    CHECK(count(DimKind::Angular2Line) == 1);
    CHECK(count(DimKind::Angular3Point) == 1);
    // First dimension: horizontal, 10 long; its definition points are the two ends.
    const DimensionData& lin = *dims[0];
    CHECK(near(lin.defPts[0], Vec2{0, 0}));
    CHECK(near(lin.defPts[1], Vec2{10, 0}));
    CHECK(near(lin.measurement, 10));
    CHECK(near(dims[1]->measurement, 10));  // derived from the points: the file has no group 42
    CHECK(near(dims[1]->rotation, std::atan2(6.0, 8.0), 1e-9));
    // Every dimension brings a proxy with the drawn lines and loose text for its dimension text.
    CHECK(modelEntities<ProxyData>(d).size() == 6);
    CHECK(modelEntities<MTextData>(d).size() + modelEntities<TextData>(d).size() >= 6);
    for (const ProxyData* p : modelEntities<ProxyData>(d)) {
        CHECK_FALSE(p->graphics.items.empty());
    }
}

TEST_CASE("dimensions: the drawn graphics are not kept by default") {
    Loaded l = loadCorpus("r2000_dimensions.dxf");
    CHECK(modelEntities<DimensionData>(l.doc()).size() == 6);
    CHECK(modelEntities<ProxyData>(l.doc()).empty());
}

TEST_CASE("leader and proxy entities") {
    Loaded l = loadCorpus("r2000_entities.dxf");
    auto leaders = modelEntities<LeaderData>(l.doc());
    REQUIRE(leaders.size() == 1);
    CHECK(leaders[0]->vertices.size() == 3);
    Loaded p = loadCorpus("proxy_entity.dxf");
    auto proxies = modelEntities<ProxyData>(p.doc());
    REQUIRE(proxies.size() == 1);
    CHECK(proxies[0]->originalType == "ACAD_PROXY_ENTITY");
    CHECK(proxies[0]->raw.size() == 6);
}

// ---------------------------------------------------------------------------------------------
// Robustness

TEST_CASE("far coordinates: beyond the work area kept, beyond the hard cap dropped") {
    Loaded l = loadCorpus("far_coordinates.dxf");
    CHECK(modelEntities<LineData>(l.doc()).size() == 2);
    CHECK(l.result.summary.beyondHardCap == 1);
    CHECK(l.result.summary.outsideWorkArea == 1);
    CHECK(l.report.countCode(code::kBeyondHardCap) == 1);
}

TEST_CASE("encoding: forcing the wrong encoding changes the text, the right one fixes it") {
    ImportOptions wrong;
    wrong.encoding = Encoding::Cp1252;
    Loaded a = loadCorpus("thai_cp874_no_codepage.dxf", wrong);
    ImportOptions right;
    right.encoding = Encoding::Cp874;
    Loaded b = loadCorpus("thai_cp874_no_codepage.dxf", right);
    auto ta = modelEntities<TextData>(a.doc());
    auto tb = modelEntities<TextData>(b.doc());
    REQUIRE(ta.size() == 1);
    REQUIRE(tb.size() == 1);
    CHECK(ta[0]->utf8 != tb[0]->utf8);
    CHECK(tb[0]->utf8.find("ส") != std::string::npos);  // THAI CHARACTER SO SUA
}

TEST_CASE("layer 0 exists even when the file has no layer table") {
    Pairs p;
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 1.0);
    Loaded l = load(entitiesOnly(p.text));
    CHECK(l.report.countCode(code::kMissingLayer) == 0);
    CHECK(modelEntities<LineData>(l.doc()).size() == 1);
}

TEST_CASE("paper space entities become a layout sheet") {
    Pairs p;
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 1.0);
    p.g(0, "LINE").g(8, "0").g(67, 1).g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 1.0);
    Loaded l = load(entitiesOnly(p.text));
    CHECK(modelEntities<LineData>(l.doc()).size() == 1);
    CHECK(l.report.countCode(code::kPaperSpace) == 0);
    CHECK(l.result.summary.layouts == 1);
    const auto it = l.doc().header().custom.find("tbcad.layouts");
    REQUIRE(it != l.doc().header().custom.end());
    CHECK(it->second.rfind("L\t1\tLayout1\tCustom\t", 0) == 0);
}

TEST_CASE("a layout takes its name and paper size from the LAYOUT object and keeps its viewports") {
    Pairs p;
    p.section("BLOCKS")
        .g(0, "BLOCK")
        .g(2, "*Paper_Space")
        .g(70, 0)
        .g(10, 0.0)
        .g(20, 0.0)
        .g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(67, 1).g(10, 5.0).g(20, 5.0).g(11, 100.0).g(21, 5.0);
    p.g(0, "VIEWPORT").g(8, "0").g(67, 1).g(10, 210.0).g(20, 148.5).g(40, 400.0).g(41, 280.0);
    p.g(69, 2).g(68, 1).g(12, 1000.0).g(22, 2000.0).g(45, 28000.0);
    p.g(0, "ENDBLK").end();
    p.section("OBJECTS").g(0, "LAYOUT").g(1, "Model").g(71, 0).g(44, 297.0).g(45, 210.0);
    p.g(0, "LAYOUT").g(1, "Sheet A1").g(71, 1).g(44, 841.0).g(45, 594.0).g(73, 0).end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    CHECK(l.result.summary.layouts == 1);
    const std::string& t = l.doc().header().custom.at("tbcad.layouts");
    CHECK(t.rfind("L\t1\tSheet A1\tCustom\t1\t841\t594\t", 0) == 0);
    // Viewport 1: paper rectangle (10,8.5)-(410,288.5), centre (1000,2000), 1:100.
    CHECK(t.find("V\t1\t10\t8.5\t410\t288.5\t1000\t2000\t100\t1\t") != std::string::npos);
}

TEST_CASE("a viewport on a layer that does not plot has no plotted border") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "LAYER").g(70, 2);
    p.g(0, "LAYER").g(2, "0").g(70, 0).g(62, 7).g(6, "Continuous");
    p.g(0, "LAYER").g(2, "Defpoints").g(70, 0).g(62, 7).g(6, "Continuous").g(290, 0);
    p.g(0, "ENDTAB").g(0, "ENDSEC");
    p.section("BLOCKS")
        .g(0, "BLOCK")
        .g(2, "*Paper_Space")
        .g(70, 0)
        .g(10, 0.0)
        .g(20, 0.0)
        .g(30, 0.0);
    p.g(0, "VIEWPORT")
        .g(8, "Defpoints")
        .g(67, 1)
        .g(10, 210.0)
        .g(20, 148.5)
        .g(40, 400.0)
        .g(41, 280.0);
    p.g(69, 2).g(68, 1).g(12, 1000.0).g(22, 2000.0).g(45, 28000.0);
    p.g(0, "VIEWPORT").g(8, "0").g(67, 1).g(10, 600.0).g(20, 148.5).g(40, 100.0).g(41, 100.0);
    p.g(69, 3).g(68, 1).g(12, 1000.0).g(22, 2000.0).g(45, 10000.0);
    p.g(0, "ENDBLK").end();
    p.section("OBJECTS").g(0, "LAYOUT").g(1, "Model").g(71, 0).g(44, 297.0).g(45, 210.0);
    p.g(0, "LAYOUT").g(1, "Sheet A1").g(71, 1).g(44, 841.0).g(45, 594.0).g(73, 0).end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const std::string& t = l.doc().header().custom.at("tbcad.layouts");
    const auto line = [&](const char* start) {
        const auto at = t.find(start);
        REQUIRE(at != std::string::npos);
        return t.substr(at, t.find('\n', at) - at);
    };
    const std::string v1 = line("V\t1\t"), v2 = line("V\t2\t");
    CHECK(v1.substr(v1.size() - 2) == "\t0");  // border off: Defpoints does not plot
    CHECK(v2.substr(v2.size() - 2) == "\t1");
}

TEST_CASE("a layout's plot style table becomes a page setup assigned to it") {
    Pairs p;
    p.section("BLOCKS")
        .g(0, "BLOCK")
        .g(2, "*Paper_Space")
        .g(70, 0)
        .g(10, 0.0)
        .g(20, 0.0)
        .g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(67, 1).g(10, 5.0).g(20, 5.0).g(11, 100.0).g(21, 5.0);
    p.g(0, "ENDBLK").end();
    p.section("OBJECTS");
    p.g(0, "LAYOUT").g(1, "Sheet A1").g(7, "D:\\office\\Plot Styles\\mono.ctb").g(71, 1);
    p.g(44, 841.0).g(45, 594.0).g(73, 0).end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const std::string& t = l.doc().header().custom.at("tbcad.layouts");
    CHECK(t.find("\tSheet A1\n") != std::string::npos);
    CHECK(l.doc().header().custom.at("tbcad.pagesetups") == "S\tSheet A1\tstyle=mono.ctb\n");
}

TEST_CASE("dimension: style overrides in its extended data and a relative text height") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(2, "Big").g(140, 450.0).g(41, 400.0).g(40, 1.0);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Big").g(10, 5.0).g(20, 5.0).g(11, 5.0);
    p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
    p.g(1, "{\\H560x;<>}");
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 140).g(1040, 1.25).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(near(ds->textHeight, 700.0));
    CHECK(near(ds->arrowSize, 400.0));
}

// Only a height code that starts the text sizes the whole dimension text; one in the middle
// ("9d{\\H0.8x;b}", a smaller "b") leaves the style's height alone.
TEST_CASE("dimension: a relative height in the middle of the text is not the text height") {
    auto heightFor = [](const std::string& text) {
        Pairs p;
        p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
        p.g(0, "DIMSTYLE").g(2, "S").g(140, 2.0).g(41, 1.0).g(40, 1.0);
        p.g(0, "ENDTAB").end();
        p.section("ENTITIES");
        p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "S").g(10, 5.0).g(20, 5.0).g(11, 5.0);
        p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
        p.g(1, text);
        p.end().g(0, "EOF");
        Loaded l = load(p.text);
        auto dims = modelEntities<DimensionData>(l.doc());
        REQUIRE(dims.size() == 1);
        const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
        REQUIRE(ds != nullptr);
        return ds->textHeight;
    };
    CHECK(near(heightFor("9d{\\H0.8x;b}"), 2.0));
    CHECK(near(heightFor("<>{\\H0.5x;mm}"), 2.0));
    CHECK(near(heightFor("{\\H0.5x;<>}"), 1.0));
    CHECK(near(heightFor("\\A1;{\\H0.5x;<>}"), 1.0));
    CHECK(near(heightFor("{\\Fromans|c0;\\H0.5x;<>}"), 1.0));
}

TEST_CASE("hatch: gradient colours, angle and name are kept") {
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "HATCH").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(2, "SOLID").g(70, 1).g(71, 0);
    p.g(91, 1).g(92, 3).g(72, 0).g(73, 1).g(93, 4);
    p.g(10, 0.0).g(20, 0.0).g(10, 10.0).g(20, 0.0).g(10, 10.0).g(20, 10.0).g(10, 0.0).g(20, 10.0);
    p.g(97, 0).g(75, 0).g(76, 1);
    p.g(450, 1).g(451, 0).g(460, 1.5).g(461, 0.0).g(452, 0).g(462, 1.0).g(453, 2);
    p.g(463, 0.0).g(421, 16711680).g(463, 1.0).g(421, 255).g(470, "LINEAR");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    auto hatches = modelEntities<HatchData>(l.doc());
    REQUIRE(hatches.size() == 1);
    REQUIRE(hatches[0]->gradient.has_value());
    CHECK(hatches[0]->gradient->kind == GradientKind::Linear);
    CHECK(near(hatches[0]->gradient->angle, 1.5));
    CHECK(hatches[0]->gradient->color1 == Color::rgb(255, 0, 0));
    CHECK(hatches[0]->gradient->color2 == Color::rgb(0, 0, 255));
}

TEST_CASE("dimension style: DIMTXSTY names the text style by handle, also as an override") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE");
    p.g(0, "STYLE").g(5, "11").g(2, "Notes").g(70, 0).g(40, 0.0).g(3, "arial.ttf");
    p.g(0, "STYLE").g(5, "12").g(2, "Fixed").g(70, 0).g(40, 3.5).g(3, "arial.ttf");
    p.g(0, "ENDTAB");
    p.g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "Dn").g(340, "11").g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Dn").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 340).g(1005, "12").g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* dn = l.doc().dimStyleByName("Dn");
    REQUIRE(dn != nullptr);
    const TextStyle* notes = l.doc().textStyleByName("Notes");
    REQUIRE(notes != nullptr);
    CHECK(dn->textStyle == notes->id);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    const TextStyle* fixed = l.doc().textStyleByName("Fixed");
    REQUIRE(fixed != nullptr);
    CHECK(ds->textStyle == fixed->id);
    CHECK(near(ds->textHeight * ds->overallScale, 3.5));
}

TEST_CASE("dimension style: DIMLWD and DIMLWE, also as an override") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "Lw").g(371, 25).g(372, -1).g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Lw").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 372).g(1070, 50).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* lw = l.doc().dimStyleByName("Lw");
    REQUIRE(lw != nullptr);
    CHECK(lw->dimLineWeight == LineWeight::hundredths(25));
    CHECK(lw->extLineWeight == LineWeight::byLayer());
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->dimLineWeight == LineWeight::hundredths(25));
    CHECK(ds->extLineWeight == LineWeight::hundredths(50));
}

TEST_CASE("dimension style: DIMLTYPE and DIMLTEX1/2 by LTYPE handle, also as an override") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "LTYPE");
    p.g(0, "LTYPE").g(5, "14").g(2, "ByBlock").g(70, 0).g(3, "").g(72, 65).g(73, 0).g(40, 0.0);
    p.g(0, "LTYPE").g(5, "15").g(2, "ByLayer").g(70, 0).g(3, "").g(72, 65).g(73, 0).g(40, 0.0);
    p.g(0, "LTYPE").g(5, "a1").g(2, "HIDDEN").g(70, 0).g(3, "__ __").g(72, 65).g(73, 2);
    p.g(40, 9.0).g(49, 6.0).g(74, 0).g(49, -3.0).g(74, 0);
    p.g(0, "ENDTAB").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "Lt").g(345, "15").g(346, "A1").g(347, "14");
    p.g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Lt").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 345).g(1005, "14").g(1070, 347);
    p.g(1005, "a1").g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* lt = l.doc().dimStyleByName("Lt");
    REQUIRE(lt != nullptr);
    CHECK(lt->dimLinetype == "ByLayer");
    CHECK(lt->ext1Linetype == "HIDDEN");
    CHECK(lt->ext2Linetype.empty());
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->dimLinetype.empty());
    CHECK(ds->ext1Linetype == "HIDDEN");
    CHECK(ds->ext2Linetype == "HIDDEN");
}

TEST_CASE("dimension style: DIMAZIN, DIMTZIN and DIMALTZ, also as overrides") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "Z").g(78, 8).g(79, 3).g(284, 4).g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Z").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 79).g(1070, 0).g(1070, 285);
    p.g(1070, 12).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* z = l.doc().dimStyleByName("Z");
    REQUIRE(z != nullptr);
    CHECK(z->suppressTrailingZeros);
    CHECK(z->angularZeros == 3);
    CHECK(z->tolZeros == 1);
    CHECK(z->altZeros == 0);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->angularZeros == 0);
    CHECK(ds->altZeros == 3);
}

TEST_CASE("dimension style: DIMALTRND and DIMTOLJ, also as overrides") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "R").g(148, 0.5).g(283, 0).g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "R").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 283).g(1070, 2).g(1070, 148);
    p.g(1040, 0.1).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* r = l.doc().dimStyleByName("R");
    REQUIRE(r != nullptr);
    CHECK(r->altRoundOff == doctest::Approx(0.5));
    CHECK(r->tolJustify == 0);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->altRoundOff == doctest::Approx(0.1));
    CHECK(ds->tolJustify == 2);
}

TEST_CASE("dimension style: a built-in arrow block name picks the arrow kind") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(2, "Tick").g(342, "_ARCHTICK").g(140, 2.5).g(41, 2.5).g(40, 1.0);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Tick").g(10, 5.0).g(20, 5.0).g(11, 5.0);
    p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->arrowType == ArrowType::ArchTick);
}

TEST_CASE("dimension style: arrow blocks named by block record handle, as AutoCAD writes them") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(105, "30").g(2, "Tick").g(342, "1F").g(341, "3B").g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB");
    // The block record table comes after the dimension styles.
    p.g(0, "TABLE").g(2, "BLOCK_RECORD");
    p.g(0, "BLOCK_RECORD").g(5, "1F").g(2, "_ArchTick");
    p.g(0, "BLOCK_RECORD").g(5, "2a").g(2, "_Dot");
    p.g(0, "BLOCK_RECORD").g(5, "3B").g(2, "_Open30");
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Tick").g(10, 5.0).g(20, 5.0).g(11, 5.0);
    p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D2").g(3, "Tick").g(10, 5.0).g(20, 25.0).g(11, 5.0);
    p.g(21, 28.0).g(70, 1).g(13, 0.0).g(23, 20.0).g(14, 10.0).g(24, 20.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 342).g(1005, "2A");
    p.g(1070, 147).g(1040, 0.5).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* tick = l.doc().dimStyleByName("Tick");
    REQUIRE(tick != nullptr);
    CHECK(tick->arrowType == ArrowType::ArchTick);
    CHECK(tick->leaderArrow == ArrowType::Open30);  // DIMLDRBLK
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 2);
    int dots = 0, ticks = 0;
    for (const auto* d : dims) {
        const DimStyle* ds = l.doc().dimStyle(d->style);
        REQUIRE(ds != nullptr);
        if (ds->arrowType == ArrowType::Dot) {
            ++dots;
            CHECK(near(ds->textGap, 0.5));  // the value after the handle is still read
        }
        ticks += ds->arrowType == ArrowType::ArchTick;
    }
    CHECK(dots == 1);
    CHECK(ticks == 1);
}

TEST_CASE("dimension style: DIMTSZ draws ticks of that size, also as an override") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(2, "Tsz").g(140, 2.5).g(41, 2.5).g(142, 1.5).g(40, 1.0);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Standard").g(10, 5.0).g(20, 5.0).g(11, 5.0);
    p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 142).g(1040, 0.8).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* tsz = l.doc().dimStyleByName("Tsz");
    REQUIRE(tsz != nullptr);
    CHECK(tsz->arrowType == ArrowType::ArchTick);
    CHECK(near(tsz->arrowSize, 1.5));
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    CHECK(ds->arrowType == ArrowType::ArchTick);
    CHECK(near(ds->arrowSize, 0.8));
}

TEST_CASE("dimension style: DIMSAH takes the separate arrows, also as an override") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    // Separate arrows without names are the closed filled arrow.
    p.g(0, "DIMSTYLE").g(2, "Sah").g(342, "_ARCHTICK").g(173, 1).g(140, 2.5).g(41, 2.5);
    p.g(0, "DIMSTYLE").g(2, "Tick").g(342, "_ARCHTICK").g(343, "_DOT").g(140, 2.5).g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(3, "Sah").g(10, 5.0).g(20, 5.0).g(70, 1);
    p.g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0);
    // DIMSAH switched on by the dimension itself: the style's DIMBLK1.
    p.g(0, "DIMENSION").g(8, "0").g(3, "Tick").g(10, 5.0).g(20, 5.0).g(70, 1);
    p.g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 173).g(1070, 1).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 2);
    CHECK(l.doc().dimStyle(dims[0]->style)->arrowType == ArrowType::ClosedFilled);
    CHECK(l.doc().dimStyleByName("Tick")->arrowType == ArrowType::ArchTick);
    CHECK(l.doc().dimStyle(dims[1]->style)->arrowType == ArrowType::Dot);
}

TEST_CASE("dimension style: DIMSAH with a different DIMBLK2 gives the second arrow") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(2, "Two").g(343, "_DOT").g(344, "_ARCHTICK").g(173, 1).g(140, 2.5);
    p.g(41, 2.5);
    p.g(0, "DIMSTYLE").g(2, "Same").g(343, "_DOT").g(344, "_DOT").g(173, 1).g(140, 2.5);
    p.g(41, 2.5);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    // DIMSAH off by the dimension itself: both arrows are DIMBLK again (absent: DIMBLK1).
    p.g(0, "DIMENSION").g(8, "0").g(3, "Two").g(10, 5.0).g(20, 5.0).g(70, 1);
    p.g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0);
    p.g(1001, "ACAD").g(1000, "DSTYLE").g(1002, "{").g(1070, 173).g(1070, 0).g(1002, "}");
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    const DimStyle* two = l.doc().dimStyleByName("Two");
    REQUIRE(two != nullptr);
    CHECK(two->arrowType == ArrowType::Dot);
    REQUIRE(two->arrowType2.has_value());
    CHECK(*two->arrowType2 == ArrowType::ArchTick);
    CHECK_FALSE(l.doc().dimStyleByName("Same")->arrowType2.has_value());
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    CHECK(ds->arrowType == ArrowType::Dot);
    CHECK_FALSE(ds->arrowType2.has_value());
}

TEST_CASE("dimension style: a text style with a fixed height decides the text height") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE");
    p.g(0, "STYLE").g(5, "1B").g(2, "Fixed").g(70, 0).g(40, 187.5).g(3, "arial.ttf");
    p.g(0, "ENDTAB").g(0, "TABLE").g(2, "DIMSTYLE");
    p.g(0, "DIMSTYLE").g(2, "Revit").g(340, "1B").g(140, 191.3).g(41, 0.5).g(40, 304.8);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(3, "Revit").g(10, 5.0).g(20, 5.0).g(11, 5.0);
    p.g(21, 8.0).g(70, 1).g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(42, 10.0);
    p.end().g(0, "EOF");
    Loaded l = load(p.text);
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    const DimStyle* ds = l.doc().dimStyle(dims[0]->style);
    REQUIRE(ds != nullptr);
    // Text height times the overall scale is the fixed height of the style.
    CHECK(near(ds->textHeight * ds->overallScale, 187.5));
}

TEST_CASE("layout: a paper rectangle away from the origin moves to the origin") {
    Pairs p;
    p.section("BLOCKS")
        .g(0, "BLOCK")
        .g(2, "*Paper_Space")
        .g(70, 0)
        .g(10, 0.0)
        .g(20, 0.0)
        .g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(67, 1).g(10, 505.0).g(20, -40.0).g(11, 600.0).g(21, -40.0);
    p.g(0, "VIEWPORT").g(8, "0").g(67, 1).g(10, 600.0).g(20, 50.0).g(40, 200.0).g(41, 100.0);
    p.g(69, 2).g(68, 1).g(12, 1000.0).g(22, 2000.0).g(45, 10000.0);
    p.g(0, "ENDBLK").end();
    p.section("OBJECTS").g(0, "LAYOUT").g(1, "Model").g(71, 0).g(44, 297.0).g(45, 210.0);
    p.g(0, "LAYOUT").g(1, "Sheet A3").g(71, 1).g(44, 297.0).g(45, 420.0).g(73, 1);
    p.g(14, 493.0).g(24, -52.0).g(15, 913.0).g(25, 245.0).end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const std::string& t = l.doc().header().custom.at("tbcad.layouts");
    // Viewport rectangle (500,0)-(700,100) minus the paper corner (493,-52).
    CHECK(t.find("V\t1\t7\t52\t207\t152\t") != std::string::npos);
}

TEST_CASE("an older version than R12 is refused") {
    Report report;
    Result<ImportResult> r = importDxf(
        bytesOf("  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1006\n  0\nENDSEC\n  0\nEOF\n"),
        {}, report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("AC1006") != std::string::npos);
}

TEST_CASE("the JSON report lists counts, units, encoding and diagnostics") {
    Loaded l = loadCorpus("unknown_entities.dxf");
    std::string json = l.result.summary.toJson(l.report);
    CHECK(json.find("\"3DSOLID\":{\"read\":2,\"imported\":1,\"skipped\":1}") != std::string::npos);
    CHECK(json.find("\"encoding\":\"ANSI_1252\"") != std::string::npos);
    CHECK(json.find("dxf.reader.skipped_entity") != std::string::npos);
}

TEST_CASE("attributes: ATTDEF becomes an attribute definition and ATTRIB values go to the insert") {
    Loaded l = loadCorpus("r2013_blocks.dxf");
    const Document& d = l.doc();
    bool foundDef = false;
    d.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e); t && t->attdef) {
            foundDef = true;
            CHECK(t->attdef->tag == "TAG");
            CHECK(t->attdef->defaultValue == "default");
        }
    });
    CHECK(foundDef);
    std::vector<std::string> values;
    for (const InsertData* i : modelEntities<InsertData>(d))
        for (const AttribValue& v : i->attribs)
            values.push_back(v.tag + "=" + v.value);
    REQUIRE(values.size() == 1);
    CHECK(values[0] == "TAG=V1");
    CHECK(modelEntities<TextData>(d).empty());  // the value is no longer a loose text
}

TEST_CASE("attributes: flags, prompt, an unknown tag stays visible as text") {
    Pairs p;
    p.section("BLOCKS").g(0, "BLOCK").g(2, "B").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(0, "ATTDEF").g(8, "0").g(10, 1.0).g(20, 2.0).g(40, 2.5).g(1, "dflt").g(3, "Name?");
    p.g(2, "nm").g(70, 5);
    p.g(0, "ENDBLK").end();
    p.section("ENTITIES").g(0, "INSERT").g(8, "0").g(66, 1).g(2, "B").g(10, 5.0).g(20, 5.0);
    p.g(0, "ATTRIB").g(8, "0").g(10, 6.0).g(20, 7.0).g(40, 2.5).g(1, "Bob").g(2, "NM").g(70, 0);
    p.g(0, "ATTRIB").g(8, "0").g(10, 8.0).g(20, 9.0).g(40, 2.5).g(1, "stray").g(2, "OTHER");
    p.g(70, 0).g(0, "SEQEND").g(8, "0").end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const Document& d = l.doc();
    const BlockDef* b = d.blockByName("B");
    REQUIRE(b);
    REQUIRE(b->entities.size() == 1);
    const auto* def = entityAs<TextData>(*d.find(b->entities[0]));
    REQUIRE(def);
    REQUIRE(def->attdef);
    CHECK(def->attdef->tag == "NM");  // upper-cased
    CHECK(def->attdef->prompt == "Name?");
    CHECK(def->attdef->invisible);
    CHECK(def->attdef->verify);
    CHECK_FALSE(def->attdef->constant);
    auto inserts = modelEntities<InsertData>(d);
    REQUIRE(inserts.size() == 1);
    REQUIRE(inserts[0]->attribs.size() == 1);
    CHECK(inserts[0]->attribs[0].tag == "NM");
    CHECK(inserts[0]->attribs[0].value == "Bob");
    auto texts = modelEntities<TextData>(d);
    REQUIRE(texts.size() == 1);
    CHECK(texts[0]->utf8 == "stray");
    CHECK_FALSE(texts[0]->attdef.has_value());
}

TEST_CASE("MLEADER: lines, dogleg and text come in as one multileader") {
    Pairs p;
    p.g(0, "MULTILEADER").g(8, "0").g(100, "AcDbMLeader").g(270, 2);
    p.g(300, "CONTEXT_DATA{").g(40, 1.0).g(10, 50.0).g(20, 20.0).g(30, 0.0);
    p.g(41, 2.5).g(290, 1).g(304, "Room {\\C1;A}");
    p.g(12, 50.0).g(22, 20.0).g(32, 0.0);
    p.g(302, "LEADER{").g(290, 1).g(291, 1).g(10, 40.0).g(20, 20.0).g(30, 0.0);
    p.g(11, 1.0).g(21, 0.0).g(31, 0.0).g(90, 0).g(40, 5.0);
    p.g(304, "LEADER_LINE{").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(10, 20.0).g(20, 10.0).g(30, 0.0);
    p.g(10, 40.0).g(20, 20.0).g(30, 0.0).g(91, 0).g(305, "}").g(303, "}");
    p.g(301, "}");
    Loaded l = load(entitiesOnly(p.text));
    auto leaders = modelEntities<MLeaderData>(l.doc());
    REQUIRE(leaders.size() == 1);
    const MLeaderData& d = *leaders[0];
    // From the arrowhead at the far end to the landing, then the dogleg to the note.
    REQUIRE(d.lines.size() == 1);
    CHECK(near(d.lines[0].front(), Vec2{0, 0}));
    CHECK(near(d.landing, Vec2{40, 20}));
    CHECK(near(d.contentPos, Vec2{45, 20}));
    CHECK(d.dogleg);
    CHECK(d.utf8.find("Room") != std::string::npos);
}

TEST_CASE("3DFACE: the visible edges in plan") {
    Pairs p;
    p.g(0, "3DFACE").g(8, "0").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(11, 10.0).g(21, 0.0).g(31, 5.0);
    p.g(12, 10.0).g(22, 10.0).g(32, 5.0).g(13, 0.0).g(23, 10.0).g(33, 0.0).g(70, 2);
    Loaded l = load(entitiesOnly(p.text));
    CHECK(modelEntities<LineData>(l.doc()).size() == 3);  // edge 2 is invisible
}

TEST_CASE("WIPEOUT: the clip boundary becomes a closed polyline that hides what is behind it") {
    Pairs p;
    p.g(0, "WIPEOUT").g(8, "0").g(100, "AcDbWipeout").g(90, 0);
    p.g(10, 100.0).g(20, 0.0).g(30, 0.0).g(11, 20.0).g(21, 0.0).g(31, 0.0);
    p.g(12, 0.0).g(22, 20.0).g(32, 0.0).g(13, 1.0).g(23, 1.0).g(71, 2).g(91, 5);
    p.g(14, -0.5).g(24, 0.5).g(14, 0.5).g(24, 0.5).g(14, 0.5).g(24, -0.5);
    p.g(14, -0.5).g(24, -0.5).g(14, -0.5).g(24, 0.5);
    Loaded l = load(entitiesOnly(p.text));
    bool found = false;
    l.doc().forEachEntity([&](const Entity& e) {
        const auto* pl = entityAs<PolylineData>(e);
        if (!pl)
            return;
        found = true;
        const bool wipeout = e.common.wipeout;
        CHECK(wipeout);
        CHECK(pl->closed);
        REQUIRE(pl->v.size() == 4);
        BBox2 b;
        for (const auto& v : pl->v)
            b.expand(v.p);
        CHECK(near(b.min, Vec2{100, 0}));
        CHECK(near(b.max, Vec2{120, 20}));
    });
    CHECK(found);
}

TEST_CASE("WIPEOUT: the clip boundary counts y down from the top of the frame") {
    // AutoCAD's L-shaped wipeout over (0,0)-(40,20): the foot of the L is at the bottom.
    Pairs p;
    p.g(0, "WIPEOUT").g(8, "0").g(100, "AcDbWipeout").g(90, 0);
    p.g(10, 0.0).g(20, 0.0).g(30, 0.0).g(11, 40.0).g(21, 0.0).g(31, 0.0);
    p.g(12, 0.0).g(22, 20.0).g(32, 0.0).g(13, 1.0).g(23, 1.0).g(71, 2).g(91, 7);
    // (0,0) (40,0) (40,10) (10,10) (10,20) (0,20) in pixel units: x/40 - 0.5, 0.5 - y/20
    const double px[][2] = {{-0.5, 0.5},   {0.5, 0.5},   {0.5, 0.0}, {-0.25, 0.0},
                            {-0.25, -0.5}, {-0.5, -0.5}, {-0.5, 0.5}};
    for (const auto& q : px)
        p.g(14, q[0]).g(24, q[1]);
    Loaded l = load(entitiesOnly(p.text));
    const Vec2 want[] = {{0, 0}, {40, 0}, {40, 10}, {10, 10}, {10, 20}, {0, 20}};
    std::size_t found = 0;
    l.doc().forEachEntity([&](const Entity& e) {
        const auto* pl = entityAs<PolylineData>(e);
        REQUIRE(pl != nullptr);
        ++found;
        REQUIRE(pl->v.size() == 6);
        for (std::size_t i = 0; i < 6; ++i)
            CHECK(near(pl->v[i].p, want[i]));
    });
    CHECK(found == 1);
}

TEST_CASE("ARC_DIMENSION: centre, ends and arc point give an arc length dimension") {
    Pairs p;
    p.g(0, "ARC_DIMENSION").g(8, "0").g(100, "AcDbDimension").g(3, "Standard");
    p.g(10, 60.0).g(20, -20.0).g(30, 0.0).g(70, 37).g(71, 5).g(1, "<>");
    p.g(100, "AcDbArcDimension").g(13, 70.0).g(23, 0.0).g(33, 0.0).g(14, 60.0).g(24, 10.0);
    p.g(34, 0.0).g(15, 60.0).g(25, 0.0).g(35, 0.0);
    Loaded l = load(entitiesOnly(p.text));
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    CHECK(dims[0]->kind == DimKind::ArcLength);
    CHECK(dims[0]->measurement == doctest::Approx(kPi / 2 * 10));
}

TEST_CASE("MLINE: one polyline per element, offset along the miter direction") {
    Pairs p;
    p.g(0, "MLINE").g(8, "0").g(100, "AcDbMline").g(2, "Standard").g(40, 1.0).g(70, 0).g(71, 1);
    p.g(72, 3).g(73, 2).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    auto vertex = [&](double x, double y, double dx, double dy, double mx, double my, double o) {
        p.g(11, x).g(21, y).g(31, 0.0).g(12, dx).g(22, dy).g(32, 0.0).g(13, mx).g(23, my).g(33,
                                                                                            0.0);
        p.g(74, 2).g(41, 0.0).g(41, 0.0).g(75, 0);
        p.g(74, 2).g(41, o).g(41, 0.0).g(75, 0);
    };
    vertex(0, 0, 1, 0, 0, 1, -1);
    vertex(50, 0, 0, 1, -0.70710678118654746, 0.70710678118654746, -1.4142135623730951);
    vertex(50, 30, 0, 1, -1, 0, -1);
    Loaded l = load(entitiesOnly(p.text));
    auto lines = modelEntities<PolylineData>(l.doc());
    REQUIRE(lines.size() == 2);
    CHECK(near(lines[0]->v[0].p, Vec2{0, 0}));
    CHECK(near(lines[0]->v[1].p, Vec2{50, 0}));
    CHECK(near(lines[1]->v[0].p, Vec2{0, -1}));
    CHECK(near(lines[1]->v[1].p, Vec2{51, -1}, 1e-9));
    CHECK(near(lines[1]->v[2].p, Vec2{51, 30}, 1e-9));
}

TEST_CASE("IMAGE: a picture block with the file of its IMAGEDEF, fade and rectangle clip") {
    Pairs ent;
    ent.section("ENTITIES");
    ent.g(0, "IMAGE").g(8, "0").g(100, "AcDbRasterImage").g(10, 100.0).g(20, 200.0).g(30, 0.0);
    ent.g(11, 2.0).g(21, 0.0).g(31, 0.0).g(12, 0.0).g(22, 2.0).g(32, 0.0).g(13, 40.0).g(23, 20.0);
    ent.g(340, "2A").g(70, 7).g(280, 1).g(283, 40).g(71, 1).g(91, 2);
    ent.g(14, 9.5).g(24, 4.5).g(14, 29.5).g(24, 14.5);
    ent.end();
    ent.section("OBJECTS");
    ent.g(0, "IMAGEDEF").g(5, "2A").g(1, "pics\\logo.png").g(10, 40.0).g(20, 20.0);
    Loaded l = load(ent.text + Pairs().end().g(0, "EOF").text);
    const Document& doc = l.doc();
    const InsertData* ins = nullptr;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    CHECK(near(ins->position, Vec2{100, 200}));
    CHECK(ins->scale.x == doctest::Approx(2));
    const BlockDef* b = doc.block(ins->block);
    REQUIRE(b);
    CHECK(b->xrefPath == "pics/logo.png");
    const underlay::ImageParams p = underlay::imageParams(doc.header(), b->name);
    CHECK(p.fade == doctest::Approx(40));
    CHECK(p.u0 == doctest::Approx(0.25));
    CHECK(p.u1 == doctest::Approx(0.75));
    CHECK(p.v0 == doctest::Approx(0.25));
    CHECK(p.v1 == doctest::Approx(0.75));
}

TEST_CASE("PDFUNDERLAY: a PDF page block with the file, page and fade of its PDFDEFINITION") {
    Pairs ent;
    ent.section("ENTITIES");
    ent.g(0, "PDFUNDERLAY").g(8, "0").g(100, "AcDbUnderlayReference").g(340, "3B");
    ent.g(10, 10.0).g(20, 20.0).g(30, 0.0).g(41, 2.0).g(42, 2.0).g(43, 2.0).g(50, 90.0);
    ent.g(281, 60).g(282, 40);
    ent.end();
    ent.section("OBJECTS");
    ent.g(0, "PDFDEFINITION").g(5, "3B").g(1, "plans\\a.pdf").g(2, "3");
    Loaded l = load(ent.text + Pairs().end().g(0, "EOF").text);
    const Document& doc = l.doc();
    const InsertData* ins = nullptr;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    CHECK(near(ins->position, Vec2{10, 20}));
    CHECK(ins->scale.x == doctest::Approx(2));
    CHECK(ins->rotation == doctest::Approx(1.5707963));
    const BlockDef* b = doc.block(ins->block);
    REQUIRE(b);
    const underlay::Spec spec = underlay::parseSpec(b->xrefPath);
    CHECK(spec.file == "plans/a.pdf");
    CHECK(spec.pdf.page == 3);
    CHECK(spec.pdf.fadePercent == doctest::Approx(40));
    CHECK(spec.pdf.contrast == 60);
}

TEST_CASE("TOLERANCE: a feature control frame becomes a frame block at the size of its dim style") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "DIMSTYLE").g(70, 1);
    p.g(0, "DIMSTYLE").g(2, "Big").g(70, 0).g(140, 5.0).g(40, 2.0);
    p.g(0, "ENDTAB").end();
    p.section("ENTITIES");
    p.g(0, "TOLERANCE").g(8, "0").g(100, "AcDbFcf").g(3, "Big").g(10, 50.0).g(20, 60.0).g(30, 0.0);
    p.g(1, "{\\Fgdt;j}%%v%%c0.5{\\Fgdt;m}%%v%%vA%%vB%%v%%v%%v%%v").g(11, 0.0).g(21, 1.0).g(31, 0.0);
    p.end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    const Document& doc = l.doc();
    const InsertData* ins = nullptr;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    CHECK(near(ins->position, Vec2{50, 60}));
    CHECK(ins->rotation == doctest::Approx(3.14159265358979 / 2));
    CHECK(ins->scale.x == doctest::Approx(4));  // 5 mm text x dimscale 2 over the frame's 2.5 mm
    const BlockDef* b = doc.block(ins->block);
    REQUIRE(b);
    const gdt::FrameTable table = gdt::readFrames(doc.header());
    REQUIRE(table.count(b->name) == 1);
    const gdt::Spec& s = table.at(b->name);
    CHECK(s.symbol == gdt::Symbol::Position);
    CHECK(s.diameterZone);
    CHECK(s.material == 'M');
    CHECK(s.value == "0.5");
    CHECK(s.datums == std::vector<std::string>{"A", "B"});
    CHECK(b->entities.size() == gdt::frameEntities(s).size());
}

TEST_CASE("TOLERANCE: text that is not a frame stays a placeholder") {
    Pairs p;
    p.section("ENTITIES");
    p.g(0, "TOLERANCE").g(8, "0").g(3, "Standard").g(10, 0.0).g(20, 0.0).g(30, 0.0).g(1, "rubbish");
    p.end();
    Loaded l = load(p.text + Pairs().g(0, "EOF").text);
    CHECK(modelEntities<InsertData>(l.doc()).empty());
}

TEST_CASE("a text or mtext without a string is counted as empty, not as skipped") {
    Pairs p;
    p.g(0, "TEXT").g(8, "0").g(10, 0.0).g(20, 0.0).g(40, 2.5).g(1, "");
    p.g(0, "MTEXT").g(8, "0").g(10, 0.0).g(20, 0.0).g(40, 2.5).g(1, "");
    p.g(0, "TEXT").g(8, "0").g(10, 0.0).g(20, 0.0).g(40, 2.5).g(1, "kept");
    Loaded l = load(entitiesOnly(p.text));
    const EntityCount& text = l.result.summary.entities.at("TEXT");
    CHECK(text.read == 2);
    CHECK(text.imported == 1);
    CHECK(text.skipped == 0);
    CHECK(text.empty == 1);
    const EntityCount& mtext = l.result.summary.entities.at("MTEXT");
    CHECK(mtext.skipped == 0);
    CHECK(mtext.empty == 1);
    std::string json = l.result.summary.toJson(l.report);
    CHECK(json.find("\"MTEXT\":{\"read\":1,\"imported\":0,\"skipped\":0,\"empty\":1}") !=
          std::string::npos);
}

TEST_CASE("an arc whose start and end angles are equal draws as a full circle") {
    Pairs p;
    p.g(0, "ARC").g(8, "0").g(10, 5.0).g(20, 6.0).g(40, 2.0).g(50, 30.0).g(51, 30.0);
    p.g(0, "ARC").g(8, "0").g(10, 0.0).g(20, 0.0).g(40, 1.0).g(50, 0.0).g(51, 360.0);
    Loaded l = load(entitiesOnly(p.text));
    auto circles = modelEntities<CircleData>(l.doc());
    REQUIRE(circles.size() == 2);
    CHECK(near(circles[0]->center, Vec2{5, 6}));
    CHECK(circles[0]->radius == doctest::Approx(2));
    CHECK(l.result.summary.entities.at("ARC").skipped == 0);
    CHECK(l.report.countCode(code::kBadEntity) == 0);
}

TEST_CASE("dimension text: \\X starts the line under the dimension line, radial text stays put") {
    Pairs p;
    p.g(0, "DIMENSION").g(8, "0").g(10, 10.0).g(20, 5.0).g(11, 5.0).g(21, 5.0).g(70, 0);
    p.g(1, "Y\\XMIN <>mm").g(13, 0.0).g(23, 0.0).g(14, 10.0).g(24, 0.0).g(50, 0.0);
    // Radius dimension with its text inside the circle and no "moved" flag.
    p.g(0, "DIMENSION").g(8, "0").g(10, 100.0).g(20, 0.0).g(11, 103.0).g(21, 1.0).g(70, 4);
    p.g(15, 110.0).g(25, 0.0);
    Loaded l = load(entitiesOnly(p.text));
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 2);
    CHECK(dims[0]->textOverride == "Y\nMIN <>mm");
    // Its text point (5,5) is not where TBCAD would put the text, so the text stays there.
    CHECK(dims[0]->textPosUser);
    CHECK(near(dims[0]->textPos, Vec2{5, 5}));
    CHECK(dims[1]->kind == DimKind::Radius);
    CHECK(dims[1]->textPosUser);
    CHECK(near(dims[1]->textPos, Vec2{103, 1}));
}

TEST_CASE("dimension text point on the dimension line is the text in its own place") {
    // AutoCAD writes group 11 on the dimension line under text lifted above it.
    Pairs p;
    p.g(0, "DIMENSION").g(8, "0").g(10, 100.0).g(20, 10.0).g(11, 50.0).g(21, 10.0).g(70, 0);
    p.g(13, 0.0).g(23, 0.0).g(14, 100.0).g(24, 0.0).g(50, 0.0);
    Loaded l = load(entitiesOnly(p.text));
    auto dims = modelEntities<DimensionData>(l.doc());
    REQUIRE(dims.size() == 1);
    CHECK_FALSE(dims[0]->textPosUser);
    CHECK(near(dims[0]->textPos, Vec2{50, 10}));
}

TEST_CASE("dimension comparison: a block drawn at group 12 and a stale block") {
    // The dimension measures 15 (0,0)-(15,0); its block was made elsewhere for 15000 and is
    // drawn moved by group 12 back onto the dimension.
    Pairs p;
    p.section("BLOCKS");
    p.g(0, "BLOCK").g(8, "0").g(2, "*D1").g(70, 1).g(10, 0.0).g(20, 0.0).g(3, "*D1");
    p.g(0, "LINE").g(8, "0").g(10, 1000.0).g(20, 2005.0).g(11, 1015.0).g(21, 2005.0);
    p.g(0, "MTEXT").g(8, "0").g(10, 1007.5).g(20, 2006.0).g(40, 1.5).g(71, 5).g(1, "15000");
    p.g(0, "ENDBLK").g(8, "0");
    p.end();
    p.section("ENTITIES");
    p.g(0, "DIMENSION").g(8, "0").g(2, "*D1").g(10, 15.0).g(20, 5.0);
    p.g(12, -1000.0).g(22, -2000.0).g(70, 32).g(13, 0.0).g(23, 0.0).g(14, 15.0).g(24, 0.0);
    p.g(50, 0.0);
    p.end().g(0, "EOF");
    ImportOptions compare;
    compare.dimensionCompare = true;
    Loaded l = load(p.text, compare);
    const auto& all = l.result.summary.dimCompare;
    REQUIRE(all.size() == 1);
    const DimComparison& c = all[0];
    CHECK(c.storedText == "15000");
    CHECK(c.stale);
    CHECK(c.detail.find("p12=(-1000,-2000)") != std::string::npos);
    // Moved by group 12, the stored dimension line lies on TBCAD's.
    CHECK(c.strayStored < 0.05);
    const std::string json = l.result.summary.toJson(l.report);
    CHECK(json.find("\"differ\":0") != std::string::npos);
    CHECK(json.find("\"stale\":1") != std::string::npos);
}

}  // namespace tbcad::dxf
