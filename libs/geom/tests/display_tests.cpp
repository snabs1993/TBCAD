#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {

Entity make(EntityData d) {
    Entity e;
    e.data = std::move(d);
    return e;
}

const DisplayPolyline& polyline(const DisplayList& l, std::size_t i = 0) {
    return std::get<DisplayPolyline>(l.items.at(i));
}

}  // namespace

TEST_CASE("arc tessellation deviates less than the tolerance (10,000 random arcs)") {
    std::mt19937 rng(2024);
    std::uniform_real_distribution<double> rad(0.01, 5000), ang(0, kTwoPi), sw(0.01, kTwoPi),
        tolD(0.001, 1);
    for (int i = 0; i < 10000; ++i) {
        double r = rad(rng), tol = tolD(rng);
        auto c = CurveRef::arc({0, 0}, r, ang(rng), sw(rng));
        std::vector<Vec2> pts;
        flatten(c, tol, pts);
        REQUIRE(pts.size() >= 5);
        REQUIRE(pts.size() <= 4097);
        // Chord deviation: distance of each chord's midpoint from the true circle.
        double worst = 0;
        for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            Vec2 m = (pts[k] + pts[k + 1]) * 0.5;
            worst = std::max(worst, r - length(m));
        }
        // The clamp (4096 segments) can only bite for huge radius / tiny tolerance.
        double bound = std::max(tol, r * (1 - std::cos(c.sweep() / 4096 / 2)));
        CHECK(worst <= bound * (1 + 1e-9) + 1e-12);
        CHECK(distance(pts.front(), c.startPoint()) < 1e-12);
        CHECK(distance(pts.back(), c.endPoint()) < 1e-9);
    }
}

TEST_CASE("segment count follows the formula and the clamp") {
    // r = 100, tol = 0.01: acos(1 - 1e-4) = 0.01414; per segment 0.02828 rad -> 223 segments.
    std::vector<Vec2> pts;
    flatten(CurveRef::circle({0, 0}, 100), 0.01, pts);
    CHECK(pts.size() - 1 == 223);
    pts.clear();
    flatten(CurveRef::circle({0, 0}, 1), 100, pts);  // tolerance larger than the radius
    CHECK(pts.size() - 1 == 4);
    pts.clear();
    flatten(CurveRef::circle({0, 0}, 1e9), 1e-6, pts);
    CHECK(pts.size() - 1 == 4096);
}

TEST_CASE("buildDisplay resolves ByLayer, ByBlock and explicit style") {
    Document doc;
    LayerId lay;
    {
        Transaction tx = doc.begin("layer");
        Layer l;
        l.name = "WALLS";
        l.color = Color::rgb(10, 20, 30);
        l.lineweight = LineWeight::mm(0.5);
        auto id = tx.addLayer(l);
        REQUIRE(id.ok());
        lay = id.value();
        Linetype lt;
        lt.name = "DASHED";
        lt.pattern = {6, -3};
        auto lid = tx.addLinetype(lt);
        REQUIRE(lid.ok());
        tx.commit();
    }
    DisplayContext ctx;
    ctx.pixelSizeMm = 0.4;
    DisplayList out;

    Entity line = make(LineData{{0, 0}, {10, 0}});
    line.common.layer = lay;
    buildDisplay(doc, line, ctx, out);
    REQUIRE(out.items.size() == 1);
    const DisplayStyle& st = polyline(out).style;
    CHECK(st.color == Rgba{10, 20, 30, 255});
    CHECK(st.lineweight == doctest::Approx(0.5));
    CHECK(st.pattern == nullptr);
    CHECK(st.layer == lay);

    out.items.clear();
    Entity red = make(LineData{{0, 0}, {10, 0}});
    red.common.layer = lay;
    red.common.color = Color::index(1);
    red.common.lineweight = LineWeight::mm(0.13);
    red.common.linetype = doc.linetypeByName("DASHED")->id;
    red.common.linetypeScale = 2;
    buildDisplay(doc, red, ctx, out);
    const DisplayStyle& s2 = polyline(out).style;
    CHECK(s2.color == paletteColor(1));
    CHECK(s2.lineweight == doctest::Approx(0.13));
    REQUIRE(s2.pattern != nullptr);
    CHECK(*s2.pattern == std::vector<double>{6, -3});
    CHECK(s2.patternScale == doctest::Approx(2));

    out.items.clear();
    Entity byBlock = make(LineData{{0, 0}, {10, 0}});
    byBlock.common.color = Color::byBlock();
    byBlock.common.lineweight = LineWeight::byBlock();
    byBlock.common.linetype = kLinetypeByBlock;
    DisplayContext inner = ctx;
    inner.byBlockColor = Rgba{1, 2, 3, 255};
    inner.byBlockLineweightMm = 0.7;
    static const std::vector<double> kPattern = {1, -1};
    inner.byBlockPattern = &kPattern;
    buildDisplay(doc, byBlock, inner, out);
    const DisplayStyle& s3 = polyline(out).style;
    CHECK(s3.color == Rgba{1, 2, 3, 255});
    CHECK(s3.lineweight == doctest::Approx(0.7));
    CHECK(s3.pattern == &kPattern);

    out.items.clear();
    Entity dflt = make(LineData{{0, 0}, {1, 0}});
    dflt.common.lineweight = LineWeight::defaultWeight();
    buildDisplay(doc, dflt, ctx, out);
    CHECK(polyline(out).style.lineweight == doctest::Approx(ctx.defaultLineweightMm));
}

TEST_CASE("built-in entities produce the expected display items") {
    Document doc;
    DisplayContext ctx;
    ctx.pixelSizeMm = 0.04;  // tolerance 0.01
    DisplayList out;

    buildDisplay(doc, make(CircleData{{0, 0}, 10}), ctx, out);
    REQUIRE(out.items.size() == 1);
    CHECK(polyline(out).closed);
    CHECK(polyline(out).pts.size() > 20);

    out.items.clear();
    buildDisplay(doc, make(ArcData{{0, 0}, 10, 0, kPi}), ctx, out);
    CHECK_FALSE(polyline(out).closed);
    CHECK(distance(polyline(out).pts.back(), Vec2{-10, 0}) < 1e-9);

    out.items.clear();
    buildDisplay(doc, make(PointData{{3, 4}}), ctx, out);
    REQUIRE(std::holds_alternative<DisplayPoints>(out.items[0]));

    out.items.clear();
    EllipseData el;
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    buildDisplay(doc, make(el), ctx, out);
    CHECK(polyline(out).closed);

    out.items.clear();
    SplineData sp = interpolateSpline(std::vector<Vec2>{{0, 0}, {5, 5}, {10, 0}}).value();
    buildDisplay(doc, make(sp), ctx, out);
    CHECK_FALSE(polyline(out).closed);
    CHECK(polyline(out).pts.size() > 10);
    CHECK(distance(polyline(out).pts.back(), Vec2{10, 0}) < 1e-9);

    // Proxy graphics are passed through.
    out.items.clear();
    ProxyData px;
    px.originalType = "FOO";
    DisplayPolyline pp;
    pp.pts = {{0, 0}, {1, 1}};
    px.graphics.items.push_back(pp);
    buildDisplay(doc, make(px), ctx, out);
    REQUIRE(out.items.size() == 1);
    CHECK(bounds(make(px)).max.x == 1);
}

TEST_CASE("polyline display: arcs, closing, widths") {
    Document doc;
    DisplayContext ctx;
    ctx.pixelSizeMm = 0.04;
    DisplayList out;
    // Rectangle with one rounded side.
    PolylineData pl;
    pl.v = {{{0, 0}, 1.0}, {{10, 0}, 0}, {{10, 5}, 0}, {{0, 5}, 0}};
    pl.closed = true;
    buildDisplay(doc, make(pl), ctx, out);
    REQUIRE(out.items.size() == 1);
    const auto& d = polyline(out);
    CHECK(d.closed);
    CHECK(d.pts.size() > 10);
    double minY = 0;
    for (const Vec2& p : d.pts)
        minY = std::min(minY, p.y);
    CHECK(minY == doctest::Approx(-5).epsilon(1e-2));

    // Constant width: a filled strip per segment.
    out.items.clear();
    PolylineData w;
    w.v = {{{0, 0}, 0}, {{10, 0}, 0}};
    w.constantWidth = 2;
    buildDisplay(doc, make(w), ctx, out);
    REQUIRE(out.items.size() == 1);
    REQUIRE(std::holds_alternative<DisplayFill>(out.items[0]));
    const auto& ring = std::get<DisplayFill>(out.items[0]).rings.at(0);
    BBox2 b;
    for (const Vec2& p : ring)
        b.expand(p);
    CHECK(b.min.y == doctest::Approx(-1));
    CHECK(b.max.y == doctest::Approx(1));
    CHECK(bounds(make(w)).min.y == doctest::Approx(-1));

    // Tapered width.
    out.items.clear();
    PolylineData t;
    t.v = {{{0, 0}, 0, 0, 4}, {{10, 0}, 0}};
    buildDisplay(doc, make(t), ctx, out);
    const auto& tring = std::get<DisplayFill>(out.items[0]).rings.at(0);
    BBox2 tb;
    for (const Vec2& p : tring)
        tb.expand(p);
    CHECK(tb.max.y == doctest::Approx(2));
}

TEST_CASE("wide polyline corners are mitered, including the closing corner") {
    Document doc;
    DisplayContext ctx;
    PolylineData sq;
    sq.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}, {{0, 10}, 0}};
    sq.closed = true;
    sq.constantWidth = 2;
    DisplayList out;
    buildDisplay(doc, make(sq), ctx, out);
    // The outer corners of the square (-1,-1), (11,-1), (11,11), (-1,11) are all covered: no notch.
    for (Vec2 corner : {Vec2{-1, -1}, Vec2{11, -1}, Vec2{11, 11}, Vec2{-1, 11}}) {
        bool covered = false;
        for (const DisplayItem& it : out.items)
            if (const auto* fill = std::get_if<DisplayFill>(&it))
                for (const auto& ring : fill->rings)
                    for (const Vec2& p : ring)
                        covered = covered || distance(p, corner) < 1e-9;
        CHECK(covered);
    }

    // An open polyline has no join at its two free ends.
    out.items.clear();
    PolylineData open;
    open.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}};
    open.constantWidth = 2;
    buildDisplay(doc, make(open), ctx, out);
    CHECK(out.items.size() == 3);  // two strips and the one corner
}

TEST_CASE("registry supplies display and bounds for other kinds") {
    Document doc;
    DisplayRegistry::clear(EntityKind::Text);
    CHECK_FALSE(DisplayRegistry::has(EntityKind::Text));
    TextData td;
    td.position = {5, 5};
    Entity text = make(td);
    DisplayList out;
    buildDisplay(doc, text, DisplayContext{}, out);
    CHECK(out.items.empty());
    CHECK(bounds(text, doc).empty());

    DisplayRegistry::set(EntityKind::Text, [](const Document&, const Entity& e,
                                              const DisplayContext&, DisplayList& l) {
        const auto& t = *entityAs<TextData>(e);
        DisplayPolyline p;
        p.pts = {t.position, t.position + Vec2{t.height * 4, 0},
                 t.position + Vec2{t.height * 4, t.height}};
        l.items.push_back(p);
    });
    CHECK(DisplayRegistry::has(EntityKind::Text));
    buildDisplay(doc, text, DisplayContext{}, out);
    CHECK(out.items.size() == 1);
    BBox2 b = bounds(text, doc);  // derived from the display list
    CHECK(b.max.x == doctest::Approx(15));
    CHECK(b.max.y == doctest::Approx(7.5));
    // Distance and hit test use the generated geometry when the document is given.
    CHECK(distance(text, {10, 5}, &doc) == doctest::Approx(0));
    CHECK(distance(text, {10, 5}) == std::numeric_limits<double>::infinity());
    CHECK(hitTest(text, {10, 5.3}, 0.5, &doc));

    // Explicit bounds function wins.
    DisplayRegistry::set(
        EntityKind::Text,
        [](const Document&, const Entity&, const DisplayContext&, DisplayList&) {},
        [](const Document&, const Entity&) {
            BBox2 b;
            b.expand({-1, -1});
            b.expand({1, 1});
            return b;
        });
    CHECK(bounds(text, doc).max.x == 1);
    // Built-in kinds cannot be overridden.
    DisplayRegistry::set(EntityKind::Line, [](const Document&, const Entity&, const DisplayContext&,
                                              DisplayList&) {});
    DisplayList l2;
    buildDisplay(doc, make(LineData{{0, 0}, {1, 1}}), DisplayContext{}, l2);
    CHECK(l2.items.size() == 1);
    DisplayRegistry::clear(EntityKind::Text);
}

TEST_CASE("buildDisplay applies layer and object transparency, and the TPY switch") {
    Document doc;
    LayerId lay;
    {
        Transaction tx = doc.begin("layer");
        Layer l;
        l.name = "GLASS";
        l.color = Color::rgb(10, 20, 30);
        l.transparency = 50;
        auto id = tx.addLayer(l);
        REQUIRE(id.ok());
        lay = id.value();
        tx.commit();
    }
    DisplayContext ctx;
    auto alphaOf = [&](const Entity& e, const DisplayContext& c) {
        DisplayList out;
        buildDisplay(doc, e, c, out);
        return polyline(out).style.color.a;
    };
    Entity line = make(LineData{{0, 0}, {10, 0}});
    line.common.layer = lay;
    CHECK(alphaOf(line, ctx) == 128);  // ByLayer: the layer's 50 %
    line.common.transparency = 0;
    CHECK(alphaOf(line, ctx) == 255);  // opaque overrides the layer
    line.common.transparency = 90;
    CHECK(alphaOf(line, ctx) == 26);
    line.common.transparency = kTransparencyByBlock;
    ctx.byBlockTransparency = 20;
    CHECK(alphaOf(line, ctx) == 204);
    ctx.showTransparency = false;
    CHECK(alphaOf(line, ctx) == 255);
}

TEST_CASE("a polyline with Ltype gen off draws each segment on its own so the pattern restarts") {
    Document doc;
    LinetypeId dashed;
    {
        Transaction tx = doc.begin("lt");
        Linetype lt;
        lt.name = "DASHED";
        lt.pattern = {6, -3};
        auto lid = tx.addLinetype(lt);
        REQUIRE(lid.ok());
        dashed = lid.value();
        tx.commit();
    }
    PolylineData pl;
    pl.v = {{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0.5, 0, 0}, {{20, 10}, 0, 0, 0}};
    Entity e = make(pl);
    e.common.linetype = dashed;
    DisplayContext ctx;
    ctx.pixelSizeMm = 0.4;
    DisplayList on;
    buildDisplay(doc, e, ctx, on);
    CHECK(on.items.size() == 1);  // one run: the pattern continues round the corners
    PolylineData off = pl;
    off.restartLinetype = true;
    Entity e2 = make(off);
    e2.common.linetype = dashed;
    DisplayList split;
    buildDisplay(doc, e2, ctx, split);
    CHECK(split.items.size() == 3);  // three segments
    // Continuous lines are unaffected: nothing to restart.
    Entity solid = make(off);
    DisplayList cont;
    buildDisplay(doc, solid, ctx, cont);
    CHECK(cont.items.size() == 1);
}

namespace {
void addDashed(Document& doc, LinetypeId& dashed) {
    Transaction tx = doc.begin("lt");
    Linetype lt;
    lt.name = "DASHED";
    lt.pattern = {6, -3};
    dashed = tx.addLinetype(lt).value();
    tx.commit();
}
}  // namespace

TEST_CASE("linetype scale follows MSLTSCALE in model space and PSLTSCALE in a viewport") {
    LinetypeId dashed;
    Document doc;
    addDashed(doc, dashed);
    {
        Transaction tx = doc.begin("scale");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.ltScale = 2;
                      h.custom[kAnnoScaleKey] = "50";
                  }).ok());
        tx.commit();
    }
    Entity line = make(LineData{{0, 0}, {100, 0}});
    line.common.linetype = dashed;
    line.common.linetypeScale = 3;
    auto scaleWith = [&](double annoScale, bool paper) {
        DisplayContext ctx;
        ctx.annoScale = annoScale;
        ctx.paperSpace = paper;
        DisplayList out;
        buildDisplay(doc, line, ctx, out);
        return polyline(out).style.patternScale;
    };
    // Model space: object scale 3 x LTSCALE 2 x annotation scale 50.
    CHECK(scaleWith(0, false) == doctest::Approx(300));
    // A viewport at 1:100: PSLTSCALE on gives the same dashes on paper at every viewport scale.
    CHECK(scaleWith(100, false) == doctest::Approx(600));
    // The sheet's own objects are not scaled by the annotation scale.
    CHECK(scaleWith(0, true) == doctest::Approx(6));
    {
        Transaction tx = doc.begin("off");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom[kMsLtScaleKey] = "0";
                      h.custom[kPsLtScaleKey] = "0";
                  }).ok());
        tx.commit();
    }
    CHECK(scaleWith(0, false) == doctest::Approx(6));
    CHECK(scaleWith(100, false) == doctest::Approx(6));
}

TEST_CASE("CELTSCALE reaches new objects") {
    Header h;
    h.custom[kCeLtScaleKey] = "2.5";
    Entity e = make(LineData{{0, 0}, {1, 0}});
    applyCurrentProperties(h, e);
    CHECK(e.common.linetypeScale == doctest::Approx(2.5));
    CHECK(ceLtScale(Header{}) == 1);
}

TEST_CASE("PLINEGEN is the default of new polylines; a dashed polyline restarts at each vertex") {
    LinetypeId dashed;
    Document doc;
    addDashed(doc, dashed);
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}, {{0, 10}, 0}};
    auto count = [&](const Entity& e) {
        DisplayList out;
        buildDisplay(doc, e, DisplayContext{}, out);
        return out.items.size();
    };
    Entity open = make(pl);
    open.common.linetype = dashed;
    CHECK(count(open) == 1);
    open.data = [&] {
        PolylineData r = pl;
        r.restartLinetype = true;
        return r;
    }();
    CHECK(count(open) == 3);

    // New polylines take the drawing's PLINEGEN.
    Header h;
    Entity fresh = make(pl);
    applyCurrentProperties(h, fresh);
    CHECK_FALSE(entityAs<PolylineData>(fresh)->restartLinetype);
    h.custom[kPlineGenKey] = "0";
    applyCurrentProperties(h, fresh);
    CHECK(entityAs<PolylineData>(fresh)->restartLinetype);
}
