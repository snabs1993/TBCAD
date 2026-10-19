// Hatches and Thai text come out of the same display lists as on screen (W5-4, W4-4).
#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/export/plot.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::exportlib;

namespace {

struct Reg {
    Reg() {
        text::registerDisplay();
        hatch::registerHatchDisplay();
    }
};
const Reg kReg;

void addEntity(Document& doc, Entity e) {
    auto tx = doc.begin("add");
    REQUIRE(tx.add(std::move(e)).ok());
    tx.commit();
}

Entity squareHatch(double side, const std::string& pattern, double scale) {
    HatchLoop loop;
    Vec2 c[4] = {{0, 0}, {side, 0}, {side, side}, {0, side}};
    for (int i = 0; i < 4; ++i)
        loop.edges.emplace_back(HatchLineEdge{c[i], c[(i + 1) % 4]});
    HatchData h;
    h.loops.push_back(std::move(loop));
    h.solid = pattern.empty();
    h.pattern = pattern;
    h.patternScale = scale;
    Entity e;
    e.data = Indirect<HatchData>(std::move(h));
    return e;
}

Entity thaiText(const std::string& utf8, double height) {
    TextData t;
    t.utf8 = utf8;
    t.height = height;
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    return e;
}

BBox2 primBounds(const PlotSheet& sheet, bool fillsOnly) {
    BBox2 b;
    for (const auto& prim : sheet.prims) {
        if (const auto* f = std::get_if<PlotFill>(&prim)) {
            for (const auto& r : f->rings)
                for (const Vec2& p : r)
                    b.expand(p);
        } else if (!fillsOnly) {
            if (const auto* p = std::get_if<PlotPath>(&prim))
                for (const Vec2& v : p->pts)
                    b.expand(v);
        }
    }
    return b;
}

}  // namespace

TEST_CASE("a pattern hatch plots as hairlines inside its boundary, spacing scaled by the plot") {
    Document doc;
    addEntity(doc, squareHatch(1000, "LINE45", 20));  // 60 mm line spacing in the drawing
    PlotSettings s;
    s.fitToPaper = false;
    s.scaleN = 10;  // the 1000 mm square becomes 100 mm
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    std::size_t lines = 0;
    for (const auto& prim : sheet.value().prims) {
        const auto* p = std::get_if<PlotPath>(&prim);
        REQUIRE(p);  // no fills, no tone
        ++lines;
        CHECK(p->style.widthMm == doctest::Approx(s.hairlineMm));
    }
    // 45 degree lines 60/sqrt2 apart across a 1000 mm diagonal span: roughly 1414/60 + ...
    CHECK(lines > 15);
    CHECK(lines < 40);
    const BBox2 b = primBounds(sheet.value(), false);
    CHECK(b.width() == doctest::Approx(100).epsilon(0.01));
    CHECK(b.height() == doctest::Approx(100).epsilon(0.01));

    auto pdf = pdfBytes(sheet.value());
    REQUIRE(pdf.ok());
    CHECK(pdf.value().find(" m\n") != std::string::npos);
}

TEST_CASE("a solid hatch plots as an even-odd fill") {
    Document doc;
    addEntity(doc, squareHatch(100, "", 1));
    PlotSettings s;
    auto sheet = buildSheet(doc, s).value();
    REQUIRE(sheet.prims.size() == 1);
    CHECK(std::holds_alternative<PlotFill>(sheet.prims[0]));
    auto pdf = pdfBytes(sheet).value();
    // f* fills, B* fills and draws the hairline edge (#822): both even-odd.
    CHECK((pdf.find("f*") != std::string::npos || pdf.find("B*") != std::string::npos));
    std::string svg = svgString(sheet);
    CHECK(svg.find("fill-rule=\"evenodd\"") != std::string::npos);
}

TEST_CASE("translucent tones are mixed with white, not printed opaque") {
    Rgba c = plotColor(Rgba{255, 0, 0, 51}, ColorMode::AsDisplayed);  // 20 % red
    CHECK(c.a == 255);
    CHECK(c.r == 255);
    CHECK(c.g == 204);
    CHECK(c.b == 204);
}

TEST_CASE("Thai text is plotted as filled glyph outlines at the right size, marks included") {
    Document doc;
    addEntity(doc, thaiText("ห้อง", 10));
    PlotSettings s;
    s.fitToPaper = false;
    s.scaleN = 1;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    REQUIRE_FALSE(sheet.value().prims.empty());
    for (const auto& prim : sheet.value().prims)
        CHECK(std::holds_alternative<PlotFill>(prim));
    BBox2 withMark = primBounds(sheet.value(), true);
    // 10 mm text at 1:1: glyphs are some millimetres tall, nothing is collapsed or huge.
    CHECK(withMark.height() > 6);
    CHECK(withMark.height() < 20);
    CHECK(withMark.width() > 6);

    // The tone mark sits above the consonant: the same word without it is lower.
    Document plain;
    addEntity(plain, thaiText("หอง", 10));
    auto bare = buildSheet(plain, s).value();
    CHECK(withMark.height() > primBounds(bare, true).height());

    // Same text at 1:2 is half the size on paper.
    s.scaleN = 2;
    BBox2 half = primBounds(buildSheet(doc, s).value(), true);
    CHECK(half.height() == doctest::Approx(withMark.height() / 2).epsilon(0.02));

    auto pdf = pdfBytes(buildSheet(doc, s).value()).value();
    // f* fills, B* fills and draws the hairline edge (#822): both even-odd.
    CHECK((pdf.find("f*") != std::string::npos || pdf.find("B*") != std::string::npos));
    std::string svg = svgString(buildSheet(doc, s).value());
    CHECK(svg.find("evenodd") != std::string::npos);
}
