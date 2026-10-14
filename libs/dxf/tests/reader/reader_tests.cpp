// Unit tests of the DXF reader: geometry mapping, OCS, units, tables, blocks.
#include <algorithm>
#include <cmath>

#include "reader_helpers.h"
#include "tbcad/geom/entity.h"

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
    REQUIRE(ellipses.size() == 1);
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

TEST_CASE("extrusion: a tilted plane is skipped with a warning") {
    Loaded l = loadCorpus("extrusion_neg_z.dxf");
    CHECK(l.report.countCode(code::kExtrusion3d) == 1);
}

TEST_CASE("extrusion -Z: INSERT array positions are mirrored before and after rotation") {
    Pairs p;
    p.section("BLOCKS").g(0, "BLOCK").g(2, "B").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 0.0).g(0, "ENDBLK").end();
    p.section("ENTITIES").g(0, "INSERT").g(8, "0").g(2, "B").g(10, 1.0).g(20, 2.0).g(50, 90.0);
    p.g(70, 2).g(71, 1).g(44, 5.0).g(45, 0.0).g(210, 0.0).g(220, 0.0).g(230, -1.0);
    Loaded l = load(p.text + Pairs().end().g(0, "EOF").text);
    auto inserts = modelEntities<InsertData>(l.doc());
    REQUIRE(inserts.size() == 2);
    // Column offset (5, 0) rotated by 90 degrees is (0, 5); the whole thing is mirrored in x.
    CHECK(near(inserts[0]->position, Vec2{-1, 2}));
    CHECK(near(inserts[1]->position, Vec2{-1, 7}));
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
}

TEST_CASE("units: a file without $INSUNITS is taken as millimetres and says so") {
    Loaded l = load(entitiesOnly(
        Pairs().g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 3.0).g(21, 4.0).text));
    CHECK(l.result.summary.unitsAssumed);
    CHECK(l.result.summary.unitScale == 1.0);
    CHECK(l.report.countCode(code::kUnitsAssumed) == 1);
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

TEST_CASE("text styles: width factor, oblique and font file are kept") {
    Pairs p;
    p.section("TABLES").g(0, "TABLE").g(2, "STYLE").g(70, 1);
    p.g(0, "STYLE").g(2, "Narrow").g(70, 0).g(40, 0.0).g(41, 0.8).g(50, 15.0).g(3, "arial.ttf");
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
    CHECK(l.report.countCode(code::kShxFont) == 2);
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

TEST_CASE("polylines: old-style POLYLINE reads its VERTEX records and skips 3D polylines") {
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
    p.g(0, "LINE").g(8, "0").g(10, 0.0).g(20, 0.0).g(11, 1.0).g(21, 1.0);
    Loaded l = load(entitiesOnly(p.text));
    auto polys = modelEntities<PolylineData>(l.doc());
    REQUIRE(polys.size() == 1);
    CHECK(polys[0]->closed);
    CHECK(near(polys[0]->v[0].bulge, 1.0));
    CHECK(modelEntities<LineData>(l.doc()).size() == 1);  // the entity after SEQEND still loads
    CHECK(l.report.countCode(code::kBadEntity) == 1);
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
    REQUIRE(inserts.size() == 8);
    // 2 rows x 3 columns of INNER at spacing 10
    std::vector<Vec2> pos;
    for (const InsertData* i : inserts) {
        if (d.block(i->block)->name == "INNER")
            pos.push_back(i->position);
    }
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
    p.section("BLOCKS").g(0, "BLOCK").g(2, "*Paper_Space").g(70, 0).g(10, 0.0).g(20, 0.0).g(30, 0.0);
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
    CHECK(json.find("\"3DSOLID\":{\"read\":2,\"imported\":0,\"skipped\":2}") != std::string::npos);
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

}  // namespace tbcad::dxf
