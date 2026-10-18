// Random TEXT and MTEXT (Thai, markup codes, odd characters) through DXF export and import.
#include <random>
#include <string>
#include <vector>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/text/text.h"

namespace tbcad::dxf {
namespace {

std::string randomMarkup(std::mt19937_64& rng, int depth = 0) {
    static const char* const kWords[] = {"Hello", "123", "A-B", "สวัสดี", "ทดสอบ", "x y", "°C", " "};
    std::string s;
    const int parts = 1 + static_cast<int>(rng() % 5);
    for (int i = 0; i < parts; ++i) {
        switch (rng() % 10) {
            case 0:
                s += "\\P";
                break;
            case 1:
                s += "{\\b1;" + std::string(kWords[rng() % 8]) + "}";
                break;
            case 2:
                s += "{\\i1;" + std::string(kWords[rng() % 8]) + "}";
                break;
            case 3:
                s += "{\\H" + std::to_string(1 + rng() % 9) + ";" + kWords[rng() % 8] + "}";
                break;
            case 4:
                s += "{\\C" + std::to_string(1 + rng() % 250) + ";" + kWords[rng() % 8] + "}";
                break;
            case 5:
                s += "\\L" + std::string(kWords[rng() % 8]) + "\\l";
                break;
            case 6:
                s += "\\\\";
                break;
            case 7:
                if (depth < 2)
                    s += "{\\b1;" + randomMarkup(rng, depth + 1) + "}";
                break;
            case 8:
                s += "\\S" + std::to_string(1 + rng() % 9) + (rng() % 2 ? "/" : "#") +
                     std::to_string(1 + rng() % 9) + ";";
                break;
            default:
                s += kWords[rng() % 8];
        }
    }
    return s;
}

}  // namespace

TEST_CASE("random TEXT and MTEXT survive DXF export and import") {
    std::mt19937_64 rng(20261019);
    for (int round = 0; round < 25; ++round) {
        Document doc;
        {
            auto tx = doc.begin("fuzz");
            for (int i = 0, n = 3 + static_cast<int>(rng() % 12); i < n; ++i) {
                Entity e;
                const double x = static_cast<double>(rng() % 2000) - 1000;
                const double y = static_cast<double>(rng() % 2000) - 1000;
                if (rng() % 2 == 0) {
                    TextData t;
                    t.position = {x, y};
                    t.alignPoint = t.position;
                    t.height = 1 + static_cast<double>(rng() % 200) / 10;
                    t.rotation = static_cast<double>(rng() % 600) / 100;
                    t.utf8 = text::plainText(randomMarkup(rng));
                    if (t.utf8.find('\n') != std::string::npos)
                        t.utf8 = "one line";
                    e.data = Indirect<TextData>(t);
                } else {
                    MTextData m;
                    m.position = {x, y};
                    m.height = 1 + static_cast<double>(rng() % 200) / 10;
                    m.width = (rng() % 2) ? 0 : 20 + static_cast<double>(rng() % 100);
                    m.rotation = static_cast<double>(rng() % 600) / 100;
                    m.attach = static_cast<Attachment>(rng() % 9);
                    m.utf8 = randomMarkup(rng);
                    if (m.width > 0 && rng() % 3 == 0) {
                        m.columns = static_cast<std::uint8_t>(2 + rng() % 3);
                        m.columnGap = (rng() % 2) ? 0 : 3 + static_cast<double>(rng() % 20);
                    }
                    e.data = Indirect<MTextData>(m);
                }
                REQUIRE(tx.add(std::move(e)).ok());
            }
            tx.commit();
        }
        Report exportReport;
        Result<ExportResult> out = exportDxf(doc, ExportOptions{}, exportReport);
        REQUIRE(out.ok());
        Report importReport;
        Result<ImportResult> back =
            importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, importReport);
        REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
        CAPTURE(round);
        std::vector<Entity> before, after;
        doc.forEachEntity([&](const Entity& e) { before.push_back(e); });
        back.value().document->forEachEntity([&](const Entity& e) { after.push_back(e); });
        REQUIRE(before.size() == after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            CAPTURE(i);
            if (const auto* a = std::get_if<Indirect<TextData>>(&before[i].data)) {
                const auto* b = std::get_if<Indirect<TextData>>(&after[i].data);
                REQUIRE(b);
                CHECK((*a)->utf8 == (*b)->utf8);
                CHECK((*a)->height == doctest::Approx((*b)->height));
                CHECK((*a)->position.x == doctest::Approx((*b)->position.x));
            } else if (const auto* a2 = std::get_if<Indirect<MTextData>>(&before[i].data)) {
                const auto* b2 = std::get_if<Indirect<MTextData>>(&after[i].data);
                REQUIRE(b2);
                CHECK((*a2)->attach == (*b2)->attach);
                CHECK((*a2)->height == doctest::Approx((*b2)->height));
                CHECK((*a2)->width == doctest::Approx((*b2)->width));
                CHECK((*a2)->columns == (*b2)->columns);
                if ((*a2)->columns > 1)
                    CHECK((*b2)->columnGap == doctest::Approx((*a2)->columnGap > 0
                                                                  ? (*a2)->columnGap
                                                                  : 2 * (*a2)->height));
                // Same pieces; the importer writes a paragraph mark as a raw newline.
                const auto pa = text::parseMarkup((*a2)->utf8, (*a2)->height);
                const auto pb = text::parseMarkup((*b2)->utf8, (*b2)->height);
                CAPTURE((*a2)->utf8);
                CAPTURE((*b2)->utf8);
                REQUIRE(pa.size() == pb.size());
                for (std::size_t k = 0; k < pa.size(); ++k) {
                    CHECK(pa[k].text == pb[k].text);
                    CHECK(pa[k].newline == pb[k].newline);
                    CHECK(pa[k].stacked == pb[k].stacked);
                    CHECK(pa[k].num == pb[k].num);
                    CHECK(pa[k].den == pb[k].den);
                    CHECK(pa[k].attr.bold == pb[k].attr.bold);
                    CHECK(pa[k].attr.italic == pb[k].attr.italic);
                    CHECK(pa[k].attr.color == pb[k].attr.color);
                    CHECK(pa[k].attr.underline == pb[k].attr.underline);
                    CHECK(pa[k].attr.height == doctest::Approx(pb[k].attr.height));
                }
            }
        }
    }
}

}  // namespace tbcad::dxf
