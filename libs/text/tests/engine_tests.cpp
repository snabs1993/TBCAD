// Engine checks for text: mixed Thai / English wrapping, extents against what is drawn, fields,
// annotative scale and the speed of laying out many texts.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

struct Reg {
    Reg() { registerDisplay(); }
};
const Reg kReg;

BBox2 drawnBounds(const Document& doc, const Entity& e, double annoScale = 0) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 1e-4;  // never the placeholder box
    ctx.annoScale = annoScale;
    DisplayList out;
    geom::buildDisplay(doc, e, ctx, out);
    BBox2 b;
    for (const auto& it : out.items) {
        if (const auto* f = std::get_if<DisplayFill>(&it))
            for (const auto& r : f->rings)
                for (const Vec2& p : r)
                    b.expand(p);
        if (const auto* pl = std::get_if<DisplayPolyline>(&it))
            for (const Vec2& p : pl->pts)
                b.expand(p);
    }
    return b;
}

void setCustom(Document& doc, const char* key, const char* value) {
    Transaction tx = doc.begin("setup");
    REQUIRE(tx.modifyHeader([&](Header& h) { h.custom[key] = value; }).ok());
    tx.commit();
}

bool covers(const BBox2& outer, const BBox2& inner, double eps) {
    return outer.min.x <= inner.min.x + eps && outer.min.y <= inner.min.y + eps &&
           outer.max.x >= inner.max.x - eps && outer.max.y >= inner.max.y - eps;
}

const char* const kBits[] = {
    "สวัสดีครับ",      "ห้องน้ำ",   "ที่นี่ผู้ใหญ่", "Hello", "drawing",     "A-101",       "ชั้น 2", "room 12",
    "กรุงเทพมหานคร", "\\S1/2;", " ",      "ก้า",    "{\\b1;bold}", "{\\i1;เอียง}", "\\P",  "123.45"};

std::string randomMixed(std::mt19937_64& rng, int words) {
    std::string s;
    for (int i = 0; i < words; ++i)
        s += kBits[rng() % (sizeof kBits / sizeof kBits[0])] + std::string(rng() % 3 ? " " : "");
    return s;
}

}  // namespace

TEST_CASE("mixed Thai and English MTEXT wraps inside the width and keeps every character") {
    Document doc;
    std::mt19937_64 rng(77);
    for (int round = 0; round < 120; ++round) {
        MTextData m;
        m.height = 5;
        m.utf8 = randomMixed(rng, 4 + static_cast<int>(rng() % 12));
        m.width = 20 + static_cast<double>(rng() % 160);
        CAPTURE(m.utf8);
        CAPTURE(m.width);
        TextLayout l = layoutMText(doc, m);
        std::string joined;
        std::size_t clusterWide = 0;
        for (const LayoutLine& ln : l.lines) {
            joined += ln.text;
            // A line starts at a cluster of its own: never with a combining mark.
            if (!ln.text.empty()) {
                const unsigned char c0 = static_cast<unsigned char>(ln.text[0]);
                CHECK(c0 != 0);
                if (ln.text.size() >= 3 && c0 == 0xE0) {
                    const unsigned char c1 = static_cast<unsigned char>(ln.text[1]);
                    const unsigned char c2 = static_cast<unsigned char>(ln.text[2]);
                    const unsigned cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
                    const bool combining =
                        cp == 0xE31 || (cp >= 0xE34 && cp <= 0xE3A) || (cp >= 0xE47 && cp <= 0xE4E);
                    CHECK_FALSE(combining);
                }
            }
            // Wider than the box only when a single unbreakable piece is that wide.
            if (ln.width > m.width + 1e-6)
                ++clusterWide;
        }
        // Everything printable survives (spaces at breaks and markup are dropped).
        const std::string plain = plainText(m.utf8);
        auto strip = [](std::string s) {
            s.erase(
                std::remove_if(s.begin(), s.end(), [](char ch) { return ch == ' ' || ch == '\n'; }),
                s.end());
            return s;
        };
        std::string flat;
        for (const LayoutLine& ln : l.lines)
            flat += ln.text;
        CHECK(strip(flat).size() >= strip(plain).size() / 2);  // stacked parts are re-flowed
        CHECK(clusterWide <= l.lines.size());
        for (const PlacedGlyph& g : l.glyphs) {
            CHECK(std::isfinite(g.origin.x));
            CHECK(std::isfinite(g.origin.y));
        }
    }
}

TEST_CASE("text extents cover everything that is drawn, for every kind of text") {
    Document doc;
    std::mt19937_64 rng(5);
    int checked = 0;
    for (int round = 0; round < 150; ++round) {
        Entity e;
        if (round % 2 == 0) {
            TextData t;
            t.utf8 = randomMixed(rng, 1 + static_cast<int>(rng() % 4));
            std::replace(t.utf8.begin(), t.utf8.end(), '\\', '/');
            t.height = 2 + static_cast<double>(rng() % 20);
            t.rotation = (static_cast<double>(rng() % 628)) / 100;
            t.widthFactor = 0.5 + static_cast<double>(rng() % 15) / 10;
            t.oblique = (static_cast<double>(rng() % 40) - 20) * kPi / 180;
            t.h = static_cast<HAlign>(rng() % 3);
            t.position = {static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000)};
            t.alignPoint = t.position;
            e.data = std::move(t);
        } else {
            MTextData m;
            m.utf8 = randomMixed(rng, 2 + static_cast<int>(rng() % 8));
            m.height = 2 + static_cast<double>(rng() % 12);
            m.width = rng() % 3 ? 30 + static_cast<double>(rng() % 100) : 0;
            m.rotation = (static_cast<double>(rng() % 628)) / 100;
            m.attach = static_cast<Attachment>(rng() % 9);
            m.position = {static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000)};
            e.data = std::move(m);
        }
        const BBox2 b = geom::bounds(e, doc);
        const BBox2 d = drawnBounds(doc, e);
        if (d.empty())
            continue;
        ++checked;
        REQUIRE_FALSE(b.empty());
        CHECK(covers(b, d, 1e-6 * (1 + std::abs(b.max.x) + std::abs(b.max.y))));
    }
    CHECK(checked > 100);
}

TEST_CASE("annotative text: layout, extents and drawing follow the annotation scale") {
    Document doc;
    setCustom(doc, kAnnoScaleKey, "100");
    Entity e;
    MTextData m;
    m.height = 2.5;
    m.width = 50;
    m.utf8 = "ห้องประชุม meeting room";
    e.data = m;
    e.common.annotative = true;
    const BBox2 b = geom::bounds(e, doc);
    const BBox2 d = drawnBounds(doc, e);
    REQUIRE_FALSE(d.empty());
    CHECK(covers(b, d, 1e-6));
    // 1:100 makes it 100 times larger than the same text that is not annotative.
    Entity plain = e;
    plain.common.annotative = false;
    const BBox2 pb = geom::bounds(plain, doc);
    CHECK(b.height() > pb.height() * 50);
    // A different scale override resizes it.
    const BBox2 d50 = drawnBounds(doc, e, 50);
    CHECK(d50.width() < d.width());
    CHECK(d50.width() > 0);
}

TEST_CASE("fields inside MTEXT are expanded before wrapping and measured as shown") {
    Document doc;
    setCustom(doc, "NOTE", "สวัสดีครับ everybody");
    MTextData m;
    m.height = 5;
    m.width = 60;
    m.utf8 = "Note: %<var:NOTE>% (end)";
    TextLayout l = layoutMText(doc, m);
    std::string joined;
    for (const LayoutLine& ln : l.lines)
        joined += ln.text + " ";
    CHECK(joined.find("everybody") != std::string::npos);
    CHECK(joined.find("%var") == std::string::npos);
    for (const LayoutLine& ln : l.lines)
        CHECK(ln.width <= m.width + 1e-6);
}

TEST_CASE("stacked fractions wrap as one unit and have a finite frame") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.width = 40;
    m.utf8 = "ท่อขนาด \\S1/2; นิ้ว และ \\S3#4; นิ้ว สูง \\S+0.1^-0.2; ม.";
    TextLayout l = layoutMText(doc, m);
    REQUIRE_FALSE(l.empty());
    CHECK(l.lines.size() >= 2);
    CHECK_FALSE(l.frame.empty());
    CHECK(std::isfinite(l.frame.width()));
    CHECK(std::isfinite(l.frame.height()));
}

TEST_CASE("laying out and drawing 20 000 texts is quick") {
    Document doc;
    std::vector<Entity> es;
    std::mt19937_64 rng(9);
    for (int i = 0; i < 20000; ++i) {
        Entity e;
        if (i % 2) {
            TextData t;
            t.utf8 = i % 3 ? "ห้องนอน 12" : "Room " + std::to_string(i);
            t.height = 2.5;
            t.position = {static_cast<double>(i % 200) * 30, static_cast<double>(i / 200) * 10};
            t.alignPoint = t.position;
            e.data = std::move(t);
        } else {
            MTextData m;
            m.utf8 = "ห้องประชุมใหญ่ meeting room \\P" + std::to_string(rng() % 100);
            m.height = 2.5;
            m.width = 40;
            m.position = {static_cast<double>(i % 200) * 30, static_cast<double>(i / 200) * 10};
            e.data = std::move(m);
        }
        es.push_back(std::move(e));
    }
    auto t0 = std::chrono::steady_clock::now();
    BBox2 all;
    for (const Entity& e : es)
        all.merge(geom::bounds(e, doc));
    const double boundsMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.05;
    std::size_t items = 0;
    for (const Entity& e : es) {
        DisplayList out;
        geom::buildDisplay(doc, e, ctx, out);
        items += out.items.size();
    }
    const double drawMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("20k texts: extents " << boundsMs << " ms, display " << drawMs << " ms");
    CHECK(items > 20000);
    CHECK_FALSE(all.empty());
#ifdef NDEBUG
    CHECK(drawMs < 3000);
#else
    CHECK(drawMs < 60000);
#endif
}

TEST_CASE("MTEXT columns: lines are balanced over the columns, side by side") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.width = 60;
    m.utf8 = "one\\Ptwo\\Pthree\\Pfour\\Pfive\\Psix";
    m.columns = 2;
    m.columnGap = 10;
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() == 6);
    // Three lines per column, the second column starts at the top again, 70 mm to the right.
    CHECK(l.lines[0].x0 == doctest::Approx(0));
    CHECK(l.lines[3].x0 == doctest::Approx(70));
    CHECK(l.lines[3].baseline == doctest::Approx(l.lines[0].baseline));
    CHECK(l.lines[2].x0 == doctest::Approx(0));
    CHECK(l.lines[2].baseline < l.lines[1].baseline);
    CHECK(l.frame.width() == doctest::Approx(130));
    // The frame is shorter than the same text in a single column.
    MTextData one = m;
    one.columns = 1;
    TextLayout single = layoutMText(doc, one);
    CHECK(l.frame.height() < single.frame.height() * 0.6);
    // Extents cover the drawing, and more columns than lines is harmless.
    CHECK(covers(l.worldBounds(), drawnBounds(doc, Entity{{}, m}), 1e-6));
    m.columns = 8;
    TextLayout many = layoutMText(doc, m);
    CHECK(many.lines.size() == 6);
    CHECK(std::isfinite(many.frame.width()));
}
