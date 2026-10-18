// How long plotting a big drawing to PDF takes, for model space and for a layout with several
// viewports, and how big the PDF gets. Size: TBCAD_BENCH_N objects (default 200,000). Prints; the
// checks guard generous limits and the results.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "tbcad/export/plot.h"
#include "tbcad/layout/layout.h"

using namespace tbcad;
using namespace tbcad::exportlib;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int benchN() {
    const char* env = std::getenv("TBCAD_BENCH_N");
    return env ? std::atoi(env) : 200000;
}

// A site-plan-like mix: 50% lines, 25% polylines, 15% circles, 5% arcs, 5% short texts.
void fill(Document& doc, int n) {
    auto tx = doc.begin("gen");
    for (int i = 0; i < n; ++i) {
        const double x = (i % 700) * 30.0, y = (i / 700) * 30.0;
        Entity e;
        switch (i % 20) {
            case 0: {
                TextData t;
                t.position = {x, y};
                t.utf8 = "T" + std::to_string(i % 97);
                e.data = std::move(t);
            } break;
            case 1:
                e.data = ArcData{{x, y}, 8, 0.2, 2.5};
                break;
            case 2:
            case 3:
            case 4:
                e.data = CircleData{{x, y}, 5};
                break;
            case 5:
            case 6:
            case 7:
            case 8:
            case 9: {
                PolylineData p;
                for (int k = 0; k < 6; ++k)
                    p.v.push_back({{x + k * 3, y + k % 3}, 0, 0, 0});
                e.data = std::move(p);
                break;
            }
            default:
                e.data = LineData{{x, y}, {x + 20, y + 10}};
        }
        e.common.color = Color::index(static_cast<std::uint8_t>(1 + (i / 40) % 7));
        (void)tx.add(std::move(e));
    }
    tx.commit();
}

}  // namespace

TEST_CASE("perf: plotting a big drawing to PDF, model space and layout") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int n = benchN();
    Document doc;
    fill(doc, n);
    PlotSettings s;

    auto t = std::chrono::steady_clock::now();
    auto sheet = buildSheet(doc, s);
    const double buildMs = ms(t);
    REQUIRE(sheet.ok());
    t = std::chrono::steady_clock::now();
    auto pdf = pdfBytes(sheet.value(), true);
    const double pdfMs = ms(t);
    REQUIRE(pdf.ok());
    t = std::chrono::steady_clock::now();
    auto raw = pdfBytes(sheet.value(), false);
    std::printf("  (without deflate: %.0f ms, %.1f MB)\n", ms(t),
                static_cast<double>(raw.value().size()) / 1e6);
    std::printf("model space, %d objects: build %.0f ms (%zu primitives), PDF %.0f ms, %.1f MB\n",
                n, buildMs, sheet.value().prims.size(), pdfMs,
                static_cast<double>(pdf.value().size()) / 1e6);
    CHECK(sheet.value().entityCount == static_cast<std::size_t>(n));
    CHECK(buildMs + pdfMs < 30000);

    {
        auto tx = doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Sheet1", "A1", true).ok());
        BBox2 ext{{0, 0}, {700 * 30.0, (n / 700 + 1) * 30.0}};
        const double w = ext.max.x - ext.min.x, h = ext.max.y - ext.min.y;
        const double sc = std::max(w, h) / 400.0;
        for (int i = 0; i < 4; ++i) {
            const double x0 = 20 + (i % 2) * 400, y0 = 20 + (i / 2) * 280;
            REQUIRE(layout::addViewport(tx, "Sheet1", {{x0, y0}, {x0 + 380, y0 + 260}},
                                        {ext.min.x + w * (0.25 + 0.5 * (i % 2)),
                                         ext.min.y + h * (0.25 + 0.5 * (i / 2))},
                                        sc * 2)
                        .ok());
        }
        tx.commit();
    }
    t = std::chrono::steady_clock::now();
    auto lsheet = layout::buildLayoutSheet(doc, "Sheet1", s);
    const double lbuildMs = ms(t);
    REQUIRE(lsheet.ok());
    t = std::chrono::steady_clock::now();
    auto lpdf = pdfBytes(lsheet.value(), true);
    const double lpdfMs = ms(t);
    REQUIRE(lpdf.ok());
    std::printf("layout, 4 viewports: build %.0f ms (%zu primitives), PDF %.0f ms, %.1f MB\n",
                lbuildMs, lsheet.value().prims.size(), lpdfMs,
                static_cast<double>(lpdf.value().size()) / 1e6);
    CHECK(lbuildMs + lpdfMs < 30000);
}
