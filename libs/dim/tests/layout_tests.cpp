#include <functional>

#include "support.h"
#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

namespace {

const Layout::Line* findExt(const Layout& L, Vec2 from) {
    for (const auto& l : L.lines)
        if (l.role == Role::ExtLine && near(l.pts.front(), from))
            return &l;
    return nullptr;
}

}  // namespace

TEST_CASE(
    "horizontal linear dimension: extension lines, dimension line, arrows inside, text above") {
    Document doc;
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(count(L, Role::ExtLine) == 2);
    const auto* e1 = findExt(L, {0, 0.625});
    REQUIRE(e1);
    CHECK(near(e1->pts.back(), {0, 11.25}));
    const auto* e2 = findExt(L, {100, 0.625});
    REQUIRE(e2);
    CHECK(near(e2->pts.back(), {100, 11.25}));

    // One dimension line from end to end, at y = 10; two arrows whose tips touch its ends.
    REQUIRE(count(L, Role::DimLine) == 1);
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine) {
            CHECK(near(l.pts.front(), {0, 10}));
            CHECK(near(l.pts.back(), {100, 10}));
        }
    REQUIRE(L.arrows.size() == 2);
    BBox2 a = arrowBox(L.arrows[0]), b = arrowBox(L.arrows[1]);
    CHECK(a.min.x == doctest::Approx(0));
    CHECK(a.max.x == doctest::Approx(2.5));
    CHECK(b.max.x == doctest::Approx(100));
    CHECK(b.min.x == doctest::Approx(97.5));

    REQUIRE(L.text.valid());
    CHECK(L.text.utf8 == "100.00");
    CHECK(L.text.center.x == doctest::Approx(50));
    CHECK(L.text.center.y == doctest::Approx(10 + 0.625 + 1.25));  // gap + half the text height
    CHECK(L.text.rotation == doctest::Approx(0));
}

TEST_CASE("centered text breaks the dimension line") {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.textPlacement = DimTextPlacement::Centered; }));
    tx.commit();
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(count(L, Role::DimLine) == 2);
    CHECK(L.text.center.y == doctest::Approx(10));
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine) {
            double lo = std::min(l.pts.front().x, l.pts.back().x);
            double hi = std::max(l.pts.front().x, l.pts.back().x);
            CHECK((hi <= 50 - L.text.width / 2 + 1e-9 || lo >= 50 + L.text.width / 2 - 1e-9));
        }
}

TEST_CASE("text placed below hangs under the dimension line") {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.textPlacement = DimTextPlacement::Below; }));
    tx.commit();
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(count(L, Role::DimLine) == 1);
    CHECK(L.text.center.y == doctest::Approx(10 - 0.625 - 1.25));
}

TEST_CASE("fit: arrows move outside, then text, when the extension lines are close") {
    Document doc;
    // Text fits between the lines but the arrows do not fit beside it.
    const double tw = text::measureWidth(text::styleFor(doc, {}), "0.00", 2.5);
    const double mid_len = tw + 2.5 + 0.1;  // > tw + 2 * gap, < tw + 2 * gap + 2 * arrow
    Layout mid = layoutDimension(doc, linear({0, 0}, {mid_len, 0}, {mid_len / 2, 10}));
    REQUIRE(mid.arrows.size() == 2);
    CHECK(mid.text.center.x == doctest::Approx(mid_len / 2));  // text inside
    CHECK(arrowBox(mid.arrows[0]).max.x <= 1e-9);              // arrow at the left end sits outside
    CHECK(arrowBox(mid.arrows[1]).min.x >= mid_len - 1e-9);

    // 3 mm: neither fits; arrows outside and the text beyond the right-hand arrow.
    Layout small = layoutDimension(doc, linear({0, 0}, {3, 0}, {1.5, 10}));
    CHECK(arrowBox(small.arrows[0]).max.x <= 1e-9);
    CHECK(small.text.center.x - small.text.width / 2 > 3);

    // 6 mm: arrows fit inside, the text does not and goes outside.
    Layout arrows = layoutDimension(doc, linear({0, 0}, {6, 0}, {3, 10}));
    CHECK(arrowBox(arrows.arrows[0]).min.x >= -1e-9);
    CHECK(arrowBox(arrows.arrows[0]).max.x <= 6.0);
    CHECK(arrows.text.center.x - arrows.text.width / 2 > 6);

    // The text goes beside the second extension line: measured from right to left, it sits
    // left of the dimension, and the dimension line runs out to it.
    Layout back = layoutDimension(doc, linear({6, 0}, {0, 0}, {3, 10}));
    CHECK(back.text.center.x + back.text.width / 2 < 0);
    double leftmost = 0;
    for (const auto& l : back.lines)
        if (l.role == Role::DimLine)
            for (Vec2 p : l.pts)
                leftmost = std::min(leftmost, p.x);
    CHECK(leftmost < -back.text.width / 2);
}

TEST_CASE("vertical and aligned dimensions") {
    Document doc;
    Layout v = layoutDimension(doc, linear({0, 0}, {0, 50}, {10, 25}, kPi / 2));
    CHECK(v.text.utf8 == "50.00");
    CHECK(v.text.rotation == doctest::Approx(kPi / 2));  // reads bottom to top

    Layout al = layoutDimension(doc, aligned({0, 0}, {30, 40}, {-24, 18}));
    CHECK(al.text.utf8 == "50.00");
    CHECK(al.text.rotation == doctest::Approx(std::atan2(40, 30)));
    // Dimension line is parallel to the points and passes the picked location.
    for (const auto& l : al.lines)
        if (l.role == Role::DimLine) {
            Vec2 dir = normalized(l.pts.back() - l.pts.front());
            CHECK(std::abs(cross(dir, Vec2{0.6, 0.8})) < 1e-9);
        }
}

TEST_CASE("text stays readable for dimension lines pointing left, and can stay horizontal") {
    Document doc;
    Layout l = layoutDimension(doc, linear({100, 0}, {0, 0}, {50, 10}));
    CHECK(l.text.rotation == doctest::Approx(0));
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.textAlign = DimTextAlign::Horizontal; }));
    tx.commit();
    Layout v = layoutDimension(doc, linear({0, 0}, {0, 50}, {10, 25}, kPi / 2));
    CHECK(v.text.rotation == doctest::Approx(0));
    CHECK(count(v, Role::DimLine) == 2);  // horizontal text on a vertical line breaks it
}

TEST_CASE("radius and diameter") {
    Document doc;
    Layout r = layoutDimension(doc, radial(DimKind::Radius, {0, 0}, {10, 0}));
    CHECK(r.text.utf8 == "R10.00");
    CHECK(r.text.center.x > 10);
    REQUIRE(r.arrows.size() == 1);
    CHECK(arrowBox(r.arrows[0]).max.x == doctest::Approx(10));

    Layout d = layoutDimension(doc, radial(DimKind::Diameter, {-20, 0}, {20, 0}));
    CHECK(d.text.utf8 ==
          "\xC3\x98"
          "40.00");
    CHECK(std::abs(d.text.center.x) < 1e-9);  // inside, in the middle
    CHECK(d.arrows.size() == 2);

    Layout small = layoutDimension(doc, radial(DimKind::Diameter, {-3, 0}, {3, 0}));
    CHECK(small.text.center.x > 3);  // does not fit: outside
}

TEST_CASE("angular dimension: arc, extension lines, arrows, text on the arc") {
    Document doc;
    DimensionData d = angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15});
    Layout L = layoutDimension(doc, d);
    REQUIRE(L.arcs.size() == 1);
    const double r = std::sqrt(450.0);
    CHECK(L.arcs[0].radius == doctest::Approx(r));
    CHECK(L.arcs[0].startAngle == doctest::Approx(0));
    CHECK(L.arcs[0].endAngle == doctest::Approx(kPi / 2));
    CHECK(count(L, Role::ExtLine) == 2);
    CHECK(L.arrows.size() == 2);
    CHECK(L.text.utf8 == "90.00\xC2\xB0");
    CHECK(L.text.center.x > 0);
    CHECK(L.text.center.y > 0);

    // The arc is inside the lines: no extension lines.
    DimensionData inside = angular2({0, 0}, {20, 0}, {0, 0}, {0, 20}, {5, 5});
    CHECK(count(layoutDimension(doc, inside), Role::ExtLine) == 0);
}

// As in AutoCAD: text on the arc that fits there, but not together with the arrows, sends the
// arrows outside (best fit); with "text first" the arrows stay in.
TEST_CASE("angular dimension: arrows go outside when the text takes the room on the arc") {
    Document doc;
    auto sweepOf = [](const Layout& L) {
        double total = 0;
        for (const auto& a : L.arcs)
            total += normalizeAngle(a.endAngle - a.startAngle);
        return total;
    };
    // The text's width, from an arc with room for everything.
    const Layout roomy = layoutDimension(doc, angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {50, 50}));
    CHECK(sweepOf(roomy) == doctest::Approx(kPi / 2));
    const double textW = roomy.text.width + 2 * 0.625;
    // An arc as long as the text and one arrow: the text fits, the arrows do not.
    const double r = (textW + 2.5) / (kPi / 2);
    const DimensionData d =
        angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {r / std::sqrt(2.0), r / std::sqrt(2.0)});
    CHECK(sweepOf(layoutDimension(doc, d)) > kPi / 2 + 0.1);

    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.fit = DimFit::TextFirst; }));
    tx.commit();
    CHECK(sweepOf(layoutDimension(doc, d)) == doctest::Approx(kPi / 2));
}

// AutoCAD: a text style with a fixed height sets the dimension text's height; DIMTXT and DIMSCALE
// do not apply to it (the arrows still scale).
TEST_CASE("a fixed-height text style sets the dimension text height") {
    Document doc;
    Transaction tx = doc.begin("styles");
    TextStyle fixed;
    fixed.name = "Fixed";
    fixed.height = 7;
    auto ts = tx.addTextStyle(fixed);
    REQUIRE(ts);
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) {
        s.textStyle = ts.value();
        s.overallScale = 2;
    }));
    tx.commit();
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(L.text.height == doctest::Approx(7));
    CHECK(arrowBox(L.arrows[0]).width() == doctest::Approx(5));
}

TEST_CASE("DIMSCALE scales text and arrows but not the measurement") {
    Document doc;
    DimensionData d = linear({0, 0}, {10000, 0}, {5000, 1000});
    Layout one = layoutDimension(doc, d);
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.overallScale = 100; }));
    tx.commit();
    Layout big = layoutDimension(doc, d);
    CHECK(big.text.utf8 == one.text.utf8);
    CHECK(big.text.utf8 == "10000.00");
    CHECK(big.text.height == doctest::Approx(one.text.height * 100));
    BBox2 a1 = arrowBox(one.arrows[0]), a2 = arrowBox(big.arrows[0]);
    CHECK(a2.width() == doctest::Approx(a1.width() * 100));
    CHECK(measurement(d) == doctest::Approx(10000));
}

TEST_CASE("arrow types") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    auto with = [&](ArrowType t) {
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) { s.arrowType = t; }));
        tx.commit();
        return layoutDimension(doc, d);
    };
    CHECK(with(ArrowType::ClosedFilled).arrows.size() == 2);
    Layout open = with(ArrowType::Open30);
    CHECK(open.arrows.empty());
    CHECK(open.lines.size() == 5);  // 2 extension lines, dimension line, 2 arrowheads
    Layout tick = with(ArrowType::ArchTick);
    CHECK(tick.arrows.empty());
    CHECK(tick.lines.size() == 5);
    CHECK(with(ArrowType::Dot).arrows.size() == 2);
    Layout none = with(ArrowType::None);
    CHECK(none.arrows.empty());
    CHECK(none.lines.size() == 3);
}

TEST_CASE("user-placed text outside the extension lines gets a short leader") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    d.textPosUser = true;
    d.textPos = {140, 10};
    Layout L = layoutDimension(doc, d);
    CHECK(L.text.center.x == doctest::Approx(140));
    bool leader = false;
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine && l.pts.back().x > 100)
            leader = true;
    CHECK(leader);
}

TEST_CASE("leader: arrow at the first vertex, landing, text") {
    Document doc;
    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}, {20, 10}};
    ld.textPos = {23, 10};
    ld.utf8 = "Note";
    Layout L = layoutLeader(doc, ld);
    REQUIRE(L.arrows.size() == 1);
    CHECK(std::abs(arrowBox(L.arrows[0]).min.x) < 1e-9);
    CHECK(L.lines.size() == 2);  // the polyline and the landing
    CHECK(near(L.lines[1].pts.front(), {20, 10}));
    CHECK(near(L.lines[1].pts.back(), {22.5, 10}));
    REQUIRE(L.text.valid());
    CHECK(L.text.mtext);
    CHECK(L.text.utf8 == "Note");
    CHECK(near(L.text.center, {23, 10}));
    CHECK(!L.text.attachRight);

    ld.textPos = {-3, 10};
    CHECK(layoutLeader(doc, ld).text.attachRight);
    LeaderData tiny;
    tiny.vertices = {{0, 0}};
    CHECK(layoutLeader(doc, tiny).lines.empty());
}

TEST_CASE("refresh recomputes the measurement and the default text position") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    refresh(doc, d);
    CHECK(d.measurement == doctest::Approx(100));
    CHECK(near(d.textPos, {50, 10}));  // on the dimension line, under the text
    d.textPosUser = true;
    d.textPos = {1, 2};
    d.defPts[1] = {200, 0};
    refresh(doc, d);
    CHECK(d.measurement == doctest::Approx(200));
    CHECK(near(d.textPos, {1, 2}));
}

TEST_CASE("a dimension text override can hold fields") {
    Document doc;
    {
        Transaction tx = doc.begin("sheet");
        REQUIRE(tx.modifyHeader([](Header& h) { h.custom["sheet.number"] = "A-101"; }));
        tx.commit();
    }
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    d.textOverride = "<> (%<sheet>%)";
    Layout L = layoutDimension(doc, d);
    CHECK(L.text.utf8 == "100.00 (A-101)");
}

namespace {
Layout withStyle(const std::function<void(DimStyle&)>& edit, const DimensionData& d) {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, edit));
    tx.commit();
    return layoutDimension(doc, d);
}
}  // namespace

TEST_CASE("DIMFXLON: extension lines of a fixed length from the dimension line") {
    // Dimension line at y = 10: a fixed length of 4 starts the extension lines at y = 6 and they
    // still reach DIMEXE beyond the dimension line.
    const auto d = linear({0, 0}, {100, 0}, {50, 10});
    Layout fixed = withStyle(
        [](DimStyle& s) {
            s.fixedExtLines = true;
            s.fixedExtLength = 4;
        },
        d);
    REQUIRE(count(fixed, Role::ExtLine) == 2);
    const auto* e1 = findExt(fixed, {0, 6});
    REQUIRE(e1);
    CHECK(near(e1->pts.back(), {0, 11.25}));
    CHECK(findExt(fixed, {100, 6}));
    // Longer than the distance to the origin: the offset from the origin still applies.
    Layout tooLong = withStyle(
        [](DimStyle& s) {
            s.fixedExtLines = true;
            s.fixedExtLength = 50;
        },
        d);
    CHECK(findExt(tooLong, {0, 0.625}));
    // Off: the length is ignored.
    Layout off = withStyle([](DimStyle& s) { s.fixedExtLength = 4; }, d);
    CHECK(findExt(off, {0, 0.625}));
}

TEST_CASE("DIMSE1/2 drop extension lines, DIMSD1/2 drop half the dimension line and its arrow") {
    const auto d = linear({0, 0}, {100, 0}, {50, 10});
    Layout se = withStyle([](DimStyle& s) { s.suppressExt1 = true; }, d);
    CHECK(count(se, Role::ExtLine) == 1);
    CHECK(findExt(se, {100, 0.625}));
    Layout sd = withStyle([](DimStyle& s) { s.suppressDim2 = true; }, d);
    REQUIRE(sd.arrows.size() == 1);
    CHECK(arrowBox(sd.arrows[0]).min.x == doctest::Approx(0));
    for (const auto& l : sd.lines)
        if (l.role == Role::DimLine) {
            const double hi = std::max(l.pts.front().x, l.pts.back().x);
            CHECK(hi <= 50 + 1e-9);
        }
}

TEST_CASE("DIMJUST puts the text next to an extension line") {
    const auto d = linear({0, 0}, {100, 0}, {50, 10});
    Layout c = withStyle([](DimStyle&) {}, d);
    Layout one = withStyle([](DimStyle& s) { s.textJustify = 1; }, d);
    Layout two = withStyle([](DimStyle& s) { s.textJustify = 2; }, d);
    CHECK(c.text.center.x == doctest::Approx(50));
    CHECK(one.text.center.x < 30);
    CHECK(two.text.center.x > 70);
}

TEST_CASE(
    "DIMTIX keeps the text inside, DIMSOXD drops arrows that do not fit, DIMTOFL off "
    "leaves the inside empty") {
    Document probe;
    const double tw = text::measureWidth(text::styleFor(probe, {}), "0.00", 2.5);
    const double len = tw + 2.5 + 0.1;  // text fits, arrows do not (see the fit test)
    const auto d = linear({0, 0}, {len, 0}, {len / 2, 10});
    Layout soxd = withStyle([](DimStyle& s) { s.suppressArrowsOutside = true; }, d);
    CHECK(soxd.arrows.empty());
    Layout tofl = withStyle([](DimStyle& s) { s.forceLineInside = false; }, d);
    for (const auto& l : tofl.lines)
        if (l.role == Role::DimLine) {
            const double lo = std::min(l.pts.front().x, l.pts.back().x);
            const double hi = std::max(l.pts.front().x, l.pts.back().x);
            CHECK((hi <= 1e-9 || lo >= len - 1e-9));
        }
    // 3 mm: the text would go outside; DIMTIX keeps it between the extension lines.
    const auto small = linear({0, 0}, {3, 0}, {1.5, 10});
    Layout out = withStyle([](DimStyle&) {}, small);
    Layout tix = withStyle([](DimStyle& s) { s.textInsideForced = true; }, small);
    CHECK(out.text.center.x > 3);
    CHECK(tix.text.center.x == doctest::Approx(1.5));
}

TEST_CASE("DIMTOH sets the orientation of text beyond the extension lines apart from DIMTIH") {
    const auto d = linear({0, 0}, {3, 3}, {1.5, 1.5});
    auto rot = [&](bool ih, bool oh) {
        Document doc;
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) {
            s.textAlign = ih ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
            s.textAlignOutside = oh ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
        }));
        tx.commit();
        auto a = aligned({0, 0}, {3, 0}, {1.5, 8});  // 3 mm: the text goes outside
        a.defPts[1] = {3, 3};
        return layoutDimension(doc, a).text.rotation;
    };
    (void)d;
    CHECK(rot(false, true) == doctest::Approx(0));
    CHECK(std::abs(rot(true, false)) > 0.1);
}

TEST_CASE("DIMTOH also turns the text of a radius or diameter horizontal when it sits outside") {
    auto rot = [&](DimKind k, Vec2 a, Vec2 b, bool oh) {
        Document doc;
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) {
            s.textAlign = DimTextAlign::WithDimLine;
            s.textAlignOutside = oh ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
        }));
        tx.commit();
        return layoutDimension(doc, radial(k, a, b)).text.rotation;
    };
    CHECK(rot(DimKind::Radius, {0, 0}, {7, 7}, true) == doctest::Approx(0));
    CHECK(std::abs(rot(DimKind::Radius, {0, 0}, {7, 7}, false)) > 0.1);
    // A diameter whose text fits inside keeps following the line.
    CHECK(std::abs(rot(DimKind::Diameter, {-20, -20}, {20, 20}, true)) > 0.1);
    // One that does not fit goes outside and turns horizontal.
    CHECK(rot(DimKind::Diameter, {-2, -2}, {2, 2}, true) == doctest::Approx(0));
}

TEST_CASE("DIMTOH turns angular text horizontal when the arc lies beyond the extension lines") {
    auto rot = [&](bool oh, Vec2 textPos) {
        Document doc;
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) {
            s.textAlign = DimTextAlign::WithDimLine;
            s.textAlignOutside = oh ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
        }));
        tx.commit();
        DimensionData d = angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15});
        d.textPosUser = true;
        d.textPos = textPos;
        return layoutDimension(doc, d).text.rotation;
    };
    CHECK(rot(true, {40, 40}) == doctest::Approx(0));
    CHECK(std::abs(rot(false, {40, 40})) > 0.1);
    CHECK(std::abs(rot(true, {5, 5})) > 0.1);  // between the lines: follows the arc
}

TEST_CASE("DIMCEN draws a centre mark on radius and diameter dimensions") {
    auto lines = [&](double mark, DimKind k, Vec2 a, Vec2 b) {
        Document doc;
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) { s.centerMark = mark; }));
        tx.commit();
        return layoutDimension(doc, radial(k, a, b));
    };
    const Layout none = lines(0, DimKind::Radius, {0, 0}, {10, 0});
    const Layout mark = lines(2.5, DimKind::Radius, {0, 0}, {10, 0});
    CHECK(mark.lines.size() == none.lines.size() + 2);
    const Layout centerLines = lines(-2.5, DimKind::Radius, {0, 0}, {10, 0});
    CHECK(centerLines.lines.size() == none.lines.size() + 2 + 4);
    // A diameter marks the middle of the circle, not its end.
    const Layout dia = lines(2.5, DimKind::Diameter, {-10, 0}, {10, 0});
    bool atMiddle = false;
    for (const auto& l : dia.lines)
        if (l.pts.size() == 2 && std::abs(l.pts[0].x + 2.5) < 1e-9 &&
            std::abs(l.pts[1].x - 2.5) < 1e-9)
            atMiddle = true;
    CHECK(atMiddle);
}

TEST_CASE("a text frame (DIMGAP below zero) draws a closed box round the dimension text") {
    auto build = [&](bool frame) {
        Document doc;
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) { s.textFrame = frame; }));
        tx.commit();
        return layoutDimension(doc, linear({0, 0}, {40, 0}, {0, 10}));
    };
    const Layout plain = build(false), framed = build(true);
    REQUIRE(framed.lines.size() == plain.lines.size() + 1);
    const auto& box = framed.lines.back();
    REQUIRE(box.pts.size() == 5);
    CHECK(box.pts.front().x == doctest::Approx(box.pts.back().x));
    CHECK(box.pts.front().y == doctest::Approx(box.pts.back().y));
    // The box is wider than the text and centred on it.
    const double w = std::abs(box.pts[1].x - box.pts[0].x);
    CHECK(w > framed.text.width);
    CHECK((box.pts[0].x + box.pts[2].x) / 2 == doctest::Approx(framed.text.center.x));
}

TEST_CASE("DIMTMOVE 1: text moved away hangs on a leader from the middle with a landing") {
    // As AutoCAD draws it: the text point is where the text starts; the leader runs from the
    // middle of the dimension line to a landing line under the text.
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    d.textPosUser = true;
    d.textPos = {150, -40};
    Layout L = withStyle([](DimStyle& s) { s.textMove = DimTextMove::Leader; }, d);
    const double raise = 0.625 + 1.25;  // gap + half the text height
    CHECK(L.text.center.x == doctest::Approx(150 + L.text.width / 2));
    CHECK(L.text.center.y == doctest::Approx(-40));
    bool leader = false, landing = false;
    for (const auto& l : L.lines) {
        if (l.role != Role::DimLine)
            continue;
        leader =
            leader || (near(l.pts.front(), {50, 10}) && near(l.pts.back(), {149.375, -40 - raise}));
        landing = landing || (near(l.pts.front(), {149.375, -40 - raise}) &&
                              l.pts.back().x == doctest::Approx(150 + L.text.width + 0.625));
    }
    CHECK(leader);
    CHECK(landing);
    // Text kept near the dimension line is not moved away: no leader.
    d.textPos = {50, 12};
    Layout onLine = withStyle([](DimStyle& s) { s.textMove = DimTextMove::Leader; }, d);
    CHECK(onLine.text.center.x == doctest::Approx(50));
}

TEST_CASE("text above the line: its point is on the dimension line, moved text stays above it") {
    // AutoCAD writes the point on the dimension line under text lifted above it (DIMTAD 1).
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    Layout L = withStyle([](DimStyle&) {}, d);
    CHECK(near(L.text.center, {50, 11.875}));
    CHECK(near(L.text.point, {50, 10}));
    // Moved by hand (DIMTMOVE 0): the point stays on the line, the text sits above it.
    d.textPosUser = true;
    d.textPos = {30, 10};
    Layout moved = withStyle([](DimStyle&) {}, d);
    CHECK(near(moved.text.center, {30, 11.875}));
    CHECK(near(moved.text.point, {30, 10}));
    // A point away from the line is the middle of the text.
    d.textPos = {30, 20};
    CHECK(near(withStyle([](DimStyle&) {}, d).text.center, {30, 20}));
    d.textPos = {30, 10};
    // Centred text is on its point.
    Layout centred =
        withStyle([](DimStyle& s) { s.textPlacement = DimTextPlacement::Centered; }, d);
    CHECK(near(centred.text.center, {30, 10}));
}

TEST_CASE("an extension line offset past the dimension line leaves the part beyond it") {
    // The origin is 5 from the dimension line, the offset 8: AutoCAD still draws from 8 beyond
    // the origin to the extension past the dimension line.
    DimensionData d = linear({0, 0}, {100, 0}, {50, -5});
    Layout L = withStyle(
        [](DimStyle& s) {
            s.extLineOffset = 8;
            s.extLineExtension = 5;
        },
        d);
    int ext = 0;
    for (const auto& l : L.lines) {
        if (l.role != Role::ExtLine)
            continue;
        ++ext;
        CHECK(l.pts.front().y == doctest::Approx(-8));
        CHECK(l.pts.back().y == doctest::Approx(-10));
    }
    CHECK(ext == 2);
}

TEST_CASE("text moved by hand keeps the arrows where the fit rules put them") {
    // DIMTIX: the text is forced between the extension lines, so the arrows go outside, even
    // when the text was then moved above the dimension.
    DimensionData d = linear({0, 0}, {6, 0}, {3, 10});
    d.textOverride = "TOO LONG TO FIT";
    d.textPosUser = true;
    d.textPos = {3, 20};
    Layout tix = withStyle([](DimStyle& s) { s.textInsideForced = true; }, d);
    Layout plain = withStyle([](DimStyle&) {}, d);
    REQUIRE(tix.arrows.size() == 2);
    REQUIRE(plain.arrows.size() == 2);
    // Arrow at x = 0: inside its tail is at +2.5, outside at -2.5.
    auto tailOfArrowAt0 = [](const Layout& L) {
        for (const auto& a : L.arrows)
            if (std::abs(a.ring[0].x) < 1e-9)
                return a.ring[1].x;
        return 0.0;
    };
    CHECK(tailOfArrowAt0(tix) < 0);
    CHECK(tailOfArrowAt0(plain) > 0);
    // Short text that would fit alone (best fit sends the arrows out in its own place) but was
    // moved away: the arrows fit, so they stay inside, as AutoCAD draws it.
    d.textOverride = "1";
    d.textPos = {20, 10};
    CHECK(tailOfArrowAt0(plain) > 0);
    Layout shortText = withStyle([](DimStyle&) {}, d);
    CHECK(tailOfArrowAt0(shortText) > 0);
}
