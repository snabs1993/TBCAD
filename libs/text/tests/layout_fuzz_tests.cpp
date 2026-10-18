// Random MTEXT (Thai, markup codes, broken markup, wrapping widths) through layout: no crash, only
// finite numbers, wrapped lines stay within the width, and every attachment gives a finite frame.
#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <string>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

std::string randomPiece(std::mt19937_64& rng) {
    static const char* const kBits[] = {
        "Hello ", "world",   "123",          " ",        "\\P", "\\\\",   "\\{",     "{",
        "}",      "\\L",     "\\l",          "\\O",      "\\o", "{\\b1;", "{\\i1;",  "{\\H0;",
        "{\\H3;", "{\\H2x;", "{\\C7;",       "{\\C999;", "\\",  "\\Z",    ";",       "\n",
        "\r\n",   "สวัสดีครับ", "การทดสอบระบบ", "ก๊า",       "°C",  "​",    "\xff\xfe"};
    return kBits[rng() % (sizeof kBits / sizeof kBits[0])];
}

bool finite(const TextLayout& l) {
    for (const PlacedGlyph& g : l.glyphs)
        if (!std::isfinite(g.origin.x) || !std::isfinite(g.origin.y) || !std::isfinite(g.scale))
            return false;
    for (const LayoutLine& ln : l.lines)
        if (!std::isfinite(ln.baseline) || !std::isfinite(ln.width) || !std::isfinite(ln.x0))
            return false;
    return true;
}

}  // namespace

TEST_CASE("random MTEXT markup lays out to finite numbers") {
    Document doc;
    std::mt19937_64 rng(20261020);
    for (int round = 0; round < 400; ++round) {
        MTextData m;
        m.height = 0.5 + static_cast<double>(rng() % 400) / 10;
        m.width = (rng() % 3 == 0) ? 0 : 5 + static_cast<double>(rng() % 300);
        m.rotation = static_cast<double>(rng() % 700) / 100;
        m.attach = static_cast<Attachment>(rng() % 9);
        for (int i = 0, n = static_cast<int>(rng() % 12); i < n; ++i)
            m.utf8 += randomPiece(rng);
        CAPTURE(m.utf8);
        TextLayout l = layoutMText(doc, m);
        CHECK(finite(l));
        if (!l.empty()) {
            const BBox2 b = l.worldBounds();
            CHECK_FALSE(b.empty());
            CHECK(std::isfinite(b.min.x));
            CHECK(std::isfinite(b.max.y));
        }
        // A wrapped line is never wider than the box, unless one cluster is wider on its own.
        if (m.width > 0)
            for (const LayoutLine& ln : l.lines)
                if (ln.text.size() > 8)  // more than a single word or cluster
                    CHECK(ln.width <= m.width * 1.5 + m.height * 3);
        (void)plainText(m.utf8);
    }
}

#include "tbcad/text/field.h"

TEST_CASE("random field text expands without crashing and never loses plain text") {
    Document doc;
    std::mt19937_64 rng(20261021);
    static const char* const kBits[] = {"%<",
                                        ">%",
                                        "%",
                                        "<",
                                        ">",
                                        ":",
                                        "date",
                                        "date:dd/MM/yyyy",
                                        "date:BBBB",
                                        "filename",
                                        "filepath",
                                        "sheet",
                                        "var:X",
                                        "length:#1",
                                        "length:#999999999999999999999",
                                        "area:#0",
                                        "text:#1:",
                                        "length:#",
                                        "#",
                                        "::",
                                        "abc ",
                                        "สวัสดี",
                                        "\\P",
                                        "{",
                                        "}"};
    for (int round = 0; round < 2000; ++round) {
        std::string s;
        for (int i = 0, n = static_cast<int>(rng() % 10); i < n; ++i)
            s += kBits[rng() % (sizeof kBits / sizeof kBits[0])];
        CAPTURE(s);
        const std::string out = expandFields(doc, s, "2026-10-18");
        if (!hasFields(s))
            CHECK(out == s);
    }
}
