#include <cmath>

#include <doctest/doctest.h>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

const std::string kThai = "สวัสดีครับผม"
                          "ชื่อทดสอบ";

// Advance of `s` computed straight from the font library, independent of the layout code.
double fontWidth(const std::string& s, double height) {
    font::FontId f = fontForStyle(nullptr);
    double capH = fonts().metrics(f).capHeight;
    double w = 0;
    for (const auto& run : fonts().shape(f, s))
        for (const auto& g : run.glyphs)
            w += double(g.xAdvance) / run.unitsPerEm;
    return w * height / capH;
}

TextData baseText(const std::string& s) {
    TextData t;
    t.utf8 = s;
    t.height = 10;
    return t;
}

}  // namespace

TEST_CASE("TBCAD at height 10 has the width the font metrics give") {
    Document doc;
    TextLayout l = layoutText(doc, baseText("TBCAD"));
    REQUIRE(l.lines.size() == 1);
    CHECK(l.lines[0].width == doctest::Approx(fontWidth("TBCAD", 10)));
    CHECK(l.lines[0].width > 20);
    CHECK(l.frame.max.y == doctest::Approx(10));  // capital height
    CHECK(measureWidth(doc.textStyle(kTextStyleStandard), "TBCAD", 10) ==
          doctest::Approx(l.lines[0].width));
}

TEST_CASE("text height is capital height: the capitals of 'H' are as tall as the height") {
    Document doc;
    TextLayout l = layoutText(doc, baseText("H"));
    REQUIRE(l.glyphs.size() == 1);
    // The glyph box top (H has no overshoot) is the capital height.
    BBox2 b = l.box;
    CHECK(b.max.y == doctest::Approx(10).epsilon(0.03));
    CHECK(l.glyphs[0].origin.y == doctest::Approx(0));
}

TEST_CASE("TEXT alignment points for each mode") {
    Document doc;
    const double W = fontWidth("TBCAD", 10);
    auto layout = [&](HAlign h, VAlign v) {
        TextData t = baseText("TBCAD");
        t.position = t.alignPoint = {100, 50};
        t.h = h;
        t.v = v;
        return layoutText(doc, t);
    };
    auto baselineStart = [](const TextLayout& l) { return l.toWorld.apply({0, 0}); };
    // Left / Baseline: the text starts at the insertion point, on the baseline.
    {
        TextLayout l = layout(HAlign::Left, VAlign::Baseline);
        Vec2 p = baselineStart(l);
        CHECK(p.x == doctest::Approx(100));
        CHECK(p.y == doctest::Approx(50));
    }
    // Center / Baseline
    {
        TextLayout l = layout(HAlign::Center, VAlign::Baseline);
        CHECK(l.toWorld.apply({l.glyphs.front().origin.x, 0}).x == doctest::Approx(100 - W / 2));
    }
    // Right / Baseline
    {
        TextLayout l = layout(HAlign::Right, VAlign::Baseline);
        CHECK(l.toWorld.apply({l.glyphs.front().origin.x, 0}).x == doctest::Approx(100 - W));
    }
    // Left / Top: capitals hang below the point; Middle: centred on it; Bottom: descender line.
    {
        TextLayout l = layout(HAlign::Left, VAlign::Top);
        CHECK(l.toWorld.apply({0, l.lines[0].baseline}).y == doctest::Approx(40));
    }
    {
        TextLayout l = layout(HAlign::Left, VAlign::Middle);
        CHECK(l.toWorld.apply({0, l.lines[0].baseline}).y == doctest::Approx(45));
    }
    {
        TextLayout l = layout(HAlign::Left, VAlign::Bottom);
        double base = l.toWorld.apply({0, l.lines[0].baseline}).y;
        CHECK(base > 50);                    // baseline above the point
        CHECK(l.toWorld.apply({0, l.frame.min.y}).y == doctest::Approx(50));  // frame bottom on it
    }
    // Middle (horizontal + vertical centre)
    {
        TextLayout l = layout(HAlign::Middle, VAlign::Baseline);
        Vec2 c = l.toWorld.apply({l.glyphs.front().origin.x, l.lines[0].baseline});
        CHECK(c.x == doctest::Approx(100 - W / 2));
        CHECK(c.y == doctest::Approx(45));
    }
}

TEST_CASE("TEXT Aligned and Fit use the align point") {
    Document doc;
    TextData t = baseText("TBCAD");
    t.position = {0, 0};
    t.alignPoint = {100, 0};
    t.h = HAlign::Fit;
    TextLayout fit = layoutText(doc, t);
    // Fit: the text spans exactly 100 mm, height unchanged.
    Vec2 end = fit.toWorld.apply({fit.lines[0].width, 0});
    CHECK(end.x == doctest::Approx(100));
    CHECK(fit.toWorld.apply({0, 10}).y == doctest::Approx(10));

    t.h = HAlign::Aligned;
    TextLayout al = layoutText(doc, t);
    end = al.toWorld.apply({al.lines[0].width, 0});
    CHECK(end.x == doctest::Approx(100));
    CHECK(al.toWorld.apply({0, 10}).y > 10);  // height grew with the width

    // Direction comes from the points, not the entity's rotation.
    t.alignPoint = {0, 100};
    t.rotation = 1.234;
    TextLayout up = layoutText(doc, t);
    Vec2 e2 = up.toWorld.apply({up.lines[0].width, 0});
    CHECK(e2.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(e2.y == doctest::Approx(100));
}

TEST_CASE("TEXT rotation, width factor and oblique") {
    Document doc;
    TextData t = baseText("TBCAD");
    t.rotation = kPi / 2;
    TextLayout l = layoutText(doc, t);
    Vec2 e = l.toWorld.apply({l.lines[0].width, 0});
    CHECK(e.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(e.y == doctest::Approx(l.lines[0].width));

    TextData w = baseText("TBCAD");
    w.widthFactor = 0.5;
    TextLayout lw = layoutText(doc, w);
    CHECK(lw.toWorld.apply({lw.lines[0].width, 0}).x == doctest::Approx(fontWidth("TBCAD", 10) / 2));

    TextData o = baseText("TBCAD");
    o.oblique = 0.3;
    TextLayout lo = layoutText(doc, o);
    CHECK(lo.toWorld.apply({0, 10}).x == doctest::Approx(10 * std::tan(0.3)));
}

TEST_CASE("style width factor and oblique combine with the entity's") {
    Document doc;
    auto tx = doc.begin("style");
    auto id = tx.addTextStyle(TextStyle{{}, "Narrow", "", "", 0.5, 0.2});
    REQUIRE(id.ok());
    tx.commit();
    TextData t = baseText("TBCAD");
    t.style = id.value();
    t.widthFactor = 2;
    TextLayout l = layoutText(doc, t);
    CHECK(l.toWorld.m00 == doctest::Approx(1.0));  // 0.5 * 2
    CHECK(l.toWorld.m01 == doctest::Approx(std::tan(0.2)));
}

TEST_CASE("mirrored text stays readable: the transform helper keeps glyph orientation") {
    Document doc;
    TextData t = baseText("TBCAD");
    t.position = {10, 10};
    Entity e{{}, t};
    auto m = Transform2::mirror({0, 0}, {0, 1});  // mirror across the Y axis
    REQUIRE(m.ok());
    auto r = geom::transformed(e, m.value());
    REQUIRE(r.ok());
    const auto* d = entityAs<TextData>(r.value());
    REQUIRE(d);
    CHECK(d->position.x == doctest::Approx(-10));
    TextLayout l = layoutText(doc, *d);
    // The glyph x axis still points to +x and y up: determinant positive.
    CHECK(l.toWorld.determinant() > 0);
}

TEST_CASE("MTEXT: paragraphs, line pitch and attachments") {
    Document doc;
    MTextData m;
    m.height = 3;
    m.utf8 = "AAA\\PBBB\\PCCC";
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() == 3);
    CHECK(l.lines[0].baseline == doctest::Approx(-3));
    CHECK(l.lines[1].baseline == doctest::Approx(-3 - kLineSpacing * 3));
    CHECK(l.lines[2].baseline == doctest::Approx(-3 - 2 * kLineSpacing * 3));
    CHECK(l.frame.max.y == doctest::Approx(0));

    m.attach = Attachment::BottomRight;
    TextLayout br = layoutMText(doc, m);
    CHECK(br.frame.min.y == doctest::Approx(0));
    CHECK(br.frame.max.x == doctest::Approx(0));

    m.attach = Attachment::MiddleCenter;
    TextLayout mc = layoutMText(doc, m);
    CHECK(mc.frame.center().x == doctest::Approx(0));
    CHECK(mc.frame.center().y == doctest::Approx(0));
}

TEST_CASE("MTEXT wraps at spaces") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.utf8 = "alpha beta gamma delta";
    m.width = fontWidth("alpha beta", 5) + 1;
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() == 3);
    CHECK(l.lines[0].text == "alpha beta");
    CHECK(l.lines[1].text == "gamma");
    CHECK(l.lines[2].text == "delta");
    for (const auto& ln : l.lines)
        CHECK(ln.width <= m.width + 1e-6);
}

TEST_CASE("MTEXT: a Thai sentence without spaces wraps only at cluster boundaries") {
    Document doc;
    auto bounds = clusterBoundaries(kThai);
    for (double frac : {0.2, 0.33, 0.5, 0.75}) {
        MTextData m;
        m.height = 5;
        m.utf8 = kThai;
        m.width = fontWidth(kThai, 5) * frac;
        TextLayout l = layoutMText(doc, m);
        REQUIRE(l.lines.size() >= 2);
        std::size_t off = 0;
        std::string joined;
        for (const auto& ln : l.lines) {
            // Every line starts at a cluster boundary of the original string.
            CHECK(std::find(bounds.begin(), bounds.end(), off) != bounds.end());
            off += ln.text.size();
            joined += ln.text;
            CHECK(ln.width <= m.width + 1e-6);
        }
        CHECK(off == kThai.size());
        CHECK(joined == kThai);
    }
}

TEST_CASE("MTEXT: a cluster wider than the width is never split") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.utf8 = "ห้อง";  // ห้อง
    m.width = 0.1;
    TextLayout l = layoutMText(doc, m);
    // Three clusters, one per line; the ห + MAI THO cluster is kept whole.
    REQUIRE(l.lines.size() == 3);
    CHECK(l.lines[0].text == "ห้");
}

TEST_CASE("MTEXT: zero-width space is a break opportunity") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.utf8 = kThai.substr(0, 18) + "​" + kThai.substr(18);
    m.width = fontWidth(kThai.substr(0, 18), 5) + 0.5;
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() >= 2);
    // The first line ends exactly at the ZWSP position, not mid-word.
    CHECK(l.lines[0].text == kThai.substr(0, 18));
}

TEST_CASE("MTEXT markup: height override changes line height, colour is carried") {
    Document doc;
    MTextData m;
    m.height = 2;
    m.utf8 = "{\\H4;BIG}\\P{\\C1;red}";
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].height == doctest::Approx(4));
    CHECK(l.lines[0].baseline == doctest::Approx(-4));
    bool sawRed = false;
    for (const auto& g : l.glyphs)
        sawRed = sawRed || g.color == 1;
    CHECK(sawRed);
}

TEST_CASE("text layout of other entities is empty, empty text has no glyphs") {
    Document doc;
    CHECK(layoutEntity(doc, Entity{{}, LineData{}}).empty());
    TextData t;
    CHECK(layoutText(doc, t).empty());
    CHECK(layoutMText(doc, MTextData{}).empty());
}
